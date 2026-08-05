/* WinRT Windows.Globalization.Calendar implementation
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

#include <stdint.h>

#include "private.h"
#include "winreg.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

#define TICKS_PER_MSEC 10000
#define TICKS_PER_SEC  10000000
#define TICKS_PER_MIN  (60 * (INT64)TICKS_PER_SEC)
#define TICKS_PER_HOUR (60 * TICKS_PER_MIN)
#define TICKS_PER_DAY  (24 * TICKS_PER_HOUR)

#define CALENDAR_MIN_YEAR 1
#define CALENDAR_MAX_YEAR 9999
#define CALENDAR_EPOCH_DAYS 584388

static const WCHAR gregorian_calendarW[] = L"GregorianCalendar";
static const WCHAR clock_12hourW[] = L"12HourClock";
static const WCHAR clock_24hourW[] = L"24HourClock";
static const WCHAR numeral_latinW[] = L"Latn";
static const WCHAR era_adW[] = L"A.D.";

struct numeral_system
{
    const WCHAR *name;
    UINT32 zero;
};

static const struct numeral_system numeral_systems[] =
{
    {L"Arab", 0x0660}, {L"ArabExt", 0x06f0}, {L"Bali", 0x1b50},
    {L"Beng", 0x09e6}, {L"Brah", 0x11066}, {L"Cham", 0xaa50},
    {L"Deva", 0x0966}, {L"FullWide", 0xff10}, {L"Gujr", 0x0ae6},
    {L"Guru", 0x0a66}, {L"HaniDec", 0}, {L"Java", 0xa9d0},
    {L"Kali", 0xa900}, {L"Khmr", 0x17e0}, {L"Knda", 0x0ce6},
    {L"Lana", 0x1a80}, {L"LanaTham", 0x1a90}, {L"Laoo", 0x0ed0},
    {L"Latn", 0x0030}, {L"Lepc", 0x1c40}, {L"Limb", 0x1946},
    {L"MathBold", 0x1d7ce}, {L"MathDbl", 0x1d7d8},
    {L"MathMono", 0x1d7f6}, {L"MathSanb", 0x1d7ec},
    {L"MathSans", 0x1d7e2}, {L"Mlym", 0x0d66}, {L"Mong", 0x1810},
    {L"Mtei", 0xabc0}, {L"Mymr", 0x1040}, {L"MymrShan", 0x1090},
    {L"Nkoo", 0x07c0}, {L"Olck", 0x1c50}, {L"Orya", 0x0b66},
    {L"Osma", 0x104a0}, {L"Saur", 0xa8d0}, {L"Sund", 0x1bb0},
    {L"Talu", 0x19d0}, {L"TamlDec", 0x0be6}, {L"Telu", 0x0c66},
    {L"Thai", 0x0e50}, {L"Tibt", 0x0f20}, {L"Vaii", 0xa620},
    {L"ZmthBold", 0x1d7ce}, {L"ZmthDbl", 0x1d7d8},
    {L"ZmthMono", 0x1d7f6}, {L"ZmthSanb", 0x1d7ec},
    {L"ZmthSans", 0x1d7e2},
};

static const UINT32 hani_digits[] =
{
    0x3007, 0x4e00, 0x4e8c, 0x4e09, 0x56db,
    0x4e94, 0x516d, 0x4e03, 0x516b, 0x4e5d,
};

struct calendar
{
    ICalendar ICalendar_iface;
    ITimeZoneOnCalendar ITimeZoneOnCalendar_iface;
    LONG ref;

    INT64 datetime;
    DYNAMIC_TIME_ZONE_INFORMATION timezone;
    WCHAR timezone_id[256];
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    WCHAR numeral_system[32];
    BOOL clock_12hour;

    HSTRING *languages;
    UINT32 languages_count;
};

static INT64 calendar_floor_div( INT64 value, INT64 divisor )
{
    INT64 quotient = value / divisor;

    if (value % divisor < 0) quotient--;
    return quotient;
}

static INT64 calendar_floor_mod( INT64 value, INT64 divisor )
{
    INT64 remainder = value % divisor;

    return remainder < 0 ? remainder + divisor : remainder;
}

static INT64 calendar_days_before_year( INT32 year )
{
    INT64 value = year - 1;

    return value * 365 + calendar_floor_div( value, 4 ) -
           calendar_floor_div( value, 100 ) + calendar_floor_div( value, 400 );
}

static WORD days_in_month( WORD year, WORD month )
{
    static const WORD days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 1 || month > 12) return 0;
    if (month == 2 && !(year % 4) && ((year % 100) || !(year % 400))) return 29;
    return days[month - 1];
}

static BOOL calendar_systemtime_to_ticks( const SYSTEMTIME *time, INT64 subsecond, INT64 *ticks )
{
    INT64 days, value;
    WORD limit;

    if (time->wYear < CALENDAR_MIN_YEAR || time->wYear > CALENDAR_MAX_YEAR + 1 ||
        time->wMonth < 1 || time->wMonth > 12 ||
        time->wHour > 23 || time->wMinute > 59 || time->wSecond > 59 ||
        subsecond < 0 || subsecond >= TICKS_PER_SEC ||
        !(limit = days_in_month( time->wYear, time->wMonth )) ||
        time->wDay < 1 || time->wDay > limit)
        return FALSE;

    days = calendar_days_before_year( time->wYear );
    for (WORD month = 1; month < time->wMonth; month++) days += days_in_month( time->wYear, month );
    days += time->wDay - 1 - CALENDAR_EPOCH_DAYS;
    value = days * TICKS_PER_DAY;
    value += time->wHour * TICKS_PER_HOUR + time->wMinute * TICKS_PER_MIN;
    value += time->wSecond * (INT64)TICKS_PER_SEC + subsecond;
    *ticks = value;
    return TRUE;
}

static BOOL calendar_ticks_to_systemtime( INT64 ticks, SYSTEMTIME *time, INT64 *subsecond )
{
    INT64 days = calendar_floor_div( ticks, TICKS_PER_DAY );
    INT64 ordinal = days + CALENDAR_EPOCH_DAYS;
    INT64 remainder = ticks - days * TICKS_PER_DAY;
    INT32 low = 0, high = CALENDAR_MAX_YEAR + 2, year;
    WORD month;

    if (ordinal < calendar_days_before_year( 0 ) ||
        ordinal >= calendar_days_before_year( CALENDAR_MAX_YEAR + 2 ))
        return FALSE;

    while (low + 1 < high)
    {
        INT32 mid = low + (high - low) / 2;

        if (calendar_days_before_year( mid ) <= ordinal) low = mid;
        else high = mid;
    }
    year = low;
    ordinal -= calendar_days_before_year( year );
    for (month = 1; ordinal >= days_in_month( year, month ); month++)
        ordinal -= days_in_month( year, month );

    memset( time, 0, sizeof(*time) );
    time->wYear = year;
    time->wMonth = month;
    time->wDay = ordinal + 1;
    time->wDayOfWeek = calendar_floor_mod( days + 1, 7 );
    time->wHour = remainder / TICKS_PER_HOUR;
    remainder %= TICKS_PER_HOUR;
    time->wMinute = remainder / TICKS_PER_MIN;
    remainder %= TICKS_PER_MIN;
    time->wSecond = remainder / TICKS_PER_SEC;
    time->wMilliseconds = remainder % TICKS_PER_SEC / TICKS_PER_MSEC;
    if (subsecond) *subsecond = remainder % TICKS_PER_SEC;
    return TRUE;
}

static BOOL calendar_add_ticks( INT64 left, INT64 right, INT64 *result )
{
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL calendar_multiply_ticks( INT32 value, INT64 unit, INT64 *result )
{
    if ((value > 0 && value > INT64_MAX / unit) ||
        (value < 0 && value < INT64_MIN / unit))
        return FALSE;
    *result = value * unit;
    return TRUE;
}

static INT64 calendar_filetime_to_ticks( const FILETIME *time )
{
    return ((UINT64)time->dwHighDateTime << 32) | time->dwLowDateTime;
}

static HRESULT calendar_create_string( const WCHAR *str, HSTRING *out )
{
    return WindowsCreateString( str, wcslen( str ), out );
}

static const struct numeral_system *calendar_find_numeral_system( const WCHAR *name )
{
    UINT32 i;

    if (!name) return NULL;
    for (i = 0; i < ARRAY_SIZE(numeral_systems); i++)
        if (!wcsicmp( name, numeral_systems[i].name )) return &numeral_systems[i];
    return NULL;
}

static UINT32 calendar_numeral_codepoint( const struct numeral_system *system, UINT32 digit )
{
    if (!system->zero) return hani_digits[digit];
    return system->zero + digit;
}

static UINT32 calendar_append_codepoint( WCHAR *buffer, UINT32 pos, UINT32 codepoint )
{
    if (codepoint <= 0xffff)
        buffer[pos++] = codepoint;
    else
    {
        codepoint -= 0x10000;
        buffer[pos++] = 0xd800 + (codepoint >> 10);
        buffer[pos++] = 0xdc00 + (codepoint & 0x3ff);
    }
    return pos;
}

static HRESULT calendar_format_number( struct calendar *impl, INT64 value, INT32 min_digits, HSTRING *out )
{
    const struct numeral_system *system;
    UINT8 reversed[20];
    WCHAR *buffer;
    UINT64 magnitude;
    UINT32 count = 0, digits, length, pos = 0, i;
    BOOL negative = value < 0;
    HRESULT hr;

    if (!out || min_digits < 0) return E_INVALIDARG;
    if (!(system = calendar_find_numeral_system( impl->numeral_system ))) return E_INVALIDARG;

    magnitude = negative ? (UINT64)(-(value + 1)) + 1 : value;
    do
    {
        reversed[count++] = magnitude % 10;
        magnitude /= 10;
    } while (magnitude);
    digits = max( count, (UINT32)min_digits );
    if (digits > (~(SIZE_T)0 / sizeof(*buffer) - negative) / 2) return E_OUTOFMEMORY;
    if (!(buffer = malloc( (digits * 2 + negative) * sizeof(*buffer) ))) return E_OUTOFMEMORY;

    if (negative) buffer[pos++] = '-';
    for (i = count; i < digits; i++)
        pos = calendar_append_codepoint( buffer, pos, calendar_numeral_codepoint( system, 0 ) );
    while (count--)
        pos = calendar_append_codepoint( buffer, pos,
                                         calendar_numeral_codepoint( system, reversed[count] ) );
    length = pos;
    hr = WindowsCreateString( buffer, length, out );
    free( buffer );
    return hr;
}

static HRESULT calendar_locale_string( struct calendar *impl, LCTYPE type, HSTRING *out )
{
    WCHAR buffer[128];

    if (!out) return E_INVALIDARG;
    if (!GetLocaleInfoEx( impl->locale, type, buffer, ARRAY_SIZE(buffer) )) return E_FAIL;
    return calendar_create_string( buffer, out );
}

static BOOL calendar_timezone_for_year( struct calendar *impl, WORD year, TIME_ZONE_INFORMATION *info )
{
    year = max( CALENDAR_MIN_YEAR, min( CALENDAR_MAX_YEAR, year ) );
    return GetTimeZoneInformationForYear( year, &impl->timezone, info );
}

static int calendar_compare_transition( const SYSTEMTIME *time, const SYSTEMTIME *transition )
{
    SYSTEMTIME first = {.wYear = time->wYear, .wMonth = transition->wMonth, .wDay = 1};
    WORD day;

    if (transition->wYear && time->wYear != transition->wYear)
        return time->wYear < transition->wYear ? -1 : 1;
    if (time->wMonth != transition->wMonth)
        return time->wMonth < transition->wMonth ? -1 : 1;

    if (transition->wYear)
    {
        day = transition->wDay;
    }
    else
    {
        INT64 ticks;

        if (!calendar_systemtime_to_ticks( &first, 0, &ticks ) ||
            !calendar_ticks_to_systemtime( ticks, &first, NULL ))
            return 0;
        day = 1 + (transition->wDayOfWeek + 7 - first.wDayOfWeek) % 7 +
              7 * (transition->wDay - 1);
        if (day > days_in_month( time->wYear, time->wMonth )) day -= 7;
    }

    if (time->wDay != day) return time->wDay < day ? -1 : 1;
    if (time->wHour != transition->wHour) return time->wHour < transition->wHour ? -1 : 1;
    if (time->wMinute != transition->wMinute) return time->wMinute < transition->wMinute ? -1 : 1;
    if (time->wSecond != transition->wSecond) return time->wSecond < transition->wSecond ? -1 : 1;
    return time->wMilliseconds - transition->wMilliseconds;
}

static DWORD calendar_timezone_id( const TIME_ZONE_INFORMATION *info, INT64 ticks, BOOL is_local )
{
    BOOL before_standard, after_daylight;
    SYSTEMTIME time;
    INT64 adjusted;
    WORD year;

    if (!info->DaylightDate.wMonth) return TIME_ZONE_ID_UNKNOWN;
    if (!info->StandardDate.wMonth ||
        (!info->StandardDate.wYear &&
         (info->StandardDate.wDay < 1 || info->StandardDate.wDay > 5 ||
          info->DaylightDate.wDay < 1 || info->DaylightDate.wDay > 5)))
        return TIME_ZONE_ID_INVALID;

    adjusted = ticks;
    if (!is_local && !calendar_add_ticks( adjusted, -(INT64)info->Bias * TICKS_PER_MIN, &adjusted ))
        return TIME_ZONE_ID_INVALID;
    if (!calendar_ticks_to_systemtime( adjusted, &time, NULL )) return TIME_ZONE_ID_INVALID;
    year = time.wYear;

    if (!is_local &&
        (!calendar_add_ticks( adjusted, -(INT64)info->DaylightBias * TICKS_PER_MIN, &ticks ) ||
         !calendar_ticks_to_systemtime( ticks, &time, NULL )))
        return TIME_ZONE_ID_INVALID;
    before_standard = time.wYear == year ? calendar_compare_transition( &time, &info->StandardDate ) < 0
                                         : time.wYear < year;

    if (!is_local &&
        (!calendar_add_ticks( adjusted, -(INT64)info->StandardBias * TICKS_PER_MIN, &ticks ) ||
         !calendar_ticks_to_systemtime( ticks, &time, NULL )))
        return TIME_ZONE_ID_INVALID;
    after_daylight = time.wYear == year ? calendar_compare_transition( &time, &info->DaylightDate ) >= 0
                                        : time.wYear > year;

    if (info->DaylightDate.wMonth < info->StandardDate.wMonth)
        return before_standard && after_daylight ? TIME_ZONE_ID_DAYLIGHT : TIME_ZONE_ID_STANDARD;
    return before_standard || after_daylight ? TIME_ZONE_ID_DAYLIGHT : TIME_ZONE_ID_STANDARD;
}

static INT64 calendar_timezone_bias( const TIME_ZONE_INFORMATION *info, DWORD id )
{
    if (id == TIME_ZONE_ID_DAYLIGHT) return (INT64)info->Bias + info->DaylightBias;
    if (id == TIME_ZONE_ID_STANDARD) return (INT64)info->Bias + info->StandardBias;
    return info->Bias;
}

static BOOL calendar_get_local( struct calendar *impl, SYSTEMTIME *local, INT64 *subsecond )
{
    TIME_ZONE_INFORMATION info;
    SYSTEMTIME utc;
    INT64 local_ticks;
    DWORD id;

    if (!calendar_ticks_to_systemtime( impl->datetime, &utc, NULL )) return FALSE;
    if (!calendar_timezone_for_year( impl, utc.wYear, &info )) return FALSE;
    if ((id = calendar_timezone_id( &info, impl->datetime, FALSE )) == TIME_ZONE_ID_INVALID)
        return FALSE;
    if (!calendar_add_ticks( impl->datetime, -calendar_timezone_bias( &info, id ) * TICKS_PER_MIN,
                             &local_ticks ) || !calendar_ticks_to_systemtime( local_ticks, local, NULL ))
        return FALSE;

    if (local->wYear != utc.wYear)
    {
        if (!calendar_timezone_for_year( impl, local->wYear, &info )) return FALSE;
        if ((id = calendar_timezone_id( &info, impl->datetime, FALSE )) == TIME_ZONE_ID_INVALID)
            return FALSE;
        if (!calendar_add_ticks( impl->datetime, -calendar_timezone_bias( &info, id ) * TICKS_PER_MIN,
                                 &local_ticks ) || !calendar_ticks_to_systemtime( local_ticks, local, NULL ))
            return FALSE;
    }

    if (local->wYear < CALENDAR_MIN_YEAR || local->wYear > CALENDAR_MAX_YEAR) return FALSE;
    if (subsecond) *subsecond = calendar_floor_mod( impl->datetime, TICKS_PER_SEC );
    return TRUE;
}

static BOOL calendar_set_local( struct calendar *impl, const SYSTEMTIME *local, INT64 subsecond )
{
    TIME_ZONE_INFORMATION info;
    SYSTEMTIME adjusted = *local;
    INT64 local_ticks, utc_ticks;
    DWORD id;

    if (adjusted.wYear < CALENDAR_MIN_YEAR || adjusted.wYear > CALENDAR_MAX_YEAR) return FALSE;
    adjusted.wMilliseconds = 0;
    if (!calendar_timezone_for_year( impl, adjusted.wYear, &info )) return FALSE;
    if (!calendar_systemtime_to_ticks( &adjusted, subsecond, &local_ticks )) return FALSE;
    if ((id = calendar_timezone_id( &info, local_ticks, TRUE )) == TIME_ZONE_ID_INVALID) return FALSE;
    if (!calendar_add_ticks( local_ticks, calendar_timezone_bias( &info, id ) * TICKS_PER_MIN,
                             &utc_ticks ))
        return FALSE;
    impl->datetime = utc_ticks;
    return TRUE;
}

static HRESULT calendar_add_local_ticks( struct calendar *impl, INT64 ticks )
{
    SYSTEMTIME local;
    INT64 subsecond, value;

    if (!calendar_get_local( impl, &local, &subsecond )) return E_FAIL;
    local.wMilliseconds = 0;
    if (!calendar_systemtime_to_ticks( &local, subsecond, &value ) ||
        !calendar_add_ticks( value, ticks, &value ) ||
        !calendar_ticks_to_systemtime( value, &local, &subsecond ) ||
        local.wYear < CALENDAR_MIN_YEAR || local.wYear > CALENDAR_MAX_YEAR)
        return E_INVALIDARG;

    return calendar_set_local( impl, &local, subsecond ) ? S_OK : E_FAIL;
}

static HRESULT calendar_add_months( struct calendar *impl, INT64 months )
{
    SYSTEMTIME local;
    INT64 subsecond, total;
    WORD limit;

    if (!calendar_get_local( impl, &local, &subsecond )) return E_FAIL;

    total = (INT64)local.wYear * 12 + local.wMonth - 1 + months;
    if (total < CALENDAR_MIN_YEAR * 12 || total >= (CALENDAR_MAX_YEAR + 1) * 12) return E_INVALIDARG;

    local.wYear = total / 12;
    local.wMonth = total % 12 + 1;
    if ((limit = days_in_month( local.wYear, local.wMonth )) && local.wDay > limit) local.wDay = limit;

    return calendar_set_local( impl, &local, subsecond ) ? S_OK : E_FAIL;
}

static HRESULT calendar_set_field( struct calendar *impl, size_t offset, WORD value )
{
    SYSTEMTIME local;
    INT64 subsecond;
    WORD limit;

    if (!calendar_get_local( impl, &local, &subsecond )) return E_FAIL;
    *(WORD *)((char *)&local + offset) = value;
    if ((limit = days_in_month( local.wYear, local.wMonth )) && local.wDay > limit) local.wDay = limit;
    return calendar_set_local( impl, &local, subsecond ) ? S_OK : E_INVALIDARG;
}

static HRESULT calendar_get_hour_24( struct calendar *impl, WORD *hour )
{
    SYSTEMTIME local;

    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *hour = local.wHour;
    return S_OK;
}

static void calendar_reset_languages( struct calendar *impl )
{
    UINT32 i;

    for (i = 0; i < impl->languages_count; i++) WindowsDeleteString( impl->languages[i] );
    free( impl->languages );
    impl->languages = NULL;
    impl->languages_count = 0;
}

static HRESULT calendar_copy_languages( struct calendar *dst, const struct calendar *src )
{
    UINT32 i;

    if (!src->languages_count) return S_OK;
    if (!(dst->languages = calloc( src->languages_count, sizeof(*dst->languages) ))) return E_OUTOFMEMORY;

    for (i = 0; i < src->languages_count; i++)
    {
        if (FAILED(WindowsDuplicateString( src->languages[i], &dst->languages[i] )))
        {
            dst->languages_count = i;
            calendar_reset_languages( dst );
            return E_OUTOFMEMORY;
        }
    }
    dst->languages_count = src->languages_count;
    return S_OK;
}

static HRESULT calendar_create( const struct calendar *source, ICalendar **out );

static inline struct calendar *impl_from_ICalendar( ICalendar *iface )
{
    return CONTAINING_RECORD( iface, struct calendar, ICalendar_iface );
}

static HRESULT WINAPI calendar_QueryInterface( ICalendar *iface, REFIID iid, void **out )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ICalendar ))
    {
        IInspectable_AddRef( (*out = &impl->ICalendar_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_ITimeZoneOnCalendar ))
    {
        IInspectable_AddRef( (*out = &impl->ITimeZoneOnCalendar_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI calendar_AddRef( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI calendar_Release( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        calendar_reset_languages( impl );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI calendar_GetIids( ICalendar *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_GetRuntimeClassName( ICalendar *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return calendar_create_string( RuntimeClass_Windows_Globalization_Calendar, class_name );
}

static HRESULT WINAPI calendar_GetTrustLevel( ICalendar *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI calendar_Clone( ICalendar *iface, ICalendar **value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    return calendar_create( impl, value );
}

static HRESULT WINAPI calendar_SetToMin( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local = {.wYear = CALENDAR_MIN_YEAR, .wMonth = 1, .wDay = 1};

    TRACE( "iface %p.\n", iface );

    return calendar_set_local( impl, &local, 0 ) ? S_OK : E_FAIL;
}

static HRESULT WINAPI calendar_SetToMax( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local = {.wYear = CALENDAR_MAX_YEAR, .wMonth = 12, .wDay = 31,
                        .wHour = 23, .wMinute = 59, .wSecond = 59};

    TRACE( "iface %p.\n", iface );

    return calendar_set_local( impl, &local, TICKS_PER_SEC - 1 ) ? S_OK : E_FAIL;
}

static HRESULT WINAPI calendar_get_Languages( ICalendar *iface, IVectorView_HSTRING **value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    HSTRING *copy;
    UINT32 i, count;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;

    count = impl->languages_count;
    if (!(copy = calloc( count ? count : 1, sizeof(*copy) ))) return E_OUTOFMEMORY;

    for (i = 0; i < count; i++)
    {
        if (FAILED(hr = WindowsDuplicateString( impl->languages[i], &copy[i] )))
        {
            while (i--) WindowsDeleteString( copy[i] );
            free( copy );
            return hr;
        }
    }

    if (FAILED(hr = hstring_vector_create( copy, count, value )))
        while (count--) WindowsDeleteString( copy[count] );
    free( copy );
    return hr;
}

static HRESULT WINAPI calendar_get_NumeralSystem( ICalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    return calendar_create_string( impl->numeral_system, value );
}

static HRESULT WINAPI calendar_put_NumeralSystem( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    const struct numeral_system *system;
    const WCHAR *str = WindowsGetStringRawBuffer( value, NULL );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (!(system = calendar_find_numeral_system( str ))) return E_INVALIDARG;
    lstrcpynW( impl->numeral_system, system->name, ARRAY_SIZE(impl->numeral_system) );
    return S_OK;
}

static HRESULT WINAPI calendar_GetCalendarSystem( ICalendar *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    return calendar_create_string( gregorian_calendarW, value );
}

static HRESULT WINAPI calendar_ChangeCalendarSystem( ICalendar *iface, HSTRING value )
{
    const WCHAR *str = WindowsGetStringRawBuffer( value, NULL );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (str && !wcscmp( str, gregorian_calendarW )) return S_OK;

    FIXME( "calendar system %s not implemented.\n", debugstr_hstring( value ) );
    return E_INVALIDARG;
}

static HRESULT WINAPI calendar_GetClock( ICalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    return calendar_create_string( impl->clock_12hour ? clock_12hourW : clock_24hourW, value );
}

static HRESULT WINAPI calendar_ChangeClock( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    const WCHAR *str = WindowsGetStringRawBuffer( value, NULL );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (!str) return E_INVALIDARG;
    if (!wcscmp( str, clock_12hourW )) impl->clock_12hour = TRUE;
    else if (!wcscmp( str, clock_24hourW )) impl->clock_12hour = FALSE;
    else return E_INVALIDARG;
    return S_OK;
}

static HRESULT WINAPI calendar_GetDateTime( ICalendar *iface, DateTime *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_INVALIDARG;
    result->UniversalTime = impl->datetime;
    return S_OK;
}

static HRESULT WINAPI calendar_SetDateTime( ICalendar *iface, DateTime value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;
    INT64 previous;

    TRACE( "iface %p, value %I64d.\n", iface, value.UniversalTime );

    previous = impl->datetime;
    impl->datetime = value.UniversalTime;
    if (!calendar_get_local( impl, &local, NULL ))
    {
        impl->datetime = previous;
        return E_INVALIDARG;
    }
    return S_OK;
}

static HRESULT WINAPI calendar_SetToNow( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    FILETIME filetime;

    TRACE( "iface %p.\n", iface );

    GetSystemTimeAsFileTime( &filetime );
    impl->datetime = calendar_filetime_to_ticks( &filetime );
    return S_OK;
}

static HRESULT WINAPI calendar_get_FirstEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfEras( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Era( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Era( ICalendar *iface, INT32 value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    return value == 1 ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI calendar_AddEras( ICalendar *iface, INT32 eras )
{
    TRACE( "iface %p, eras %d.\n", iface, eras );
    return eras ? E_INVALIDARG : S_OK;
}

static HRESULT WINAPI calendar_EraAsFullString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_INVALIDARG;
    return calendar_create_string( era_adW, result );
}

static HRESULT WINAPI calendar_EraAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return calendar_EraAsFullString( iface, result );
}

static HRESULT WINAPI calendar_get_FirstYearInThisEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = CALENDAR_MIN_YEAR;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastYearInThisEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = CALENDAR_MAX_YEAR;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfYearsInThisEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = CALENDAR_MAX_YEAR - CALENDAR_MIN_YEAR + 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Year( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = local.wYear;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Year( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < CALENDAR_MIN_YEAR || value > CALENDAR_MAX_YEAR) return E_INVALIDARG;
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wYear), value );
}

static HRESULT WINAPI calendar_AddYears( ICalendar *iface, INT32 years )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, years %d.\n", iface, years );

    return calendar_add_months( impl, (INT64)years * 12 );
}

static HRESULT WINAPI calendar_YearAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 year;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Year( iface, &year ))) return hr;
    return calendar_format_number( impl, year, 0, result );
}

static HRESULT WINAPI calendar_YearAsTruncatedString( ICalendar *iface, INT32 remaining_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 year, i;
    INT64 modulus = 1;
    HRESULT hr;

    TRACE( "iface %p, remaining_digits %d, result %p.\n", iface, remaining_digits, result );

    if (remaining_digits <= 0) return E_INVALIDARG;
    if (FAILED(hr = calendar_get_Year( iface, &year ))) return hr;

    for (i = 0; i < remaining_digits && modulus <= year; i++) modulus *= 10;
    return calendar_format_number( impl, year % modulus, i, result );
}

static HRESULT WINAPI calendar_YearAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 year;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Year( iface, &year ))) return hr;
    return calendar_format_number( impl, year, min_digits, result );
}

static HRESULT WINAPI calendar_get_FirstMonthInThisYear( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastMonthInThisYear( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 12;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfMonthsInThisYear( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 12;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Month( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = local.wMonth;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Month( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > 12) return E_INVALIDARG;
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wMonth), value );
}

static HRESULT WINAPI calendar_AddMonths( ICalendar *iface, INT32 months )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, months %d.\n", iface, months );

    return calendar_add_months( impl, months );
}

static HRESULT WINAPI calendar_MonthAsFullString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 month;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Month( iface, &month ))) return hr;
    return calendar_locale_string( impl, LOCALE_SMONTHNAME1 + month - 1, result );
}

static HRESULT WINAPI calendar_MonthAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 month;
    HRESULT hr;

    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );

    if (ideal_length <= 0 || ideal_length > 3) return calendar_MonthAsFullString( iface, result );
    if (FAILED(hr = calendar_get_Month( iface, &month ))) return hr;
    return calendar_locale_string( impl, LOCALE_SABBREVMONTHNAME1 + month - 1, result );
}

static HRESULT WINAPI calendar_MonthAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return calendar_MonthAsFullString( iface, result );
}

static HRESULT WINAPI calendar_MonthAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return calendar_MonthAsString( iface, ideal_length, result );
}

static HRESULT WINAPI calendar_MonthAsNumericString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 month;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Month( iface, &month ))) return hr;
    return calendar_format_number( impl, month, 0, result );
}

static HRESULT WINAPI calendar_MonthAsPaddedNumericString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 month;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Month( iface, &month ))) return hr;
    return calendar_format_number( impl, month, min_digits, result );
}

static HRESULT WINAPI calendar_AddWeeks( ICalendar *iface, INT32 weeks )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 ticks;

    TRACE( "iface %p, weeks %d.\n", iface, weeks );

    if (!calendar_multiply_ticks( weeks, 7 * TICKS_PER_DAY, &ticks )) return E_INVALIDARG;
    return calendar_add_local_ticks( impl, ticks );
}

static HRESULT WINAPI calendar_get_FirstDayInThisMonth( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastDayInThisMonth( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = days_in_month( local.wYear, local.wMonth );
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfDaysInThisMonth( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return calendar_get_LastDayInThisMonth( iface, value );
}

static HRESULT WINAPI calendar_get_Day( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = local.wDay;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Day( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > 31) return E_INVALIDARG;
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wDay), value );
}

static HRESULT WINAPI calendar_AddDays( ICalendar *iface, INT32 days )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 ticks;

    TRACE( "iface %p, days %d.\n", iface, days );

    if (!calendar_multiply_ticks( days, TICKS_PER_DAY, &ticks )) return E_INVALIDARG;
    return calendar_add_local_ticks( impl, ticks );
}

static HRESULT WINAPI calendar_DayAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 day;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Day( iface, &day ))) return hr;
    return calendar_format_number( impl, day, 0, result );
}

static HRESULT WINAPI calendar_DayAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 day;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Day( iface, &day ))) return hr;
    return calendar_format_number( impl, day, min_digits, result );
}

static HRESULT WINAPI calendar_get_DayOfWeek( ICalendar *iface, DayOfWeek *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = local.wDayOfWeek;
    return S_OK;
}

static HRESULT calendar_day_name( ICalendar *iface, BOOL abbreviated, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    LCTYPE base = abbreviated ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1;
    DayOfWeek day;
    HRESULT hr;

    if (FAILED(hr = calendar_get_DayOfWeek( iface, &day ))) return hr;
    return calendar_locale_string( impl, base + (day + 6) % 7, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsFullString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return calendar_day_name( iface, FALSE, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return calendar_day_name( iface, ideal_length > 0 && ideal_length <= 3, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return calendar_day_name( iface, FALSE, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return calendar_day_name( iface, ideal_length > 0 && ideal_length <= 3, result );
}

static HRESULT WINAPI calendar_get_FirstPeriodInThisDay( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastPeriodInThisDay( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = impl->clock_12hour ? 2 : 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfPeriodsInThisDay( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return calendar_get_LastPeriodInThisDay( iface, value );
}

static HRESULT WINAPI calendar_get_Period( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WORD hour;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!impl->clock_12hour)
    {
        *value = 1;
        return S_OK;
    }
    if (FAILED(hr = calendar_get_hour_24( impl, &hour ))) return hr;
    *value = hour < 12 ? 1 : 2;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Period( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WORD hour;
    HRESULT hr;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (!impl->clock_12hour) return value == 1 ? S_OK : E_INVALIDARG;
    if (value < 1 || value > 2) return E_INVALIDARG;
    if (FAILED(hr = calendar_get_hour_24( impl, &hour ))) return hr;

    hour = hour % 12 + (value == 2 ? 12 : 0);
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wHour), hour );
}

static HRESULT WINAPI calendar_AddPeriods( ICalendar *iface, INT32 periods )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 unit, ticks;

    TRACE( "iface %p, periods %d.\n", iface, periods );

    unit = impl->clock_12hour ? 12 * TICKS_PER_HOUR : TICKS_PER_DAY;
    if (!calendar_multiply_ticks( periods, unit, &ticks )) return E_INVALIDARG;
    return calendar_add_local_ticks( impl, ticks );
}

static HRESULT WINAPI calendar_PeriodAsFullString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 period;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_INVALIDARG;
    if (!impl->clock_12hour) return calendar_create_string( L"", result );
    if (FAILED(hr = calendar_get_Period( iface, &period ))) return hr;
    return calendar_locale_string( impl, period == 1 ? LOCALE_S1159 : LOCALE_S2359, result );
}

static HRESULT WINAPI calendar_PeriodAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return calendar_PeriodAsFullString( iface, result );
}

static HRESULT WINAPI calendar_get_FirstHourInThisPeriod( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = impl->clock_12hour ? 12 : 0;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastHourInThisPeriod( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = impl->clock_12hour ? 11 : 23;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfHoursInThisPeriod( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = impl->clock_12hour ? 12 : 24;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Hour( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WORD hour;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (FAILED(hr = calendar_get_hour_24( impl, &hour ))) return hr;

    if (!impl->clock_12hour) *value = hour;
    else *value = hour % 12 ? hour % 12 : 12;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Hour( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WORD hour;
    HRESULT hr;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (!impl->clock_12hour)
    {
        if (value < 0 || value > 23) return E_INVALIDARG;
        return calendar_set_field( impl, offsetof(SYSTEMTIME, wHour), value );
    }

    if (value < 1 || value > 12) return E_INVALIDARG;
    if (FAILED(hr = calendar_get_hour_24( impl, &hour ))) return hr;

    hour = value % 12 + (hour < 12 ? 0 : 12);
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wHour), hour );
}

static HRESULT WINAPI calendar_AddHours( ICalendar *iface, INT32 hours )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 ticks;

    TRACE( "iface %p, hours %d.\n", iface, hours );

    if (!calendar_multiply_ticks( hours, TICKS_PER_HOUR, &ticks )) return E_INVALIDARG;
    return calendar_add_local_ticks( impl, ticks );
}

static HRESULT WINAPI calendar_HourAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 hour;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Hour( iface, &hour ))) return hr;
    return calendar_format_number( impl, hour, 0, result );
}

static HRESULT WINAPI calendar_HourAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 hour;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Hour( iface, &hour ))) return hr;
    return calendar_format_number( impl, hour, min_digits, result );
}

static HRESULT WINAPI calendar_get_Minute( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = local.wMinute;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Minute( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 0 || value > 59) return E_INVALIDARG;
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wMinute), value );
}

static HRESULT WINAPI calendar_AddMinutes( ICalendar *iface, INT32 minutes )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 ticks;

    TRACE( "iface %p, minutes %d.\n", iface, minutes );

    if (!calendar_multiply_ticks( minutes, TICKS_PER_MIN, &ticks )) return E_INVALIDARG;
    return calendar_add_local_ticks( impl, ticks );
}

static HRESULT WINAPI calendar_MinuteAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 minute;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Minute( iface, &minute ))) return hr;
    return calendar_format_number( impl, minute, 0, result );
}

static HRESULT WINAPI calendar_MinuteAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 minute;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Minute( iface, &minute ))) return hr;
    return calendar_format_number( impl, minute, min_digits, result );
}

static HRESULT WINAPI calendar_get_Second( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    SYSTEMTIME local;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    if (!calendar_get_local( impl, &local, NULL )) return E_FAIL;
    *value = local.wSecond;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Second( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 0 || value > 59) return E_INVALIDARG;
    return calendar_set_field( impl, offsetof(SYSTEMTIME, wSecond), value );
}

static HRESULT WINAPI calendar_AddSeconds( ICalendar *iface, INT32 seconds )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 ticks;

    TRACE( "iface %p, seconds %d.\n", iface, seconds );

    if (!calendar_multiply_ticks( seconds, TICKS_PER_SEC, &ticks )) return E_INVALIDARG;
    return calendar_add_local_ticks( impl, ticks );
}

static HRESULT WINAPI calendar_SecondAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 second;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Second( iface, &second ))) return hr;
    return calendar_format_number( impl, second, 0, result );
}

static HRESULT WINAPI calendar_SecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 second;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Second( iface, &second ))) return hr;
    return calendar_format_number( impl, second, min_digits, result );
}

static HRESULT WINAPI calendar_get_Nanosecond( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = calendar_floor_mod( impl->datetime, TICKS_PER_SEC ) * 100;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Nanosecond( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 0 || value > 999999999) return E_INVALIDARG;
    impl->datetime = impl->datetime - calendar_floor_mod( impl->datetime, TICKS_PER_SEC ) + value / 100;
    return S_OK;
}

static HRESULT WINAPI calendar_AddNanoseconds( ICalendar *iface, INT32 nanoseconds )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, nanoseconds %d.\n", iface, nanoseconds );

    return calendar_add_local_ticks( impl, nanoseconds / 100 );
}

static HRESULT WINAPI calendar_NanosecondAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 nanosecond;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = calendar_get_Nanosecond( iface, &nanosecond ))) return hr;
    return calendar_format_number( impl, nanosecond, 0, result );
}

static HRESULT WINAPI calendar_NanosecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT32 nanosecond;
    HRESULT hr;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );

    if (FAILED(hr = calendar_get_Nanosecond( iface, &nanosecond ))) return hr;
    return calendar_format_number( impl, nanosecond, min_digits, result );
}

static HRESULT WINAPI calendar_Compare( ICalendar *iface, ICalendar *other, INT32 *result )
{
    DateTime value;
    HRESULT hr;

    TRACE( "iface %p, other %p, result %p.\n", iface, other, result );

    if (!other || !result) return E_INVALIDARG;
    if (FAILED(hr = ICalendar_GetDateTime( other, &value ))) return hr;
    return ICalendar_CompareDateTime( iface, value, result );
}

static HRESULT WINAPI calendar_CompareDateTime( ICalendar *iface, DateTime other, INT32 *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, other %I64d, result %p.\n", iface, other.UniversalTime, result );

    if (!result) return E_INVALIDARG;
    if (impl->datetime < other.UniversalTime) *result = -1;
    else if (impl->datetime > other.UniversalTime) *result = 1;
    else *result = 0;
    return S_OK;
}

static HRESULT WINAPI calendar_CopyTo( ICalendar *iface, ICalendar *other )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ITimeZoneOnCalendar *source_timezone = NULL;
    DateTime value;
    HSTRING calendar_system = NULL, clock = NULL, numeral_system = NULL, timezone_id = NULL;
    HRESULT hr;

    TRACE( "iface %p, other %p.\n", iface, other );

    if (!other) return E_INVALIDARG;

    if (FAILED(hr = ICalendar_GetCalendarSystem( other, &calendar_system )) ||
        FAILED(hr = ICalendar_GetClock( other, &clock )) ||
        FAILED(hr = ICalendar_get_NumeralSystem( other, &numeral_system )) ||
        FAILED(hr = ICalendar_GetDateTime( other, &value )))
        goto done;

    hr = ICalendar_QueryInterface( other, &IID_ITimeZoneOnCalendar, (void **)&source_timezone );
    if (SUCCEEDED(hr)) hr = ITimeZoneOnCalendar_GetTimeZone( source_timezone, &timezone_id );
    else if (hr == E_NOINTERFACE) hr = S_OK;
    if (FAILED(hr)) goto done;

    if (FAILED(hr = ICalendar_ChangeCalendarSystem( iface, calendar_system )) ||
        FAILED(hr = ICalendar_ChangeClock( iface, clock )) ||
        FAILED(hr = ICalendar_put_NumeralSystem( iface, numeral_system )) ||
        (timezone_id && FAILED(hr = ITimeZoneOnCalendar_ChangeTimeZone(
            &impl->ITimeZoneOnCalendar_iface, timezone_id ))) ||
        FAILED(hr = ICalendar_SetDateTime( iface, value )))
        goto done;

done:
    if (source_timezone) ITimeZoneOnCalendar_Release( source_timezone );
    WindowsDeleteString( timezone_id );
    WindowsDeleteString( numeral_system );
    WindowsDeleteString( clock );
    WindowsDeleteString( calendar_system );
    return hr;
}

static HRESULT WINAPI calendar_get_FirstMinuteInThisHour( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastMinuteInThisHour( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 59;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfMinutesInThisHour( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 60;
    return S_OK;
}

static HRESULT WINAPI calendar_get_FirstSecondInThisMinute( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastSecondInThisMinute( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 59;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfSecondsInThisMinute( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = 60;
    return S_OK;
}

static HRESULT WINAPI calendar_get_ResolvedLanguage( ICalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    return calendar_create_string( impl->locale, value );
}

static HRESULT WINAPI calendar_get_IsDaylightSavingTime( ICalendar *iface, boolean *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TIME_ZONE_INFORMATION info;
    SYSTEMTIME local;
    DWORD id;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;

    if (!calendar_get_local( impl, &local, NULL ) ||
        !calendar_timezone_for_year( impl, local.wYear, &info ) ||
        (id = calendar_timezone_id( &info, impl->datetime, FALSE )) == TIME_ZONE_ID_INVALID)
        return E_FAIL;
    *value = info.DaylightBias != info.StandardBias && id == TIME_ZONE_ID_DAYLIGHT;
    return S_OK;
}

static const struct ICalendarVtbl calendar_vtbl =
{
    calendar_QueryInterface,
    calendar_AddRef,
    calendar_Release,
    /* IInspectable methods */
    calendar_GetIids,
    calendar_GetRuntimeClassName,
    calendar_GetTrustLevel,
    /* ICalendar methods */
    calendar_Clone,
    calendar_SetToMin,
    calendar_SetToMax,
    calendar_get_Languages,
    calendar_get_NumeralSystem,
    calendar_put_NumeralSystem,
    calendar_GetCalendarSystem,
    calendar_ChangeCalendarSystem,
    calendar_GetClock,
    calendar_ChangeClock,
    calendar_GetDateTime,
    calendar_SetDateTime,
    calendar_SetToNow,
    calendar_get_FirstEra,
    calendar_get_LastEra,
    calendar_get_NumberOfEras,
    calendar_get_Era,
    calendar_put_Era,
    calendar_AddEras,
    calendar_EraAsFullString,
    calendar_EraAsString,
    calendar_get_FirstYearInThisEra,
    calendar_get_LastYearInThisEra,
    calendar_get_NumberOfYearsInThisEra,
    calendar_get_Year,
    calendar_put_Year,
    calendar_AddYears,
    calendar_YearAsString,
    calendar_YearAsTruncatedString,
    calendar_YearAsPaddedString,
    calendar_get_FirstMonthInThisYear,
    calendar_get_LastMonthInThisYear,
    calendar_get_NumberOfMonthsInThisYear,
    calendar_get_Month,
    calendar_put_Month,
    calendar_AddMonths,
    calendar_MonthAsFullString,
    calendar_MonthAsString,
    calendar_MonthAsFullSoloString,
    calendar_MonthAsSoloString,
    calendar_MonthAsNumericString,
    calendar_MonthAsPaddedNumericString,
    calendar_AddWeeks,
    calendar_get_FirstDayInThisMonth,
    calendar_get_LastDayInThisMonth,
    calendar_get_NumberOfDaysInThisMonth,
    calendar_get_Day,
    calendar_put_Day,
    calendar_AddDays,
    calendar_DayAsString,
    calendar_DayAsPaddedString,
    calendar_get_DayOfWeek,
    calendar_DayOfWeekAsFullString,
    calendar_DayOfWeekAsString,
    calendar_DayOfWeekAsFullSoloString,
    calendar_DayOfWeekAsSoloString,
    calendar_get_FirstPeriodInThisDay,
    calendar_get_LastPeriodInThisDay,
    calendar_get_NumberOfPeriodsInThisDay,
    calendar_get_Period,
    calendar_put_Period,
    calendar_AddPeriods,
    calendar_PeriodAsFullString,
    calendar_PeriodAsString,
    calendar_get_FirstHourInThisPeriod,
    calendar_get_LastHourInThisPeriod,
    calendar_get_NumberOfHoursInThisPeriod,
    calendar_get_Hour,
    calendar_put_Hour,
    calendar_AddHours,
    calendar_HourAsString,
    calendar_HourAsPaddedString,
    calendar_get_Minute,
    calendar_put_Minute,
    calendar_AddMinutes,
    calendar_MinuteAsString,
    calendar_MinuteAsPaddedString,
    calendar_get_Second,
    calendar_put_Second,
    calendar_AddSeconds,
    calendar_SecondAsString,
    calendar_SecondAsPaddedString,
    calendar_get_Nanosecond,
    calendar_put_Nanosecond,
    calendar_AddNanoseconds,
    calendar_NanosecondAsString,
    calendar_NanosecondAsPaddedString,
    calendar_Compare,
    calendar_CompareDateTime,
    calendar_CopyTo,
    calendar_get_FirstMinuteInThisHour,
    calendar_get_LastMinuteInThisHour,
    calendar_get_NumberOfMinutesInThisHour,
    calendar_get_FirstSecondInThisMinute,
    calendar_get_LastSecondInThisMinute,
    calendar_get_NumberOfSecondsInThisMinute,
    calendar_get_ResolvedLanguage,
    calendar_get_IsDaylightSavingTime,
};

