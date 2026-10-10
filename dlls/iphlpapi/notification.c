/*
 * IP interface, address and route change notifications
 *
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#define IPHLPAPI_DLL_LINKAGE
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winsock2.h"
#include "ws2ipdef.h"
#include "iphlpapi.h"
#include "netioapi.h"
#include "netiodef.h"
#include "wine/list.h"
#include "wine/nsi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

enum notification_kind
{
    NOTIFY_INTERFACE,
    NOTIFY_ADDRESS,
    NOTIFY_ROUTE,
};

union notification_callback
{
    PIPINTERFACE_CHANGE_CALLBACK interface;
    PUNICAST_IPADDRESS_CHANGE_CALLBACK address;
    PIPFORWARD_CHANGE_CALLBACK route;
};

struct notification;

struct notification_source
{
    struct notification *notification;
    ADDRESS_FAMILY family;
    OVERLAPPED overlapped;
    struct nsiproxy_request_notification request;
    BOOL pending;
    BOOL dirty;
    BOOL disabled;
    DWORD retry_delay;
    ULONGLONG retry_deadline;
    void *table;
};

struct notification
{
    struct list entry;
    HANDLE handle;
    CRITICAL_SECTION cs;
    enum notification_kind kind;
    union notification_callback callback;
    void *context;
    BOOL cancelled;
    HANDLE device;
    PTP_IO io;
    PTP_TIMER retry;
    ULONGLONG timer_deadline;
    unsigned int source_count;
    struct notification_source sources[2];
};

static SRWLOCK notification_lock = SRWLOCK_INIT;
static struct list notifications = LIST_INIT(notifications);
static ULONG_PTR next_handle;

union notification_row
{
    MIB_IPINTERFACE_ROW interface;
    MIB_UNICASTIPADDRESS_ROW address;
    MIB_IPFORWARD_ROW2 route;
};

static int compare_uint64( ULONGLONG a, ULONGLONG b )
{
    return a < b ? -1 : a > b;
}

static int compare_address( const SOCKADDR_INET *a, const SOCKADDR_INET *b )
{
    if (a->si_family != b->si_family) return a->si_family - b->si_family;
    if (a->si_family == AF_INET)
        return memcmp( &a->Ipv4.sin_addr, &b->Ipv4.sin_addr, sizeof(a->Ipv4.sin_addr) );
    return memcmp( &a->Ipv6.sin6_addr, &b->Ipv6.sin6_addr, sizeof(a->Ipv6.sin6_addr) );
}

static int __cdecl compare_interface( const void *a, const void *b )
{
    const MIB_IPINTERFACE_ROW *left = a, *right = b;

    return compare_uint64( left->InterfaceLuid.Value, right->InterfaceLuid.Value );
}

static int __cdecl compare_unicast( const void *a, const void *b )
{
    const MIB_UNICASTIPADDRESS_ROW *left = a, *right = b;
    int ret;

    if ((ret = compare_uint64( left->InterfaceLuid.Value, right->InterfaceLuid.Value ))) return ret;
    return compare_address( &left->Address, &right->Address );
}

static int __cdecl compare_route( const void *a, const void *b )
{
    const MIB_IPFORWARD_ROW2 *left = a, *right = b;
    int ret;

    if ((ret = compare_uint64( left->InterfaceLuid.Value, right->InterfaceLuid.Value ))) return ret;
    if ((ret = compare_address( &left->DestinationPrefix.Prefix, &right->DestinationPrefix.Prefix ))) return ret;
    if (left->DestinationPrefix.PrefixLength != right->DestinationPrefix.PrefixLength)
        return left->DestinationPrefix.PrefixLength - right->DestinationPrefix.PrefixLength;
    return compare_address( &left->NextHop, &right->NextHop );
}

struct notification_table
{
    DWORD nsi_table;
    size_t offset, row_size;
    int (__cdecl *compare)( const void *, const void * );
};

static const struct notification_table notification_tables[] =
{
    { NSI_IP_INTERFACE_TABLE, offsetof(MIB_IPINTERFACE_TABLE, Table), sizeof(MIB_IPINTERFACE_ROW), compare_interface },
    { NSI_IP_UNICAST_TABLE, offsetof(MIB_UNICASTIPADDRESS_TABLE, Table), sizeof(MIB_UNICASTIPADDRESS_ROW), compare_unicast },
    { NSI_IP_FORWARD_TABLE, offsetof(MIB_IPFORWARD_TABLE2, Table), sizeof(MIB_IPFORWARD_ROW2), compare_route },
};

static DWORD get_table( struct notification_source *source, void **table )
{
    *table = NULL;
    switch (source->notification->kind)
    {
    case NOTIFY_INTERFACE: return GetIpInterfaceTable( source->family, (MIB_IPINTERFACE_TABLE **)table );
    case NOTIFY_ADDRESS: return GetUnicastIpAddressTable( source->family, (MIB_UNICASTIPADDRESS_TABLE **)table );
    case NOTIFY_ROUTE: return GetIpForwardTable2( source->family, (MIB_IPFORWARD_TABLE2 **)table );
    }
    return ERROR_INVALID_PARAMETER;
}

static BOOL row_changed( enum notification_kind kind, const void *a, const void *b )
{
    switch (kind)
    {
    case NOTIFY_INTERFACE:
        /* GetIpInterfaceTable zeroes its rows, including padding. */
        return memcmp( a, b, sizeof(MIB_IPINTERFACE_ROW) ) != 0;
    case NOTIFY_ADDRESS:
    {
        const MIB_UNICASTIPADDRESS_ROW *left = a, *right = b;

        return left->InterfaceIndex != right->InterfaceIndex || left->PrefixOrigin != right->PrefixOrigin ||
               left->SuffixOrigin != right->SuffixOrigin || left->ValidLifetime != right->ValidLifetime ||
               left->PreferredLifetime != right->PreferredLifetime ||
               left->OnLinkPrefixLength != right->OnLinkPrefixLength || left->SkipAsSource != right->SkipAsSource ||
               left->DadState != right->DadState || left->ScopeId.Value != right->ScopeId.Value;
    }
    case NOTIFY_ROUTE:
    {
        const MIB_IPFORWARD_ROW2 *left = a, *right = b;

        /* Age increases without any change to the route. */
        return left->InterfaceIndex != right->InterfaceIndex || left->SitePrefixLength != right->SitePrefixLength ||
               left->ValidLifetime != right->ValidLifetime || left->PreferredLifetime != right->PreferredLifetime ||
               left->Metric != right->Metric || left->Protocol != right->Protocol || left->Loopback != right->Loopback ||
               left->AutoconfigureAddress != right->AutoconfigureAddress || left->Publish != right->Publish ||
               left->Immortal != right->Immortal || left->Origin != right->Origin;
    }
    }
    return FALSE;
}

