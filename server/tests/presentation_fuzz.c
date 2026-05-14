/*
 * libFuzzer harness for resolve_presentation_parent.
 *
 * Builds with clang + libFuzzer + ASan/UBSan:
 *   clang -I.. -O1 -g -fsanitize=fuzzer,address,undefined \
 *         presentation_fuzz.c ../presentation.c -o presentation_fuzz
 *
 * Run:
 *   ./presentation_fuzz                          # forever
 *   ./presentation_fuzz -runs=1000000            # 1M inputs
 *   ./presentation_fuzz -max_len=256             # cap input size
 *   ./presentation_fuzz corpus/                  # replay corpus
 *
 * The fuzzer mutates raw bytes; we parse them into a synthetic
 * window-tree fixture and call resolve_presentation_parent.  Failures
 * are signalled by assert(); libFuzzer captures the input, ASan
 * reports memory errors, UBSan reports integer / pointer UB.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#include "presentation.h"

/* - Fixture (kept in sync with presentation_test.c's mock_window) - */

struct mock_window
{
    unsigned int handle;
    unsigned int owner;
    unsigned int parent;
    unsigned int protocol;
    unsigned int pid;
    const void  *desktop;
    int          destroyed;
};

struct fixture
{
    struct mock_window *windows;
    size_t              count;
};

/* Two stable desktop sentinels. */
static const char desktop_a_marker;
static const char desktop_b_marker;
#define DESKTOP_A (&desktop_a_marker)
#define DESKTOP_B (&desktop_b_marker)

static struct mock_window *fixture_find( struct fixture *fx, unsigned int handle )
{
    size_t i;
    for (i = 0; i < fx->count; i++)
    {
        if (fx->windows[i].handle == handle && !fx->windows[i].destroyed)
            return &fx->windows[i];
    }
    return NULL;
}

static void *cb_lookup( unsigned int handle, void *ctx )
{
    return fixture_find( (struct fixture *)ctx, handle );
}

static unsigned int cb_get_owner( void *node, void *ctx )
{
    (void)ctx;
    return ((struct mock_window *)node)->owner;
}

static unsigned int cb_get_protocol( void *node, void *ctx )
{
    (void)ctx;
    return ((struct mock_window *)node)->protocol;
}

static unsigned int cb_get_pid( void *node, void *ctx )
{
    (void)ctx;
    return ((struct mock_window *)node)->pid;
}

static unsigned int cb_get_parent( void *node, void *ctx )
{
    (void)ctx;
    return ((struct mock_window *)node)->parent;
}

static const void *cb_get_desktop( void *node, void *ctx )
{
    (void)ctx;
    return ((struct mock_window *)node)->desktop;
}

static const struct presentation_query_ops mock_ops =
{
    cb_lookup, cb_get_owner, cb_get_protocol,
    cb_get_pid, cb_get_parent, cb_get_desktop,
};

/* - Fuzz target -
 *
 * Input layout (raw bytes consumed in order; missing bytes default to 0):
 *
 *   byte 0       : window count (2 .. MAX_WIN, mod-clamped)
 *   byte 1       : start-window index (mod count)
 *   byte 2       : querying_pid (0 .. 7)
 *   bytes 3..N*5+2 : per-window attributes, 5 bytes each:
 *     +0 : owner-index byte (0=no owner, else (b%count) gives owner)
 *     +1 : parent-index byte (same encoding)
 *     +2 : protocol  (0=NONE, 1=XDG, else NONE)
 *     +3 : pid (0..7)
 *     +4 : low bit=desktop-A vs desktop-B, bit-1=destroyed
 *
 * Total bytes needed for N windows: 3 + 5*N.  Anything past that is
 * ignored.  Anything shorter than 3 produces a 2-window default fixture
 * driven by the bytes we have. */

enum { MAX_WIN = 16 };

static unsigned int byte_or_zero( const uint8_t *data, size_t size, size_t offset )
{
    return (offset < size) ? data[offset] : 0u;
}