DEFINE_IINSPECTABLE( calendar_timezone, ITimeZoneOnCalendar, struct calendar, ICalendar_iface )

static HRESULT WINAPI calendar_timezone_GetTimeZone( ITimeZoneOnCalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_INVALIDARG;
    return calendar_create_string( impl->timezone_id, value );
}

static BOOL calendar_find_timezone( const WCHAR *id, DYNAMIC_TIME_ZONE_INFORMATION *timezone )
{
    static const WCHAR mapping_keyW[] = L"Software\\Wine\\Time Zones\\TZ Mapping";
    WCHAR windows_id[128];
    DWORD index, size = sizeof(windows_id);

    for (index = 0; EnumDynamicTimeZoneInformation( index, timezone ) == ERROR_SUCCESS; index++)
        if (!wcscmp( timezone->TimeZoneKeyName, id )) return TRUE;

    if (RegGetValueW( HKEY_LOCAL_MACHINE, mapping_keyW, id, RRF_RT_REG_SZ, NULL,
                      windows_id, &size ) != ERROR_SUCCESS)
        return FALSE;

    for (index = 0; EnumDynamicTimeZoneInformation( index, timezone ) == ERROR_SUCCESS; index++)
        if (!wcscmp( timezone->TimeZoneKeyName, windows_id )) return TRUE;
    return FALSE;
}