static void notify_row( struct notification *notification, const void *row, MIB_NOTIFICATION_TYPE type )
{
    union notification_row copy;

    /* Do not expose the snapshot to writes by the application. */
    if (row) memcpy( &copy, row, notification_tables[notification->kind].row_size );
    switch (notification->kind)
    {
    case NOTIFY_INTERFACE:
        notification->callback.interface( notification->context, row ? &copy.interface : NULL, type );
        break;
    case NOTIFY_ADDRESS:
        notification->callback.address( notification->context, row ? &copy.address : NULL, type );
        break;
    case NOTIFY_ROUTE:
        notification->callback.route( notification->context, row ? &copy.route : NULL, type );
        break;
    }
}

static void swap_rows( void *a, void *b, size_t size )
{
    union notification_row tmp;

    if (a == b) return;
    memcpy( &tmp, a, size );
    memcpy( a, b, size );
    memcpy( b, &tmp, size );
}

static void notify_changed_rows( struct notification *notification, char *old_rows, ULONG old_count,
                                 char *new_rows, ULONG new_count, size_t row_size )
{
    ULONG i, j, matched = 0;

    /* Linux can expose multiple routes with the same Windows identity. Match
     * unchanged rows first, independently of the order within an equal-key group. */
    for (i = 0; i < old_count; ++i)
    {
        for (j = matched; j < new_count; ++j)
        {
            if (row_changed( notification->kind, old_rows + i * row_size, new_rows + j * row_size )) continue;
            swap_rows( old_rows + i * row_size, old_rows + matched * row_size, row_size );
            swap_rows( new_rows + j * row_size, new_rows + matched * row_size, row_size );
            ++matched;
            break;
        }
    }
    for (i = matched; i < old_count && i < new_count; ++i)
        notify_row( notification, new_rows + i * row_size, MibParameterNotification );
    for (j = i; j < old_count; ++j)
        notify_row( notification, old_rows + j * row_size, MibDeleteInstance );
    for (j = i; j < new_count; ++j)
        notify_row( notification, new_rows + j * row_size, MibAddInstance );
}