int LLVMFuzzerTestOneInput( const uint8_t *data, size_t size )
{
    struct mock_window wins[MAX_WIN];
    struct fixture fx;
    unsigned int reason = 0xDEADBEEF;
    unsigned int anchor;
    unsigned int count, start_idx, qpid;
    unsigned int i;

    count = 2 + (byte_or_zero( data, size, 0 ) % (MAX_WIN - 1));
    start_idx = byte_or_zero( data, size, 1 ) % count;
    qpid = byte_or_zero( data, size, 2 ) & 0x7;

    for (i = 0; i < count; i++)
    {
        unsigned int base = 3 + i * 5;
        unsigned int owner_byte    = byte_or_zero( data, size, base + 0 );
        unsigned int parent_byte   = byte_or_zero( data, size, base + 1 );
        unsigned int proto_byte    = byte_or_zero( data, size, base + 2 );
        unsigned int pid_byte      = byte_or_zero( data, size, base + 3 );
        unsigned int flags_byte    = byte_or_zero( data, size, base + 4 );

        wins[i].handle    = 0x1000 + i;
        wins[i].owner     = owner_byte ? (0x1000 + (owner_byte % count)) : 0;
        wins[i].parent    = parent_byte ? (0x1000 + (parent_byte % count)) : 0;
        wins[i].protocol  = (proto_byte == 1)
                                ? PRESENTATION_PROTOCOL_XDG_FOREIGN_V2
                                : PRESENTATION_PROTOCOL_NONE;
        wins[i].pid       = pid_byte & 0x7;
        wins[i].desktop   = (flags_byte & 0x1) ? DESKTOP_A : DESKTOP_B;
        wins[i].destroyed = !!(flags_byte & 0x2);
    }
    fx.windows = wins;
    fx.count   = count;

    anchor = resolve_presentation_parent( 0x1000 + start_idx, qpid,
                                          &mock_ops, &fx, &reason );

    /* Invariant 1: reason_out always overwritten (never 0xDEADBEEF). */
    assert( reason != 0xDEADBEEF );

    /* Invariant 2: anchor != 0 iff EXPORT_FOUND set. */
    if (anchor != 0)
        assert( reason & PRESENTATION_REASON_EXPORT_FOUND );
    if (reason & PRESENTATION_REASON_EXPORT_FOUND)
        assert( anchor != 0 );

    /* Invariant 3: OWNER_CHAIN bit is set iff EXPORT_FOUND bit is set. */
    assert( !!(reason & PRESENTATION_REASON_OWNER_CHAIN) ==
            !!(reason & PRESENTATION_REASON_EXPORT_FOUND) );

    /* Invariant 4: SAME_PROCESS implies EXPORT_FOUND. */
    if (reason & PRESENTATION_REASON_SAME_PROCESS)
        assert( reason & PRESENTATION_REASON_EXPORT_FOUND );

    /* Invariant 5: CYCLE_BROKEN and EXPORT_FOUND are mutually exclusive. */
    assert( !((reason & PRESENTATION_REASON_CYCLE_BROKEN) &&
              (reason & PRESENTATION_REASON_EXPORT_FOUND)) );

    /* Invariant 6: returned anchor handle, if non-zero, must exist in
     * the fixture and not be destroyed. */
    if (anchor != 0)
    {
        struct mock_window *a = fixture_find( &fx, anchor );
        assert( a != NULL );
        assert( !a->destroyed );
        /* The anchor must NOT be the start window itself. */
        assert( anchor != wins[start_idx].handle );
        /* The anchor must have a non-NONE protocol. */
        assert( a->protocol != PRESENTATION_PROTOCOL_NONE );
    }

    /* Invariant 7: stateless contract - second resolve on the same
     * fixture must return identical result.  Catches accidental
     * statics or TLS state that survives a call. */
    {
        unsigned int reason2 = 0xDEADBEEF;
        unsigned int anchor2 = resolve_presentation_parent(
            0x1000 + start_idx, qpid, &mock_ops, &fx, &reason2 );
        assert( anchor == anchor2 );
        assert( reason == reason2 );
    }

    return 0;
}