static HRESULT WINAPI calendar_timezone_ChangeTimeZone( ITimeZoneOnCalendar *iface, HSTRING timezone_id )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    UINT32 length;
    const WCHAR *str = WindowsGetStringRawBuffer( timezone_id, &length );
    DYNAMIC_TIME_ZONE_INFORMATION previous;
    SYSTEMTIME local;

    TRACE( "iface %p, timezone_id %s.\n", iface, debugstr_hstring( timezone_id ) );

    if (!str || !length || length >= ARRAY_SIZE(impl->timezone_id)) return E_INVALIDARG;

    previous = impl->timezone;
    if (calendar_find_timezone( str, &impl->timezone ) && calendar_get_local( impl, &local, NULL ))
    {
        lstrcpynW( impl->timezone_id, str, ARRAY_SIZE(impl->timezone_id) );
        return S_OK;
    }
    impl->timezone = previous;

    WARN( "time zone %s not found.\n", debugstr_hstring( timezone_id ) );
    return E_INVALIDARG;
}

static HRESULT WINAPI calendar_timezone_TimeZoneAsFullString( ITimeZoneOnCalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    boolean daylight = FALSE;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_INVALIDARG;

    if (FAILED(hr = ICalendar_get_IsDaylightSavingTime( &impl->ICalendar_iface, &daylight ))) return hr;
    return calendar_create_string( daylight ? impl->timezone.DaylightName : impl->timezone.StandardName, result );
}