static DWORD refresh_table( struct notification_source *source )
{
    struct notification *notification = source->notification;
    const struct notification_table *description = &notification_tables[notification->kind];
    ULONG i = 0, j = 0, old_count, new_count, old_end, new_end;
    char *old_rows, *new_rows;
    void *table;
    DWORD err;
    int ret;

    if ((err = get_table( source, &table )))
    {
        FreeMibTable( table );
        return err;
    }
    new_count = *(ULONG *)table;
    new_rows = (char *)table + description->offset;
    qsort( (void *)new_rows, new_count, description->row_size, description->compare );
    if (source->table)
    {
        old_count = *(ULONG *)source->table;
        old_rows = (char *)source->table + description->offset;
        while (i < old_count || j < new_count)
        {
            const void *old_row = old_rows + i * description->row_size;
            const void *new_row = new_rows + j * description->row_size;

            ret = i == old_count ? 1 : j == new_count ? -1 : description->compare( old_row, new_row );
            if (ret < 0)
            {
                notify_row( notification, old_row, MibDeleteInstance );
                ++i;
            }
            else if (ret > 0)
            {
                notify_row( notification, new_row, MibAddInstance );
                ++j;
            }
            else
            {
                for (old_end = i + 1; old_end < old_count; ++old_end)
                    if (description->compare( old_row, old_rows + old_end * description->row_size )) break;
                for (new_end = j + 1; new_end < new_count; ++new_end)
                    if (description->compare( new_row, new_rows + new_end * description->row_size )) break;
                notify_changed_rows( notification, old_rows + i * description->row_size, old_end - i,
                                     new_rows + j * description->row_size, new_end - j, description->row_size );
                i = old_end;
                j = new_end;
            }
        }
        FreeMibTable( source->table );
    }
    source->table = table;
    source->dirty = FALSE;
    return ERROR_SUCCESS;
}

static DWORD arm_source( struct notification_source *source )
{
    struct notification *notification = source->notification;
    const NPI_MODULEID *module = source->family == AF_INET ? &NPI_MS_IPV4_MODULEID : &NPI_MS_IPV6_MODULEID;
    DWORD err, bytes;

    source->request.module = *module;
    source->request.table = notification_tables[notification->kind].nsi_table;
    /* Completion-port requests with no event survive the issuing thread. */
    StartThreadpoolIo( notification->io );
    if (!DeviceIoControl( notification->device, IOCTL_NSIPROXY_WINE_CHANGE_NOTIFICATION,
                         &source->request, sizeof(source->request), NULL, 0, &bytes, &source->overlapped ))
    {
        if ((err = GetLastError()) != ERROR_IO_PENDING)
        {
            CancelThreadpoolIo( notification->io );
            return err;
        }
    }
    source->pending = TRUE;
    return ERROR_SUCCESS;
}

static BOOL retryable_error( DWORD err )
{
    return err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY || err == ERROR_NO_SYSTEM_RESOURCES ||
           err == ERROR_MORE_DATA || err == ERROR_RETRY;
}

static void update_retry_timer( struct notification *notification )
{
    ULONGLONG deadline = 0, now;
    LARGE_INTEGER delay;
    unsigned int i;

    if (!notification->cancelled)
        for (i = 0; i < notification->source_count; ++i)
        {
            struct notification_source *source = &notification->sources[i];

            if (!source->retry_deadline) continue;
            if (!deadline || source->retry_deadline < deadline) deadline = source->retry_deadline;
        }
    if (deadline == notification->timer_deadline) return;
    notification->timer_deadline = deadline;
    if (!deadline) SetThreadpoolTimer( notification->retry, NULL, 0, 0 );
    else
    {
        now = GetTickCount64();
        delay.QuadPart = -(LONGLONG)(deadline > now ? deadline - now : 1) * 10000;
        SetThreadpoolTimer( notification->retry, (FILETIME *)&delay, 0, 0 );
    }
}

static void schedule_retry( struct notification_source *source )
{
    /* Retry failures indefinitely, but never poll healthy sources or postpone
     * an existing deadline when another network event arrives. */
    if (source->retry_deadline) return;
    source->retry_delay = source->retry_delay ? min( source->retry_delay * 2, 30000 ) : 1000;
    source->retry_deadline = GetTickCount64() + source->retry_delay;
}

static void source_failed( struct notification_source *source, DWORD err )
{
    if (retryable_error( err )) schedule_retry( source );
    else
    {
        WARN( "Disabling network notification for family %u, error %lu.\n", source->family, err );
        source->disabled = TRUE;
        source->retry_deadline = 0;
        source->retry_delay = 0;
    }
}

static void refresh_source( struct notification_source *source )
{
    /* Enumeration errors do not imply a failed subscription. For example,
     * nsiproxy maps getifaddrs and procfs failures to NO_MORE_ITEMS and
     * NOT_SUPPORTED. Retain the snapshot and live watch until a query succeeds. */
    if (refresh_table( source )) schedule_retry( source );
    else
    {
        source->retry_deadline = 0;
        source->retry_delay = 0;
    }
}

static void CALLBACK notification_ready( PTP_CALLBACK_INSTANCE instance, void *context, void *overlapped,
                                         ULONG result, ULONG_PTR bytes, PTP_IO io )
{
    struct notification_source *source = CONTAINING_RECORD( overlapped, struct notification_source, overlapped );
    struct notification *notification = context;
    DWORD err;

    EnterCriticalSection( &notification->cs );
    source->pending = FALSE;
    if (!notification->cancelled && !source->disabled)
    {
        source->dirty = TRUE;
        if (result && result != ERROR_OPERATION_ABORTED)
        {
            source_failed( source, result );
            goto done;
        }
        /* Subscribe before reading the table so a concurrent change remains observable. */
        if ((err = arm_source( source ))) source_failed( source, err );
        else refresh_source( source );
    }
done:
    update_retry_timer( notification );
    LeaveCriticalSection( &notification->cs );
}

static void CALLBACK retry_notification( PTP_CALLBACK_INSTANCE instance, void *context, PTP_TIMER timer )
{
    struct notification *notification = context;
    ULONGLONG now;
    unsigned int i;

    EnterCriticalSection( &notification->cs );
    notification->timer_deadline = 0;
    now = GetTickCount64();
    if (!notification->cancelled)
    {
        for (i = 0; i < notification->source_count; ++i)
        {
            struct notification_source *source = &notification->sources[i];
            DWORD err;

            if (source->disabled || !source->retry_deadline || source->retry_deadline > now) continue;
            source->retry_deadline = 0;
            if (!source->pending)
            {
                if ((err = arm_source( source )))
                {
                    source_failed( source, err );
                    continue;
                }
            }
            if (source->dirty) refresh_source( source );
        }
    }
    update_retry_timer( notification );
    LeaveCriticalSection( &notification->cs );
}

static void destroy_notification( struct notification *notification )
{
    unsigned int i;

    EnterCriticalSection( &notification->cs );
    notification->cancelled = TRUE;
    notification->timer_deadline = 0;
    if (notification->retry) SetThreadpoolTimer( notification->retry, NULL, 0, 0 );
    if (notification->device != INVALID_HANDLE_VALUE) CancelIoEx( notification->device, NULL );
    LeaveCriticalSection( &notification->cs );
    if (notification->retry)
    {
        WaitForThreadpoolTimerCallbacks( notification->retry, TRUE );
        CloseThreadpoolTimer( notification->retry );
    }
    if (notification->io)
    {
        /* Deliver cancelled I/O completions before releasing OVERLAPPED storage. */
        WaitForThreadpoolIoCallbacks( notification->io, FALSE );
        CloseThreadpoolIo( notification->io );
    }
    if (notification->device != INVALID_HANDLE_VALUE) CloseHandle( notification->device );
    for (i = 0; i < notification->source_count; ++i)
    {
        struct notification_source *source = &notification->sources[i];

        FreeMibTable( source->table );
    }
    DeleteCriticalSection( &notification->cs );
    free( notification );
}