static HRESULT WINAPI calendar_timezone_TimeZoneAsString( ITimeZoneOnCalendar *iface, INT32 ideal_length,
                                                          HSTRING *result )
{
    HSTRING full = NULL;
    const WCHAR *str;
    WCHAR abbreviated[32];
    UINT32 length, pos = 0, i;
    HRESULT hr;

    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );

    if (!result) return E_INVALIDARG;
    if (FAILED(hr = calendar_timezone_TimeZoneAsFullString( iface, &full ))) return hr;
    str = WindowsGetStringRawBuffer( full, &length );
    if (ideal_length <= 0 || length <= ideal_length)
    {
        *result = full;
        return S_OK;
    }

    for (i = 0; i < length && pos < ARRAY_SIZE(abbreviated); i++)
    {
        if ((i && str[i - 1] != ' ') || str[i] == ' ') continue;
        abbreviated[pos++] = str[i];
    }
    if (!pos)
    {
        pos = min( length, ARRAY_SIZE(abbreviated) );
        memcpy( abbreviated, str, pos * sizeof(*str) );
    }
    pos = min( pos, (UINT32)ideal_length );
    hr = WindowsCreateString( abbreviated, pos, result );
    WindowsDeleteString( full );
    return hr;
}

static const struct ITimeZoneOnCalendarVtbl calendar_timezone_vtbl =
{
    calendar_timezone_QueryInterface,
    calendar_timezone_AddRef,
    calendar_timezone_Release,
    /* IInspectable methods */
    calendar_timezone_GetIids,
    calendar_timezone_GetRuntimeClassName,
    calendar_timezone_GetTrustLevel,
    /* ITimeZoneOnCalendar methods */
    calendar_timezone_GetTimeZone,
    calendar_timezone_ChangeTimeZone,
    calendar_timezone_TimeZoneAsFullString,
    calendar_timezone_TimeZoneAsString,
};

static void calendar_resolve_locale( struct calendar *impl )
{
    const WCHAR *language = NULL;

    if (impl->languages_count) language = WindowsGetStringRawBuffer( impl->languages[0], NULL );

    if (!language || !ResolveLocaleName( language, impl->locale, ARRAY_SIZE(impl->locale) ))
        GetUserDefaultLocaleName( impl->locale, ARRAY_SIZE(impl->locale) );
}

static void calendar_default_clock( struct calendar *impl )
{
    WCHAR buffer[8];

    if (GetLocaleInfoEx( impl->locale, LOCALE_ITIME, buffer, ARRAY_SIZE(buffer) ))
        impl->clock_12hour = buffer[0] == '0';
    else
        impl->clock_12hour = FALSE;
}

static HRESULT calendar_create( const struct calendar *source, ICalendar **out )
{
    struct calendar *impl;
    FILETIME filetime;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->ICalendar_iface.lpVtbl = &calendar_vtbl;
    impl->ITimeZoneOnCalendar_iface.lpVtbl = &calendar_timezone_vtbl;
    impl->ref = 1;

    if (source)
    {
        if (FAILED(hr = calendar_copy_languages( impl, source )))
        {
            free( impl );
            return hr;
        }
        impl->datetime = source->datetime;
        impl->timezone = source->timezone;
        impl->clock_12hour = source->clock_12hour;
        lstrcpynW( impl->timezone_id, source->timezone_id, ARRAY_SIZE(impl->timezone_id) );
        lstrcpynW( impl->locale, source->locale, ARRAY_SIZE(impl->locale) );
        lstrcpynW( impl->numeral_system, source->numeral_system, ARRAY_SIZE(impl->numeral_system) );
    }
    else
    {
        GetDynamicTimeZoneInformation( &impl->timezone );
        GetSystemTimeAsFileTime( &filetime );
        impl->datetime = calendar_filetime_to_ticks( &filetime );
        lstrcpynW( impl->timezone_id, impl->timezone.TimeZoneKeyName, ARRAY_SIZE(impl->timezone_id) );
        lstrcpynW( impl->numeral_system, numeral_latinW, ARRAY_SIZE(impl->numeral_system) );
        calendar_resolve_locale( impl );
        calendar_default_clock( impl );
    }

    *out = &impl->ICalendar_iface;
    return S_OK;
}