static DWORD register_notification( enum notification_kind kind, ADDRESS_FAMILY family,
                                    union notification_callback callback, void *context,
                                    BOOLEAN initial, HANDLE *handle )
{
    const ADDRESS_FAMILY families[] = { AF_INET, AF_INET6 };
    struct notification *notification;
    unsigned int i;
    DWORD err;

    if (handle) *handle = NULL;
    if (!handle || !callback.interface || (family != AF_UNSPEC && family != AF_INET && family != AF_INET6))
        return ERROR_INVALID_PARAMETER;
    if (!(notification = calloc( 1, sizeof(*notification) ))) return ERROR_NOT_ENOUGH_MEMORY;
    InitializeCriticalSection( &notification->cs );
    EnterCriticalSection( &notification->cs );
    notification->device = INVALID_HANDLE_VALUE;
    notification->kind = kind;
    notification->callback = callback;
    notification->context = context;
    if (!(notification->retry = CreateThreadpoolTimer( retry_notification, notification, NULL )))
    {
        err = GetLastError();
        goto failed;
    }
    notification->device = CreateFileW( L"\\\\.\\Nsi", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED, NULL );
    if (notification->device == INVALID_HANDLE_VALUE ||
        !(notification->io = CreateThreadpoolIo( notification->device, notification_ready, notification, NULL )))
    {
        err = GetLastError();
        goto failed;
    }
    for (i = 0; i < ARRAY_SIZE(families); ++i)
    {
        struct notification_source *source;

        if (family != AF_UNSPEC && family != families[i]) continue;
        source = &notification->sources[notification->source_count++];
        source->notification = notification;
        source->family = families[i];
        if ((err = arm_source( source )) || (err = refresh_table( source ))) goto failed;
    }

    if (initial) notify_row( notification, NULL, MibInitialNotification );
    AcquireSRWLockExclusive( &notification_lock );
    /* These are registration cookies, not kernel handles or dereferenceable caller pointers. */
    notification->handle = (HANDLE)(ULONG_PTR)(0x80000001u | ((++next_handle & 0x1fffffff) << 2));
    list_add_tail( &notifications, &notification->entry );
    *handle = notification->handle;
    ReleaseSRWLockExclusive( &notification_lock );
    LeaveCriticalSection( &notification->cs );
    return ERROR_SUCCESS;

failed:
    notification->cancelled = TRUE;
    LeaveCriticalSection( &notification->cs );
    destroy_notification( notification );
    return err;
}

DWORD WINAPI NotifyIpInterfaceChange( ADDRESS_FAMILY family, PIPINTERFACE_CHANGE_CALLBACK callback,
                                      void *context, BOOLEAN initial, HANDLE *handle )
{
    union notification_callback cb;
    cb.interface = callback;
    return register_notification( NOTIFY_INTERFACE, family, cb, context, initial, handle );
}

DWORD WINAPI NotifyUnicastIpAddressChange( ADDRESS_FAMILY family, PUNICAST_IPADDRESS_CHANGE_CALLBACK callback,
                                          void *context, BOOLEAN initial, HANDLE *handle )
{
    union notification_callback cb;
    cb.address = callback;
    return register_notification( NOTIFY_ADDRESS, family, cb, context, initial, handle );
}

DWORD WINAPI NotifyRouteChange2( ADDRESS_FAMILY family, PIPFORWARD_CHANGE_CALLBACK callback,
                                void *context, BOOLEAN initial, HANDLE *handle )
{
    union notification_callback cb;
    cb.route = callback;
    return register_notification( NOTIFY_ROUTE, family, cb, context, initial, handle );
}

DWORD WINAPI CancelMibChangeNotify2( HANDLE handle )
{
    struct notification *notification, *found = NULL;

    AcquireSRWLockExclusive( &notification_lock );
    LIST_FOR_EACH_ENTRY( notification, &notifications, struct notification, entry )
    {
        if (notification->handle != handle) continue;
        list_remove( &notification->entry );
        found = notification;
        break;
    }
    ReleaseSRWLockExclusive( &notification_lock );
    if (!found) return ERROR_INVALID_PARAMETER;
    destroy_notification( found );
    return ERROR_SUCCESS;
}