static HRESULT calendar_collect_languages( IIterable_HSTRING *languages, HSTRING **out, UINT32 *out_count )
{
    IIterator_HSTRING *iterator;
    HSTRING *array = NULL;
    UINT32 count = 0, capacity = 0;
    boolean has_current = FALSE;
    HRESULT hr;

    *out = NULL;
    *out_count = 0;
    if (!languages) return S_OK;

    if (FAILED(hr = IIterable_HSTRING_First( languages, &iterator ))) return hr;

    hr = IIterator_HSTRING_get_HasCurrent( iterator, &has_current );
    while (SUCCEEDED(hr) && has_current)
    {
        HSTRING value;

        if (FAILED(hr = IIterator_HSTRING_get_Current( iterator, &value ))) break;

        if (count == capacity)
        {
            UINT32 new_capacity = capacity ? capacity * 2 : 4;
            HSTRING *new_array;

            if (!(new_array = realloc( array, new_capacity * sizeof(*array) )))
            {
                WindowsDeleteString( value );
                hr = E_OUTOFMEMORY;
                break;
            }
            array = new_array;
            capacity = new_capacity;
        }

        array[count++] = value;
        hr = IIterator_HSTRING_MoveNext( iterator, &has_current );
    }

    IIterator_HSTRING_Release( iterator );

    if (FAILED(hr))
    {
        while (count--) WindowsDeleteString( array[count] );
        free( array );
        return hr;
    }

    *out = array;
    *out_count = count;
    return S_OK;
}

static HRESULT calendar_create_with_options( IIterable_HSTRING *languages, HSTRING calendar_system,
                                             HSTRING clock, HSTRING timezone_id, ICalendar **result )
{
    struct calendar *impl;
    ICalendar *iface;
    HRESULT hr;

    if (!result) return E_INVALIDARG;
    *result = NULL;

    if (FAILED(hr = calendar_create( NULL, &iface ))) return hr;
    impl = impl_from_ICalendar( iface );

    if (FAILED(hr = calendar_collect_languages( languages, &impl->languages, &impl->languages_count )))
    {
        ICalendar_Release( iface );
        return hr;
    }
    if (impl->languages_count)
    {
        calendar_resolve_locale( impl );
        calendar_default_clock( impl );
    }

    if (calendar_system && FAILED(hr = ICalendar_ChangeCalendarSystem( iface, calendar_system )))
    {
        ICalendar_Release( iface );
        return hr;
    }
    if (clock && FAILED(hr = ICalendar_ChangeClock( iface, clock )))
    {
        ICalendar_Release( iface );
        return hr;
    }
    if (timezone_id &&
        FAILED(hr = ITimeZoneOnCalendar_ChangeTimeZone( &impl->ITimeZoneOnCalendar_iface, timezone_id )))
    {
        ICalendar_Release( iface );
        return hr;
    }

    *result = iface;
    return S_OK;
}

struct calendar_factory
{
    IActivationFactory IActivationFactory_iface;
    ICalendarFactory ICalendarFactory_iface;
    ICalendarFactory2 ICalendarFactory2_iface;
    LONG ref;
};

static inline struct calendar_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct calendar_factory, IActivationFactory_iface );
}

static HRESULT WINAPI activation_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct calendar_factory *factory = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef( (*out = &factory->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_ICalendarFactory ))
    {
        ICalendarFactory_AddRef( (*out = &factory->ICalendarFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_ICalendarFactory2 ))
    {
        ICalendarFactory2_AddRef( (*out = &factory->ICalendarFactory2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI activation_factory_AddRef( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI activation_factory_Release( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI activation_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_ActivateInstance( IActivationFactory *iface, IInspectable **out )
{
    ICalendar *calendar;
    HRESULT hr;

    TRACE( "iface %p, out %p.\n", iface, out );

    if (!out) return E_INVALIDARG;
    if (FAILED(hr = calendar_create( NULL, &calendar ))) return hr;

    *out = (IInspectable *)calendar;
    return S_OK;
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    activation_factory_QueryInterface,
    activation_factory_AddRef,
    activation_factory_Release,
    /* IInspectable methods */
    activation_factory_GetIids,
    activation_factory_GetRuntimeClassName,
    activation_factory_GetTrustLevel,
    /* IActivationFactory methods */
    activation_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( calendar_factory, ICalendarFactory, struct calendar_factory, IActivationFactory_iface )

static HRESULT WINAPI calendar_factory_CreateCalendarDefaultCalendarAndClock( ICalendarFactory *iface,
                                                                              IIterable_HSTRING *languages,
                                                                              ICalendar **result )
{
    TRACE( "iface %p, languages %p, result %p.\n", iface, languages, result );

    return calendar_create_with_options( languages, NULL, NULL, NULL, result );
}

static HRESULT WINAPI calendar_factory_CreateCalendar( ICalendarFactory *iface, IIterable_HSTRING *languages,
                                                       HSTRING calendar_system, HSTRING clock,
                                                       ICalendar **result )
{
    TRACE( "iface %p, languages %p, calendar_system %s, clock %s, result %p.\n", iface, languages,
           debugstr_hstring( calendar_system ), debugstr_hstring( clock ), result );

    return calendar_create_with_options( languages, calendar_system, clock, NULL, result );
}

static const struct ICalendarFactoryVtbl calendar_factory_vtbl =
{
    calendar_factory_QueryInterface,
    calendar_factory_AddRef,
    calendar_factory_Release,
    /* IInspectable methods */
    calendar_factory_GetIids,
    calendar_factory_GetRuntimeClassName,
    calendar_factory_GetTrustLevel,
    /* ICalendarFactory methods */
    calendar_factory_CreateCalendarDefaultCalendarAndClock,
    calendar_factory_CreateCalendar,
};

DEFINE_IINSPECTABLE( calendar_factory2, ICalendarFactory2, struct calendar_factory, IActivationFactory_iface )

static HRESULT WINAPI calendar_factory2_CreateCalendarWithTimeZone( ICalendarFactory2 *iface,
                                                                    IIterable_HSTRING *languages,
                                                                    HSTRING calendar_system, HSTRING clock,
                                                                    HSTRING timezone_id, ICalendar **result )
{
    TRACE( "iface %p, languages %p, calendar_system %s, clock %s, timezone_id %s, result %p.\n", iface,
           languages, debugstr_hstring( calendar_system ), debugstr_hstring( clock ),
           debugstr_hstring( timezone_id ), result );

    return calendar_create_with_options( languages, calendar_system, clock, timezone_id, result );
}

static const struct ICalendarFactory2Vtbl calendar_factory2_vtbl =
{
    calendar_factory2_QueryInterface,
    calendar_factory2_AddRef,
    calendar_factory2_Release,
    /* IInspectable methods */
    calendar_factory2_GetIids,
    calendar_factory2_GetRuntimeClassName,
    calendar_factory2_GetTrustLevel,
    /* ICalendarFactory2 methods */
    calendar_factory2_CreateCalendarWithTimeZone,
};

static struct calendar_factory factory =
{
    {&activation_factory_vtbl},
    {&calendar_factory_vtbl},
    {&calendar_factory2_vtbl},
    1,
};

IActivationFactory *calendar_factory = &factory.IActivationFactory_iface;
