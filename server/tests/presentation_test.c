/*
 * Unit tests for resolve_presentation_parent.
 *
 * Standalone test binary; build with:
 *   gcc -I.. -Wall -Wextra -O0 -g -pthread \
 *       presentation_test.c ../presentation.c -o presentation_test
 * Run with:
 *   ./presentation_test
 *
 * Exits 0 on success, prints first failure and exits 1 otherwise.
 *
 * Mocks the server's struct window with a tiny fixture struct and
 * provides callback implementations of struct presentation_query_ops
 * over a static array. Since the resolver is callback-abstracted, we
 * don't need any server bootstrap - no thread, desktop, atom table,
 * shared memory, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "presentation.h"

/* Test fixture: a tiny mock-window struct with just the fields the
 * resolver queries. */
struct mock_window
{
    unsigned int handle;
    unsigned int owner;       /* 0 if no owner */
    unsigned int parent;      /* GW_CHILD parent for WS_CHILD fallback (0 otherwise) */
    unsigned int protocol;    /* PRESENTATION_PROTOCOL_NONE / _XDG_FOREIGN_V2 */
    unsigned int pid;
    const void *desktop;      /* opaque desktop identity (NULL = unknown) */
    int destroyed;            /* simulate destroyed state */
};

/* The fixture context: an array of mock_windows, scanned linearly. */
struct fixture
{
    struct mock_window *windows;
    size_t count;
};

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

/* Callback ops for the resolver. */
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
    cb_lookup,
    cb_get_owner,
    cb_get_protocol,
    cb_get_pid,
    cb_get_parent,
    cb_get_desktop,
};

/* Variant ops without get_parent - pins the "older callers without
 * the callback only walk the owner chain" contract from the header. */
static const struct presentation_query_ops mock_ops_no_parent =
{
    cb_lookup,
    cb_get_owner,
    cb_get_protocol,
    cb_get_pid,
    NULL,
    NULL,
};

/* - Test infrastructure - */

static int tests_run = 0;
static int tests_failed = 0;
static const char *current_test = NULL;

#define ASSERT_EQ_HEX(actual, expected, msg)                                        \
    do {                                                                            \
        unsigned int _a = (actual), _e = (expected);                                \
        if (_a != _e)                                                               \
        {                                                                           \
            fprintf( stderr, "  FAIL [%s]: %s\n", current_test, msg );              \
            fprintf( stderr, "    expected 0x%x, got 0x%x\n", _e, _a );             \
            tests_failed++;                                                         \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define ASSERT_FLAG_SET(actual, flag, msg)                                          \
    do {                                                                            \
        if (!((actual) & (flag)))                                                   \
        {                                                                           \
            fprintf( stderr, "  FAIL [%s]: %s\n", current_test, msg );              \
            fprintf( stderr, "    expected flag 0x%x in 0x%x\n",                    \
                     (unsigned int)(flag), (actual) );                              \
            tests_failed++;                                                         \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define ASSERT_FLAG_CLEAR(actual, flag, msg)                                        \
    do {                                                                            \
        if ((actual) & (flag))                                                      \
        {                                                                           \
            fprintf( stderr, "  FAIL [%s]: %s\n", current_test, msg );              \
            fprintf( stderr, "    expected flag 0x%x ABSENT from 0x%x\n",           \
                     (unsigned int)(flag), (actual) );                              \
            tests_failed++;                                                         \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define ASSERT(cond)                                                                \
    do {                                                                            \
        if (!(cond))                                                                \
        {                                                                           \
            fprintf( stderr, "  FAIL [%s]: ASSERT(%s) at %s:%d\n",                  \
                     current_test, #cond, __FILE__, __LINE__ );                     \
            tests_failed++;                                                         \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define RUN_TEST(fn)                                                                \
    do {                                                                            \
        current_test = #fn;                                                         \
        tests_run++;                                                                \
        fn();                                                                       \
    } while (0)

/* - Tests - */

/* T1: a single window with no owner resolves to NO_ANCHOR. */
static void test_no_owner( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
    };
    struct fixture fx = { wins, 1 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "expected no anchor for ownerless window" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "reason should be NO_ANCHOR" );
}

/* T2: simple owner chain - child owned by an exporter resolves to that owner. */
static void test_simple_owner( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1 },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1 },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "expected anchor to be the exporting owner" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_OWNER_CHAIN, "should report OWNER_CHAIN" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "should report EXPORT_FOUND" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_SAME_PROCESS, "should report SAME_PROCESS" );
}

/* T3: skip rule - invisible CEF helper in the middle of the chain is skipped. */
static void test_skip_invisible_helper( void )
{
    struct mock_window wins[] = {
        /* Main shell, exports a token */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1 },
        /* CEF-internal helper, no export - must be skipped */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2 },
        /* CEF DComp modal, owned by the helper */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2 },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "must walk past helper to reach exporting main shell" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "should report EXPORT_FOUND" );
    /* Anchor (0x100) is in pid 1; querying pid is 2 - different processes. */
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS, "must NOT be same-process" );
}

/* T4: cross-process detection: anchor in different pid -> SAME_PROCESS NOT set. */
static void test_cross_process( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1 },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2 },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "anchor resolves" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS, "must NOT be same-process" );
}

/* T5: dangling owner handle (owner destroyed before child) -> NO_ANCHOR. */
static void test_dangling_owner( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2,
          .pid = 1, .destroyed = 1 },                                       /* destroyed */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,
          .pid = 1 },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "destroyed owner must yield no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "reason should be NO_ANCHOR" );
}

/* T6: chain with no exporter anywhere - NO_ANCHOR even after walking. */
static void test_chain_no_exporter( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "no exporter in chain -> no anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no EXPORT_FOUND" );
}

/* T7: cycle detection - A owns B, B owns A. Resolver must terminate. */
static void test_cycle_detection( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    /* Should terminate without infinite-looping. */
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "cycle has no exporter -> no anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN, "CYCLE_BROKEN must be reported" );
}

/* T8: deep chain exceeds PRESENTATION_MAX_DEPTH -> CYCLE_BROKEN. */
static void test_max_depth( void )
{
    /* Build a chain of length MAX_DEPTH+5 with no exporter at the end. */
    enum { CHAIN = PRESENTATION_MAX_DEPTH + 5 };
    struct mock_window wins[CHAIN];
    struct fixture fx;
    unsigned int reason;
    unsigned int anchor;
    int i;

    for (i = 0; i < CHAIN; i++)
    {
        wins[i].handle    = 0x1000 + i;
        wins[i].owner     = (i + 1 < CHAIN) ? 0x1000 + i + 1 : 0;
        wins[i].protocol  = PRESENTATION_PROTOCOL_NONE;
        wins[i].pid       = 1;
        wins[i].destroyed = 0;
    }
    fx.windows = wins;
    fx.count   = CHAIN;

    anchor = resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "chain longer than MAX_DEPTH bails out without anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN, "depth limit must report CYCLE_BROKEN" );
}

/* T9: querying pid = 0 disables SAME_PROCESS check entirely. */
static void test_querying_pid_zero( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1 },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1 },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "anchor resolves with pid=0" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS, "pid=0 disables SAME_PROCESS" );
}

/* T10: nearer exporter wins (we don't keep walking past the first match). */
static void test_nearest_exporter( void )
{
    struct mock_window wins[] = {
        /* Outer exporter - must NOT be picked because the inner one is nearer. */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1 },
        /* Inner exporter - should be picked. */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1 },
        /* Child of inner. */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1 },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x200, "nearest exporter must be picked, not the further one" );
    (void)reason;
}

/* T11: bad inputs - NULL ops, NULL lookup callback, zero start. */
static void test_bad_inputs( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
    };
    struct fixture fx = { wins, 1 };
    unsigned int reason = 0xDEADBEEF;
    unsigned int anchor;

    /* zero handle */
    anchor = resolve_presentation_parent( 0, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "zero start handle yields no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "zero handle: NO_ANCHOR" );

    /* NULL ops */
    reason = 0xDEADBEEF;
    anchor = resolve_presentation_parent( 0x100, 1, NULL, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "NULL ops yields no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "NULL ops: NO_ANCHOR" );

    /* NULL reason_out is allowed and must not crash. */
    anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, NULL );
    ASSERT_EQ_HEX( anchor, 0, "NULL reason_out tolerated" );

    /* Lookup of unknown handle */
    reason = 0xDEADBEEF;
    anchor = resolve_presentation_parent( 0xDEAD, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "unknown handle yields no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "unknown handle: NO_ANCHOR" );
}

/* T12: pid match still requires anchor, not just chain walk.
 *
 * Rationale for clearing SAME_PROCESS on NO_ANCHOR: the flag exists to
 * tell drivers "skip the xdg_foreign import path and link directly".
 * It only makes sense when there is something to link to. Setting it
 * whenever the chain has any same-pid link - even when no exporter is
 * found - would let callers see a SAME_PROCESS hint without an anchor
 * handle, which is undefined behaviour on the import side and just
 * noise on the trace side. The resolver therefore sets SAME_PROCESS
 * only inside the success path, after EXPORT_FOUND is decided. This
 * test pins that contract: same-pid chain, no exporter, anchor=0 =>
 * SAME_PROCESS must be 0. */
static void test_same_process_only_with_export( void )
{
    /* A -> B (no export) -> C (queries). All same pid. SAME_PROCESS must
     * NOT be set in the NO_ANCHOR case because there's no anchor. */
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1 },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "no exporter -> no anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS,
                       "SAME_PROCESS only meaningful when an anchor was found" );
}

/* T13: GW_CHILD parent fallback. WS_CHILD frame B has no owner but
 * parent A is an exporter; child popup C resolves to A by walking
 * owner=0 -> parent=A. Without the fallback C would resolve to nothing. */
static void test_child_parent_fallback( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .parent = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2 },
        { .handle = 0x200, .owner = 0,     .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE          }, /* WS_CHILD frame */
        { .handle = 0x300, .owner = 0x200, .parent = 0,     .protocol = PRESENTATION_PROTOCOL_NONE          }, /* popup owned by frame */
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "popup -> frame.owner=0 -> frame.parent=A" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "anchor has export" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_OWNER_CHAIN, "reached via chain walk" );
}

/* T14: owner takes priority over parent at every step. B has both an
 * owner (X, no export) and a parent (A, exporter); the resolver must
 * follow the owner first. Since X has no further owner/parent, we
 * stop with NO_ANCHOR rather than backtracking to A. */
static void test_owner_priority_over_parent( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2 }, /* would-be parent anchor */
        { .handle = 0x400, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE          }, /* owner stub */
        { .handle = 0x200, .owner = 0x400, .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE  },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "owner chain wins; X dead-ends -> no anchor" );
}

/* T15: missing get_parent callback (NULL) => pure owner-chain walk.
 * Same fixture as T13, but without parent fallback the popup must
 * resolve to nothing. */
static void test_no_parent_callback_owner_only( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .parent = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2 },
        { .handle = 0x200, .owner = 0,     .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE          },
        { .handle = 0x300, .owner = 0x200, .parent = 0,     .protocol = PRESENTATION_PROTOCOL_NONE          },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 0, &mock_ops_no_parent, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "no get_parent callback -> owner-only walk -> NO_ANCHOR" );
}

/* Two opaque desktop sentinels. The resolver only checks pointer
 * equality on these, so any two distinct addresses work. */
static const char desktop_a_marker;
static const char desktop_b_marker;
#define DESKTOP_A (&desktop_a_marker)
#define DESKTOP_B (&desktop_b_marker)

/* T16: cross-desktop anchor is rejected. Start on desktop A, would-be
 * anchor on desktop B -> resolver must walk past it (or return NO_ANCHOR
 * if no other candidate exists). */
static void test_cross_desktop_rejected( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2,
          .desktop = DESKTOP_B },
        { .handle = 0x200, .owner = 0x100, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE,
          .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "cross-desktop anchor rejected" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND,
                       "rejected anchor must not flag EXPORT_FOUND" );
}

/* T17: anchor on the SAME desktop is accepted normally - proves T16
 * isn't a bug that just kills every walk. */
static void test_same_desktop_accepted( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2,
          .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE,
          .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "same-desktop anchor accepted" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* T18: chain that crosses desktops mid-walk and then returns to the
 * start's desktop with a viable exporter - the cross-desktop intermediate
 * is skipped, and the higher exporter on the start's desktop is found. */
static void test_chain_skips_other_desktop( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2,
          .desktop = DESKTOP_A },
        { .handle = 0x150, .owner = 0x100, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2,
          .desktop = DESKTOP_B }, /* exporter on wrong desktop - must be skipped */
        { .handle = 0x200, .owner = 0x150, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE,
          .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "skip cross-desktop intermediate, accept correct-desktop ancestor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* ============================================================== */
/*   Realistic CEF / Chromium broker scenarios                     */
/* ============================================================== */

/* Ubisoft Connect / Steam pattern: launcher main toplevel owns CEF
 * subprocess main, which owns the render subprocess, which owns a
 * modal "Are you sure?" dialog.  Only the launcher main carries
 * xdg-foreign export; the modal must walk 3 levels across 4 distinct
 * processes to reach it. */
static void test_cef_4_process_modal_chain( void )
{
    struct mock_window wins[] = {
        /* launcher main, the one xdg-foreign-exported toplevel */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* CEF main process toplevel, owned by launcher */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
        /* render subprocess offscreen toplevel, owned by CEF main */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 3, .desktop = DESKTOP_A },
        /* "Are you sure?" modal, owned by render subprocess toplevel */
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 4, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x400, 4, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "4-process modal must resolve to launcher main" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "anchor has xdg-foreign" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_OWNER_CHAIN, "reached via 3-hop owner walk" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS, "modal and launcher in different processes" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN, "no cycle, no depth limit hit" );
}

/* Two sibling popups owned by the same main toplevel - CEF spawns one
 * for autocomplete and another for a context menu, both anchored to the
 * input field's containing top-level.  Both must resolve to the same
 * anchor and not pollute each other's state. */
static void test_cef_sibling_modals_same_anchor( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x201, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason1, reason2;
    unsigned int anchor1 = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason1 );
    unsigned int anchor2 = resolve_presentation_parent( 0x201, 1, &mock_ops, &fx, &reason2 );
    ASSERT_EQ_HEX( anchor1, 0x100, "first sibling resolves" );
    ASSERT_EQ_HEX( anchor2, 0x100, "second sibling resolves to same anchor" );
    ASSERT_EQ_HEX( reason1, reason2, "reasons match for sibling popups" );
}

/* Window destroyed mid-chain (race: process B died after popup created).
 * Chain: A(XDG) owned by B(destroyed) - wait, that's start-side.
 * Actual case: start C -> owner B (destroyed) -> ... A.  Lookup of B
 * returns NULL because destroyed, chain terminates at B with no anchor. */
static void test_cef_mid_chain_destroyed( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A, .destroyed = 1 },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 3, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 3, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "destroyed mid-chain truncates walk; anchor unreachable" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no exporter found" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN, "not a cycle, just a dangling intermediate" );
}

/* Start window itself is destroyed (resolver called on stale handle
 * from a queued message).  Must return NO_ANCHOR without crash. */
static void test_cef_destroyed_start_window( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A, .destroyed = 1 },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "destroyed start window: no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "reason is NO_ANCHOR" );
}

/* Boundary: xdg-foreign exporter sits exactly at MAX_DEPTH - 1 hops
 * from the start window (the deepest reachable position).  Must still
 * resolve successfully without tripping CYCLE_BROKEN.  CEF's worst-
 * case nested-iframe modal stack can hit double-digit owner depth. */
static void test_cef_xdg_at_max_depth_boundary_succeeds( void )
{
    /* Build a chain of length MAX_DEPTH where the LAST node carries XDG.
     * That means depth between start (index 0) and anchor (index MAX-1)
     * is MAX-1 - the deepest position still reachable. */
    enum { CHAIN = PRESENTATION_MAX_DEPTH };
    struct mock_window wins[CHAIN];
    struct fixture fx;
    unsigned int reason;
    unsigned int anchor;
    int i;

    for (i = 0; i < CHAIN; i++)
    {
        wins[i].handle    = 0x1000 + i;
        wins[i].owner     = (i + 1 < CHAIN) ? 0x1000 + i + 1 : 0;
        wins[i].protocol  = (i == CHAIN - 1)
                                ? PRESENTATION_PROTOCOL_XDG_FOREIGN_V2
                                : PRESENTATION_PROTOCOL_NONE;
        wins[i].pid       = 1;
        wins[i].destroyed = 0;
        wins[i].parent    = 0;
        wins[i].desktop   = DESKTOP_A;
    }
    fx.windows = wins;
    fx.count   = CHAIN;

    anchor = resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x1000 + CHAIN - 1, "anchor at depth MAX-1 must still resolve" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set at boundary" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN, "exactly at limit, no overrun" );
}

/* Alternating protocols: the walk must pick the NEAREST xdg-foreign,
 * not the FURTHEST one.  Realistic CEF case: nested iframes where each
 * sub-frame's render-subprocess toplevel carries its own export. */
static void test_cef_alternating_proto_picks_nearest_export( void )
{
    struct mock_window wins[] = {
        /* deepest anchor - has XDG but should be SKIPPED in favor of the nearer one */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
        /* near-mid anchor - this is what the resolver should return */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 3, .desktop = DESKTOP_A },
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 4, .desktop = DESKTOP_A },
        /* start window */
        { .handle = 0x500, .owner = 0x400, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 5, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 5 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x500, 5, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x300, "nearest exporter (0x300) wins over deeper one (0x100)" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* PID=0 anchor (e.g. the owner record predates PID tracking, or the
 * process slot was reused).  Even if the querying PID is also non-zero
 * and equal, SAME_PROCESS must NOT be set because the anchor's PID is
 * unreliable. */
static void test_cef_pid_zero_anchor_clears_same_process( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 0, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "anchor with PID=0 still resolved" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS,
                       "anchor PID=0 must NOT match querying PID, even when equal" );
}

/* Minimum-viable callbacks: only lookup/get_owner/get_protocol set.
 * Older wineserver wrappers that hadn't added get_pid/get_parent/
 * get_desktop must still get correct anchor resolution; only the
 * SAME_PROCESS hint and cross-desktop guard are silently dropped. */
static void test_cef_minimum_viable_ops( void )
{
    /* Custom ops with only the three required callbacks. */
    static const struct presentation_query_ops min_ops = {
        cb_lookup, cb_get_owner, cb_get_protocol, NULL, NULL, NULL
    };
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &min_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "minimum-viable ops still resolve anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS,
                       "no get_pid callback -> SAME_PROCESS never set" );
}

/* Cross-process WS_CHILD fallback: CEF's GPU subprocess creates a
 * popup-style WS_CHILD whose Wine parent is the launcher's main
 * toplevel in a different process.  Owner is NULL, parent fallback
 * is what reaches the exporter. */
static void test_cef_cross_process_via_parent_fallback( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* popup with no owner - relies on parent (cross-process) */
        { .handle = 0x200, .owner = 0, .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,       .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "cross-process WS_CHILD parent fallback resolves anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS,
                       "popup and anchor in different processes" );
}

/* Walk passes through a node whose get_desktop returns NULL while
 * start_desktop is non-NULL.  The cross-desktop guard treats NULL as
 * mismatch and skips the would-be anchor.  Realistic case: window
 * unparented mid-resolve, race between query and SetWindowOwner. */
static void test_cef_unknown_desktop_anchor_rejected( void )
{
    struct mock_window wins[] = {
        /* would-be anchor, but its desktop is NULL (unknown / detached) */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = NULL },
        /* start, on a known desktop */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "anchor on unknown/NULL desktop rejected" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no usable export found" );
}

/* Two-hop fallback: start is WS_CHILD with no owner, parent has owner
 * which has XDG.  This exercises the parent-then-owner walk pattern,
 * not just parent-as-anchor.  CEF's BrowserHost spawns child widgets
 * whose Wine parent is a wrapper window owned by the actual toplevel. */
static void test_cef_parent_then_owner_chain( void )
{
    struct mock_window wins[] = {
        /* launcher main, the exporter */
        { .handle = 0x100, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* wrapper window owned by main */
        { .handle = 0x200, .owner = 0x100, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE,       .pid = 1, .desktop = DESKTOP_A },
        /* child widget: no owner, parent is the wrapper */
        { .handle = 0x300, .owner = 0, .parent = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,       .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "child -> parent -> owner reaches the exporter" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_SAME_PROCESS,
                     "everything in pid 1, SAME_PROCESS set" );
}

/* Cycle at depth 2 (mutual ownership A <-> B reached through C).  CEF
 * cannot create this directly, but bad state from window-owner
 * mutation races has shown up in field reports. */
static void test_cef_cycle_via_three_window_loop( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    /* Start at 0x200 -> 0x100 -> 0x300 -> 0x200 (back to start). */
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "3-window cycle has no exporter -> no anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "CYCLE_BROKEN must be reported when start is revisited" );
}

/* ============================================================== */
/*   Dynamic mutation scenarios (T31-T42 review batch)             */
/* ============================================================== */

/* T31: popup's GW_OWNER changes from A to B then back to nothing.
 * Each resolve call observes the current fixture state and must
 * report a consistent answer; the algorithm itself is stateless
 * across calls, but this pins that contract. */
static void test_cef_owner_to_parent_transition( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x101, .owner = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* popup: owner mutates between calls */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,       .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason, anchor;

    /* Initial state: popup -> A. */
    anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "initial owner = A -> anchor A" );

    /* Owner reassigned: popup -> B. */
    wins[2].owner = 0x101;
    anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x101, "owner reassigned to B -> anchor B" );

    /* Owner cleared: popup has no anchor. */
    wins[2].owner = 0;
    anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "owner cleared -> no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "cleared owner: NO_ANCHOR" );
}

/* T32: mid-chain intermediate that initially has no export later
 * acquires one. Two resolves: first finds the deeper exporter, second
 * finds the now-closer exporter. */
static void test_cef_mid_chain_acquires_export( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 3, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason, anchor;

    /* Before: 0x200 has no protocol, so anchor walks to 0x100. */
    anchor = resolve_presentation_parent( 0x300, 3, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "initially: deepest exporter wins" );

    /* 0x200 now acquires xdg-foreign export. */
    wins[1].protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2;

    anchor = resolve_presentation_parent( 0x300, 3, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x200, "intermediate acquired export: now closer wins" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS,
                       "0x200 in pid 2, querying from pid 3" );
}

/* T33: chain with two intermediates, neither initially exporting.
 * First the deeper exporter is reached. Then the lower intermediate
 * acquires export; new walk picks that. */
static void test_cef_multiple_intermediates_progressive_export( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 3, .desktop = DESKTOP_A },
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 4, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason, anchor;

    anchor = resolve_presentation_parent( 0x400, 4, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "initially: walk to deepest" );

    /* 0x200 (next-deepest) acquires export. */
    wins[1].protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2;
    anchor = resolve_presentation_parent( 0x400, 4, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x200, "0x200 closer than 0x100 once it has export" );

    /* 0x300 (closest) ALSO acquires export. Now the nearest wins. */
    wins[2].protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2;
    anchor = resolve_presentation_parent( 0x400, 4, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x300, "nearest exporter wins over both deeper ones" );
}

/* T35: exporter clears its export between two resolves. The popup
 * previously resolving to it must now miss the (cleared) anchor and
 * either walk further or return NO_ANCHOR. */
static void test_cef_exporter_becomes_unexported( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason, anchor;

    /* Before: anchor resolves. */
    anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "initial anchor resolves" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );

    /* Exporter clears its export. */
    wins[0].protocol = PRESENTATION_PROTOCOL_NONE;

    /* Walk continues past 0x100 (no protocol), 0x100's owner is 0 ->
     * chain ends -> NO_ANCHOR with EXPORT_FOUND clear. */
    anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "after clear: no anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no EXPORT after clear" );
}

/* T37: walk reaches a desktop-like terminus (owner=0, no parent,
 * protocol=NONE). Must return NO_ANCHOR without cycle-breaking
 * (the chain ended cleanly, just no exporter). */
static void test_cef_chain_ends_at_desktop_window( void )
{
    struct mock_window wins[] = {
        /* "desktop": no owner, no parent, no protocol */
        { .handle = 0x010, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 0, .desktop = DESKTOP_A },
        /* intermediate */
        { .handle = 0x100, .owner = 0x010, .protocol = PRESENTATION_PROTOCOL_NONE,          .pid = 1, .desktop = DESKTOP_A },
        /* start */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,          .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "desktop terminus: no anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                       "clean chain end, not a cycle" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no exporter" );
}

/* T39: cross-desktop intermediate, but chain re-enters start's desktop
 * higher up. start (A) -> mid (B, would-be anchor SKIPPED) -> top (A, real anchor).
 * The per-step desktop guard skips B but keeps walking to find the
 * desktop-A anchor on top. */
static void test_cef_cross_desktop_then_return_to_start_desktop( void )
{
    struct mock_window wins[] = {
        /* topmost: on desktop A, has export - this should win */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* intermediate: on desktop B (different from start) - must be SKIPPED */
        { .handle = 0x150, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 2, .desktop = DESKTOP_B },
        /* start: on desktop A */
        { .handle = 0x200, .owner = 0x150, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 3, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 3, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "skip cross-desktop intermediate, find desktop-A anchor higher up" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN, "not a cycle" );
}

/* T40: three-hop chain where ONLY the deepest ancestor carries the
 * export. Validates that the walk doesn't bail after the first NONE
 * intermediate. */
static void test_cef_depth_3_all_none_except_deepest( void )
{
    struct mock_window wins[] = {
        /* deepest, only one with export */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* start */
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x400, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "walk past 3 NONE intermediates to reach deepest exporter" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_SAME_PROCESS,
                     "all in pid 1, SAME_PROCESS set" );
}

/* T41: mid-chain node has neither owner nor parent. The walk
 * terminates cleanly. Pins behavior at the "ops->get_parent returns
 * 0" path when the chain ends mid-way. */
static void test_cef_mid_chain_terminates_with_no_owner_no_parent( void )
{
    struct mock_window wins[] = {
        /* dead-end: no owner, no parent */
        { .handle = 0x100, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        /* start */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "mid-chain terminus: no anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                       "clean termination, not depth-limited" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no exporter found" );
}

/* T36 (reframed): destroyed intermediate truncates the walk and any
 * exporter beyond is unreachable. Same shape as
 * test_cef_mid_chain_destroyed but with the exporter ABOVE the
 * destroyed intermediate (rather than below) - pins that the algorithm
 * doesn't try to "step over" a dangling lookup. */
static void test_cef_destroyed_blocks_walk_to_exporter_above( void )
{
    struct mock_window wins[] = {
        /* exporter, reachable in principle */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* intermediate, destroyed - lookup returns NULL */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A, .destroyed = 1 },
        /* start */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 3, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 3, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0,
                   "destroyed intermediate truncates walk; exporter above is unreachable" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no anchor reached" );
}

/* Self-owner at start: start.owner = start.  The algorithm's
 * cursor_handle == start_handle guard fires at the first step.
 * Distinct from the existing 2-window cycle_detection (A<->B). */
static void test_cef_self_owner_at_start( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 1 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "self-owner at start: no anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "self-owner triggers cycle guard" );
}

/* Self-owner mid-chain: cursor.owner == cursor (not start).  The
 * start_handle guard does NOT fire; instead the walk spins on the
 * self-owning node until depth hits MAX_DEPTH. */
static void test_cef_self_owner_at_mid_chain_hits_depth_limit( void )
{
    struct mock_window wins[] = {
        /* mid-chain self-owner */
        { .handle = 0x100, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        /* start: owner = self-owning node */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "self-owner mid-chain: no anchor (depth limit)" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "self-owner mid-chain spins until MAX_DEPTH -> CYCLE_BROKEN" );
}

/* Parent fallback creates an immediate cycle.  start has no owner,
 * parent fallback returns start itself.  The cursor_handle == start
 * guard fires on the entry-step assignment. */
static void test_cef_parent_fallback_creates_immediate_cycle( void )
{
    struct mock_window wins[] = {
        /* start with no owner, parent fallback points back to self */
        { .handle = 0x100, .owner = 0, .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 1 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "parent-loop to self: no anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "parent fallback to start triggers cycle guard" );
}

/* Non-XDG, non-NONE protocol value treated as "has protocol".  The
 * algorithm tests `!= PRESENTATION_PROTOCOL_NONE`, so any future
 * protocol value (or a fixture returning a bogus number) is treated
 * as an exporter.  Pins the contract. */
static void test_cef_non_xdg_protocol_value_treated_as_anchor( void )
{
    struct mock_window wins[] = {
        /* anchor with future / unknown protocol value (255) */
        { .handle = 0x100, .owner = 0,     .protocol = 255,                              .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,       .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "any non-NONE protocol value is treated as anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* start_desktop = NULL disables the cross-desktop guard.  When the
 * algorithm can't determine the start's desktop, it accepts any
 * anchor regardless of the anchor's desktop value. */
static void test_cef_start_desktop_null_disables_cross_desktop_guard( void )
{
    struct mock_window wins[] = {
        /* anchor on desktop B */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_B },
        /* start with NULL desktop - guard disabled */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = NULL },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100,
                   "start_desktop NULL disables guard; cross-desktop anchor accepted" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* start window's own protocol doesn't matter: the algorithm walks
 * straight to the OWNER on the entry step; start is never considered
 * as anchor.  If owner has no protocol, NO_ANCHOR. */
static void test_cef_start_has_protocol_is_ignored( void )
{
    struct mock_window wins[] = {
        /* would-be anchor if start were itself anchorable - but it's not */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* start has XDG export, but we never anchor at start */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0,
                   "start's own protocol is ignored; walk goes to owner which has no proto -> NO_ANCHOR" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no exporter found" );
}

/* All would-be anchors are cross-desktop: walk skips each one, hits
 * MAX_DEPTH, returns CYCLE_BROKEN with no EXPORT_FOUND.  Pins the
 * combination of "walk continues past skipped candidates" + depth
 * limit. */
static void test_cef_all_cross_desktop_anchors_hit_max_depth( void )
{
    enum { CHAIN = PRESENTATION_MAX_DEPTH + 2 };
    struct mock_window wins[CHAIN];
    struct fixture fx;
    unsigned int reason;
    unsigned int anchor;
    int i;

    /* Build a long chain where EVERY node has xdg-foreign export but
     * is on DESKTOP_B.  start is on DESKTOP_A.  Every candidate is
     * skipped, depth hits the limit. */
    for (i = 0; i < CHAIN; i++)
    {
        wins[i].handle    = 0x1000 + i;
        wins[i].owner     = (i + 1 < CHAIN) ? 0x1000 + i + 1 : 0;
        wins[i].parent    = 0;
        wins[i].destroyed = 0;
        wins[i].pid       = 1;
        wins[i].protocol  = (i == 0)
                                ? PRESENTATION_PROTOCOL_NONE   /* start */
                                : PRESENTATION_PROTOCOL_XDG_FOREIGN_V2;
        wins[i].desktop   = (i == 0) ? DESKTOP_A : DESKTOP_B;
    }
    fx.windows = wins;
    fx.count   = CHAIN;

    anchor = resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "all anchors cross-desktop: no resolution" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "MAX_DEPTH reached after skipping every cross-desktop anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND,
                       "no anchor was actually accepted" );
}

/* Stateless across calls: identical fixture, three back-to-back
 * resolves must produce byte-identical results.  Pins that the
 * algorithm carries no implicit state between invocations. */
static void test_cef_stateless_across_repeated_calls( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason1, reason2, reason3, anchor1, anchor2, anchor3;

    anchor1 = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason1 );
    anchor2 = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason2 );
    anchor3 = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason3 );

    ASSERT_EQ_HEX( anchor1, anchor2, "anchor stable across calls 1->2" );
    ASSERT_EQ_HEX( anchor2, anchor3, "anchor stable across calls 2->3" );
    ASSERT_EQ_HEX( reason1, reason2, "reason stable across calls 1->2" );
    ASSERT_EQ_HEX( reason2, reason3, "reason stable across calls 2->3" );
}

/* Steam friends-list pattern: main toplevel -> friends-list popup ->
 * friend's profile sub-popup -> DM panel sub-popup.  4 levels deep,
 * all in the same process (Steam main), only the main toplevel has
 * xdg-foreign. */
static void test_cef_steam_friends_list_4_level_chain( void )
{
    struct mock_window wins[] = {
        /* Steam main window, the only one with xdg-foreign */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* friends list popup */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* friend profile sub-popup */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* DM panel sub-popup (deepest leaf) */
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x400, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "DM panel resolves to Steam main 3 hops up" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_SAME_PROCESS,
                     "all in Steam's process -> SAME_PROCESS set" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_OWNER_CHAIN, "reached via 3-hop owner walk" );
}

/* NULL get_owner callback - one of the three required ops is missing.
 * The early-return guard rejects without touching the chain. */
static void test_cef_null_get_owner_callback_rejects( void )
{
    static const struct presentation_query_ops ops_no_owner = {
        cb_lookup, NULL, cb_get_protocol, cb_get_pid, cb_get_parent, cb_get_desktop
    };
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &ops_no_owner, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "missing required get_owner callback -> NO_ANCHOR" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "reason is exactly NO_ANCHOR" );
}

/* NULL get_protocol callback - same required-callback validation
 * for the third mandatory op. */
static void test_cef_null_get_protocol_callback_rejects( void )
{
    static const struct presentation_query_ops ops_no_proto = {
        cb_lookup, cb_get_owner, NULL, cb_get_pid, cb_get_parent, cb_get_desktop
    };
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &ops_no_proto, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "missing required get_protocol callback -> NO_ANCHOR" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR, "reason is exactly NO_ANCHOR" );
}

/* reason_out is written BEFORE early-return on bad input.  Caller
 * may pass un-initialized memory; the resolver must zero it.
 * Pin the contract by passing pre-filled garbage and verifying it's
 * overwritten to NO_ANCHOR (== 0). */
static void test_cef_reason_out_overwritten_on_early_return( void )
{
    unsigned int reason = 0xDEADBEEF;
    unsigned int anchor = resolve_presentation_parent( 0, 1, NULL, NULL, &reason );
    ASSERT_EQ_HEX( anchor, 0, "NULL ops + zero handle -> no anchor" );
    ASSERT_EQ_HEX( reason, PRESENTATION_REASON_NO_ANCHOR,
                   "reason_out must be overwritten to NO_ANCHOR, not left as garbage" );
}

/* Mid-walk parent fallback (as opposed to entry-step): a node
 * encountered DURING the walk has owner=0 but a valid parent.  The
 * algorithm's in-loop fallback `if (!cursor_handle && ops->get_parent)`
 * must pick up the parent so the walk continues. */
static void test_cef_mid_walk_parent_fallback( void )
{
    struct mock_window wins[] = {
        /* anchor */
        { .handle = 0x100, .owner = 0,     .parent = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* mid-chain node: owner=0 but parent points to anchor */
        { .handle = 0x200, .owner = 0,     .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* start: owner = mid-chain node */
        { .handle = 0x300, .owner = 0x200, .parent = 0,     .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100,
                   "in-loop parent fallback bridges past owner=0 to reach the anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* Cycle back to start at deeper depth (depth 3 vs the existing 1- and
 * 2-window cycle tests).  start -> A -> B -> C -> start.  Walk hits
 * cursor_handle == start_handle at depth 3, CYCLE_BROKEN, no anchor. */
static void test_cef_cycle_back_to_start_at_depth_3( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A }, /* start */
        { .handle = 0x200, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x300, .owner = 0x400, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x400, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A }, /* closes loop */
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "4-window cycle back to start: no anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "CYCLE_BROKEN must fire on deeper cycle to start" );
}

/* Coincidental PID collision: querying pid happens to equal the
 * anchor's pid even though they're conceptually unrelated processes
 * (e.g., process slot reused after the original process died and a
 * new unrelated process got the same numeric pid).  The algorithm
 * has no way to distinguish, so SAME_PROCESS gets set.
 *
 * This is a documentation test - the contract is "match-by-numeric-pid",
 * not "match-by-process-identity". */
static void test_cef_querying_pid_coincidental_collision_sets_same_process( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 42, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 42, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 42, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "anchor resolves" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_SAME_PROCESS,
                     "numeric-pid match sets SAME_PROCESS even if processes are unrelated - documented contract" );
}

/* Browser autofill popup: input element's containing toplevel ->
 * page render subprocess -> frame containing the input -> autofill
 * popup.  4-level same-process chain (CEF spawns autofill popups
 * inside the renderer subprocess, not as a separate Chromium-style
 * popup process). */
static void test_cef_browser_autofill_4_level_same_process( void )
{
    struct mock_window wins[] = {
        /* page main, the exporter */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 7, .desktop = DESKTOP_A },
        /* render subprocess toplevel */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 7, .desktop = DESKTOP_A },
        /* frame holding the input */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 7, .desktop = DESKTOP_A },
        /* autofill popup */
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 7, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x400, 7, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "autofill popup resolves to page main 3 hops up" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_SAME_PROCESS,
                     "all in renderer pid 7, SAME_PROCESS set" );
}

/* Modal cascade (Ubisoft Connect / Steam shopping cart pattern):
 * confirm dialog opens password prompt opens progress dialog.  Each
 * modal owned by the previous; chain depth 3.  Walk from progress
 * dialog reaches main toplevel 3 hops up. */
static void test_cef_modal_password_cascade_3_levels( void )
{
    struct mock_window wins[] = {
        /* main toplevel */
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* "Confirm purchase" modal */
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* "Enter password" modal */
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* "Processing..." progress dialog */
        { .handle = 0x400, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x400, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "progress dialog resolves to main 3 hops up" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_OWNER_CHAIN, "via owner chain" );
}

/* Anchor placed EXACTLY at MAX_DEPTH from start - one position past
 * the boundary.  Walk's `depth < MAX_DEPTH` test exits the loop BEFORE
 * the anchor is processed.  Returns NO_ANCHOR + CYCLE_BROKEN. */
static void test_cef_anchor_just_past_max_depth_fails( void )
{
    /* CHAIN = MAX_DEPTH + 2: indices 0..MAX_DEPTH+1.
     * start at index 0; the exporter is at index MAX_DEPTH+1, i.e.,
     * MAX_DEPTH+1 hops from start, which is one step past the limit. */
    enum { CHAIN = PRESENTATION_MAX_DEPTH + 2 };
    struct mock_window wins[CHAIN];
    struct fixture fx;
    unsigned int reason;
    unsigned int anchor;
    int i;

    for (i = 0; i < CHAIN; i++)
    {
        wins[i].handle    = 0x1000 + i;
        wins[i].owner     = (i + 1 < CHAIN) ? 0x1000 + i + 1 : 0;
        wins[i].parent    = 0;
        wins[i].destroyed = 0;
        wins[i].pid       = 1;
        wins[i].protocol  = (i == CHAIN - 1)
                                ? PRESENTATION_PROTOCOL_XDG_FOREIGN_V2
                                : PRESENTATION_PROTOCOL_NONE;
        wins[i].desktop   = DESKTOP_A;
    }
    fx.windows = wins;
    fx.count   = CHAIN;

    anchor = resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "anchor at depth MAX_DEPTH+1 is just out of reach" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "depth limit hit before reaching anchor" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND,
                       "EXPORT_FOUND must be clear when the anchor was never accepted" );
}

/* Cross-desktop intermediate skipped, then chain cycles back to start.
 * Combines two control-flow paths: the per-step desktop skip AND the
 * cursor_handle == start_handle guard. */
static void test_cef_cross_desktop_skip_then_cycle_back_to_start( void )
{
    struct mock_window wins[] = {
        /* start, on desktop A */
        { .handle = 0x100, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        /* would-be anchor on desktop B - skipped */
        { .handle = 0x200, .owner = 0x300, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_B },
        /* cycles back to start */
        { .handle = 0x300, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0,
                   "skip cross-desktop anchor, then cycle back to start: no anchor" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                     "cycle guard fires after desktop skip" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND,
                       "skipped anchor doesn't count as found" );
}

/* No get_desktop callback at all: cross-desktop check disabled
 * entirely.  Anchor on conceptually-different "desktop" still
 * accepted because the algorithm has no way to compare. */
static void test_cef_no_get_desktop_callback_accepts_any_anchor( void )
{
    /* Custom ops WITHOUT get_desktop. */
    static const struct presentation_query_ops no_desktop_ops = {
        cb_lookup, cb_get_owner, cb_get_protocol, cb_get_pid, cb_get_parent, NULL
    };
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_B },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &no_desktop_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100,
                   "no get_desktop callback -> cross-desktop guard disabled -> anchor accepted" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* ============================================================== */
/*  Invariant pinning between reason flags and returned anchor     */
/* ============================================================== */

/* INVARIANT: anchor-found return path works with NULL reason_out.
 * The success branch is `if (reason_out) *reason_out = reason; return
 * cursor_handle;` - the NULL guard must protect both writes. */
static void test_cef_anchor_found_with_null_reason_out( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, NULL );
    ASSERT_EQ_HEX( anchor, 0x100, "anchor returned correctly with NULL reason_out" );
}

/* INVARIANT: EXPORT_FOUND => anchor != 0.  Whenever the resolver
 * reports an exporter, it must also return a non-zero handle.
 * Pinned across several fixture shapes. */
static void test_cef_invariant_export_found_implies_anchor_nonzero( void )
{
    /* Fixture 1: simple owner. */
    struct mock_window f1[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    /* Fixture 2: deeper chain. */
    struct mock_window f2[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x300, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    /* Fixture 3: parent fallback at entry. */
    struct mock_window f3[] = {
        { .handle = 0x100, .owner = 0, .parent = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0, .parent = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx[] = { { f1, 2 }, { f2, 3 }, { f3, 2 } };
    unsigned int starts[] = { 0x200, 0x300, 0x200 };
    unsigned int pids[]   = { 2,     1,     1 };
    size_t i;

    for (i = 0; i < 3; i++)
    {
        unsigned int reason;
        unsigned int anchor = resolve_presentation_parent( starts[i], pids[i],
                                                            &mock_ops, &fx[i], &reason );
        if (reason & PRESENTATION_REASON_EXPORT_FOUND)
            ASSERT( anchor != 0 );
    }
}

/* INVARIANT: OWNER_CHAIN bit is set iff EXPORT_FOUND bit is set -
 * the algorithm always sets them together (`reason = OWNER_CHAIN |
 * EXPORT_FOUND`). */
static void test_cef_invariant_owner_chain_iff_export_found( void )
{
    /* Fixture 1: anchor found (both should be set). */
    struct mock_window f1[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    /* Fixture 2: no anchor (neither should be set). */
    struct mock_window f2[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    /* Fixture 3: cycle (CYCLE_BROKEN, neither OWNER_CHAIN nor EXPORT_FOUND). */
    struct mock_window f3[] = {
        { .handle = 0x100, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx[] = { { f1, 2 }, { f2, 2 }, { f3, 2 } };
    size_t i;

    for (i = 0; i < 3; i++)
    {
        unsigned int reason;
        resolve_presentation_parent( 0x200, 1, &mock_ops, &fx[i], &reason );
        /* xor of both flags should be 0 (both set or both clear). */
        unsigned int oc = !!(reason & PRESENTATION_REASON_OWNER_CHAIN);
        unsigned int ef = !!(reason & PRESENTATION_REASON_EXPORT_FOUND);
        ASSERT( oc == ef );
    }
}

/* INVARIANT: SAME_PROCESS => EXPORT_FOUND.  SAME_PROCESS is only
 * ever set inside the anchor-found branch, after EXPORT_FOUND was
 * already added to reason.  It must never appear alone. */
static void test_cef_invariant_same_process_implies_export_found( void )
{
    /* Same-process anchor found. */
    struct mock_window f1[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 7, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 7, .desktop = DESKTOP_A },
    };
    /* Same-process no anchor (chain has no protocol anywhere). */
    struct mock_window f2[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 7, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 7, .desktop = DESKTOP_A },
    };
    struct fixture fx[] = { { f1, 2 }, { f2, 2 } };
    size_t i;

    for (i = 0; i < 2; i++)
    {
        unsigned int reason;
        resolve_presentation_parent( 0x200, 7, &mock_ops, &fx[i], &reason );
        if (reason & PRESENTATION_REASON_SAME_PROCESS)
            ASSERT( reason & PRESENTATION_REASON_EXPORT_FOUND );
    }
}

/* INVARIANT: CYCLE_BROKEN and EXPORT_FOUND are mutually exclusive.
 * The anchor-found branch returns BEFORE the post-loop CYCLE_BROKEN
 * fixup; the cycle-detected branch breaks BEFORE entering the
 * anchor-found branch.  They can never coexist on a return. */
static void test_cef_invariant_cycle_broken_excludes_export_found( void )
{
    /* Cycle (CYCLE_BROKEN, no EXPORT_FOUND). */
    struct mock_window f1[] = {
        { .handle = 0x100, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    /* Depth limit (CYCLE_BROKEN, no EXPORT_FOUND). */
    enum { CHAIN = PRESENTATION_MAX_DEPTH + 5 };
    struct mock_window f2[CHAIN];
    struct fixture fx[2] = { { f1, 2 }, { f2, CHAIN } };
    /* Anchor found (EXPORT_FOUND, no CYCLE_BROKEN). */
    struct mock_window f3[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx3 = { f3, 2 };
    int i;
    unsigned int reason;

    /* Populate the depth-limit chain. */
    for (i = 0; i < CHAIN; i++)
    {
        f2[i].handle    = 0x1000 + i;
        f2[i].owner     = (i + 1 < CHAIN) ? 0x1000 + i + 1 : 0;
        f2[i].parent    = 0;
        f2[i].destroyed = 0;
        f2[i].pid       = 1;
        f2[i].protocol  = PRESENTATION_PROTOCOL_NONE;
        f2[i].desktop   = DESKTOP_A;
    }

    /* Run the cycle fixture. */
    resolve_presentation_parent( 0x100, 1, &mock_ops, &fx[0], &reason );
    if (reason & PRESENTATION_REASON_CYCLE_BROKEN)
        ASSERT( !(reason & PRESENTATION_REASON_EXPORT_FOUND) );

    /* Run the depth-limit fixture. */
    resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx[1], &reason );
    if (reason & PRESENTATION_REASON_CYCLE_BROKEN)
        ASSERT( !(reason & PRESENTATION_REASON_EXPORT_FOUND) );

    /* Run the anchor-found fixture. */
    resolve_presentation_parent( 0x200, 1, &mock_ops, &fx3, &reason );
    if (reason & PRESENTATION_REASON_EXPORT_FOUND)
        ASSERT( !(reason & PRESENTATION_REASON_CYCLE_BROKEN) );
}

/* INVARIANT: anchor == 0 => reason has no EXPORT_FOUND bit.  If the
 * resolver returns zero, EXPORT_FOUND must be clear (otherwise the
 * caller would think an anchor existed). */
static void test_cef_invariant_anchor_zero_no_export_found( void )
{
    struct mock_window f1[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    struct mock_window f2[] = {
        { .handle = 0x100, .owner = 0x200, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    /* Cross-desktop only - anchor present in chain but skipped. */
    struct mock_window f3[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_B },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx[] = { { f1, 2 }, { f2, 2 }, { f3, 2 } };
    size_t i;

    for (i = 0; i < 3; i++)
    {
        unsigned int reason;
        unsigned int anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx[i], &reason );
        if (anchor == 0)
            ASSERT( !(reason & PRESENTATION_REASON_EXPORT_FOUND) );
    }
}

/* Both querying_pid AND anchor pid are 0.  The SAME_PROCESS guard
 * checks `querying_pid && ops->get_pid && anchor_pid && anchor_pid ==
 * querying_pid`.  With querying_pid == 0, the guard short-circuits
 * BEFORE reading anchor_pid, so the anchor_pid value (also 0) is
 * never even checked. */
static void test_cef_both_pids_zero_no_same_process( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 0, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 0, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 0, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "anchor resolves with both pids = 0" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_SAME_PROCESS,
                       "querying_pid=0 short-circuits before anchor_pid check" );
}

/* When a window has BOTH a non-zero owner AND a non-zero parent at
 * the ENTRY step, the resolver prefers owner.  Pin the precedence:
 * the parent fallback only fires when owner == 0. */
static void test_cef_entry_step_owner_preferred_over_parent( void )
{
    struct mock_window wins[] = {
        /* candidate anchor via OWNER */
        { .handle = 0x100, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* decoy anchor via PARENT - should NOT be visited */
        { .handle = 0x101, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 99, .desktop = DESKTOP_A },
        /* start has BOTH owner and parent set */
        { .handle = 0x200, .owner = 0x100, .parent = 0x101, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "entry step: owner takes precedence over parent" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* Same precedence rule in the IN-LOOP fallback: when a node mid-walk
 * has both owner != 0 AND parent != 0, owner wins.  Distinct from the
 * entry-step test above. */
static void test_cef_in_loop_owner_preferred_over_parent( void )
{
    struct mock_window wins[] = {
        /* deepest anchor via OWNER chain */
        { .handle = 0x100, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        /* decoy anchor via PARENT fallback - should NOT be visited */
        { .handle = 0x101, .owner = 0, .parent = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 99, .desktop = DESKTOP_A },
        /* intermediate mid-chain with BOTH owner and parent set */
        { .handle = 0x200, .owner = 0x100, .parent = 0x101, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 2, .desktop = DESKTOP_A },
        /* start */
        { .handle = 0x300, .owner = 0x200, .parent = 0,     .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 3, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 4 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x300, 3, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "in-loop step: owner takes precedence over parent" );
    ASSERT_FLAG_SET( reason, PRESENTATION_REASON_EXPORT_FOUND, "EXPORT_FOUND set" );
}

/* ============================================================== */
/*  Stress / fuzz / concurrent broker tests                        */
/* ============================================================== */

#include <pthread.h>
#include <time.h>

/* Stress: 100,000 resolves on a small fixture.  Pins that the
 * algorithm has no per-call resource leak (FDs, heap, etc.) and
 * that performance is acceptable (no accidental quadratic behavior
 * over many calls). */
static void test_cef_stress_100k_resolves( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason, anchor;
    int i;

    for (i = 0; i < 100000; i++)
    {
        anchor = resolve_presentation_parent( 0x200, 2, &mock_ops, &fx, &reason );
        if (anchor != 0x100 || !(reason & PRESENTATION_REASON_EXPORT_FOUND))
        {
            ASSERT_EQ_HEX( anchor, 0x100, "all 100k resolves must return same anchor" );
            return;
        }
    }
}

/* Stress: 1000 different MAX_DEPTH chains, each fully walked.
 * Pins no stack growth, no integer overflow on the depth counter,
 * and that the depth-limit guard fires correctly every time. */
static void test_cef_stress_1000_max_depth_walks( void )
{
    enum { CHAIN = PRESENTATION_MAX_DEPTH + 5 };
    struct mock_window wins[CHAIN];
    struct fixture fx = { wins, CHAIN };
    unsigned int reason;
    int run, i;

    for (i = 0; i < CHAIN; i++)
    {
        wins[i].owner     = (i + 1 < CHAIN) ? 0x10000 + i + 1 : 0;
        wins[i].parent    = 0;
        wins[i].destroyed = 0;
        wins[i].pid       = 1;
        wins[i].protocol  = PRESENTATION_PROTOCOL_NONE;
        wins[i].desktop   = DESKTOP_A;
    }

    for (run = 0; run < 1000; run++)
    {
        /* Vary the handle base to force fresh fixture search each run. */
        for (i = 0; i < CHAIN; i++)
        {
            wins[i].handle = 0x10000 + i + run * CHAIN;
            wins[i].owner  = (i + 1 < CHAIN) ? wins[i].handle + 1 : 0;
        }
        ASSERT_EQ_HEX( resolve_presentation_parent( wins[0].handle, 1, &mock_ops, &fx, &reason ),
                       0, "MAX_DEPTH chain returns no anchor" );
        ASSERT_FLAG_SET( reason, PRESENTATION_REASON_CYCLE_BROKEN,
                         "depth limit fires every iteration" );
    }
}

/* Concurrent resolves on the SAME fixture from N threads.  Algorithm
 * is documented stateless; this pins it.  Each thread must observe
 * the SAME anchor.  Catches accidental statics, TLS gone wrong, etc. */
struct concurrent_args
{
    struct fixture *fx;
    unsigned int start_handle;
    unsigned int querying_pid;
    unsigned int expected_anchor;
    int failed;
};

static void *concurrent_resolver( void *arg )
{
    struct concurrent_args *args = arg;
    unsigned int reason;
    int i;
    for (i = 0; i < 10000; i++)
    {
        unsigned int got = resolve_presentation_parent(
            args->start_handle, args->querying_pid, &mock_ops, args->fx, &reason );
        if (got != args->expected_anchor) { args->failed = 1; return NULL; }
    }
    return NULL;
}

static void test_cef_concurrent_resolves_same_fixture( void )
{
    enum { THREADS = 8 };
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    struct concurrent_args args[THREADS];
    pthread_t threads[THREADS];
    int i;

    for (i = 0; i < THREADS; i++)
    {
        args[i].fx               = &fx;
        args[i].start_handle     = 0x200;
        args[i].querying_pid     = 2;
        args[i].expected_anchor  = 0x100;
        args[i].failed           = 0;
        pthread_create( &threads[i], NULL, concurrent_resolver, &args[i] );
    }
    for (i = 0; i < THREADS; i++)
        pthread_join( threads[i], NULL );

    for (i = 0; i < THREADS; i++)
        ASSERT( args[i].failed == 0 );
}

/* Concurrent resolves with a mutating fixture (one thread changes
 * an owner reference, others resolve).  No crash, and every resolve
 * must return either the pre-mutation anchor or the post-mutation
 * anchor - never garbage.  The mock fixture is racy by design; this
 * pins that the algorithm itself doesn't propagate the race into
 * undefined behavior. */
struct mutating_state
{
    struct mock_window *wins;
    int                 stop;
};

static void *fixture_mutator( void *arg )
{
    struct mutating_state *st = arg;
    int i;
    for (i = 0; !st->stop && i < 100000; i++)
        st->wins[2].owner = (i & 1) ? 0x100 : 0x101;
    return NULL;
}

struct racy_resolver_args
{
    struct fixture *fx;
    int             crash;
};

static void *racy_resolver( void *arg )
{
    struct racy_resolver_args *args = arg;
    unsigned int reason;
    int i;
    for (i = 0; i < 50000; i++)
    {
        unsigned int got = resolve_presentation_parent( 0x200, 2, &mock_ops, args->fx, &reason );
        /* Result must be one of the two valid anchors (or 0 if the
         * mutator picked an unset owner momentarily).  Anything else
         * would mean the algorithm produced garbage. */
        if (got != 0 && got != 0x100 && got != 0x101) { args->crash = 1; return NULL; }
    }
    return NULL;
}

static void test_cef_concurrent_resolve_with_mutating_owner( void )
{
    enum { RESOLVERS = 4 };
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x101, .owner = 0, .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,        .pid = 2, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 3 };
    struct mutating_state mut_st = { wins, 0 };
    struct racy_resolver_args args[RESOLVERS] = { {0} };
    pthread_t mutator, resolvers[RESOLVERS];
    int i;

    for (i = 0; i < RESOLVERS; i++) args[i].fx = &fx;

    pthread_create( &mutator, NULL, fixture_mutator, &mut_st );
    for (i = 0; i < RESOLVERS; i++)
        pthread_create( &resolvers[i], NULL, racy_resolver, &args[i] );

    for (i = 0; i < RESOLVERS; i++)
        pthread_join( resolvers[i], NULL );
    mut_st.stop = 1;
    pthread_join( mutator, NULL );

    for (i = 0; i < RESOLVERS; i++)
        ASSERT( args[i].crash == 0 );
}

/* Fuzz: generate 1000 random fixtures with random chain shapes and
 * resolve from a random start handle.  Verify the cross-cutting
 * invariants always hold (EXPORT_FOUND => anchor != 0, CYCLE_BROKEN
 * and EXPORT_FOUND are mutually exclusive, etc.). */
static unsigned int fuzz_rand_state = 1;

static unsigned int fuzz_rand( void )
{
    /* xorshift32 - deterministic and fast. */
    fuzz_rand_state ^= fuzz_rand_state << 13;
    fuzz_rand_state ^= fuzz_rand_state >> 17;
    fuzz_rand_state ^= fuzz_rand_state << 5;
    return fuzz_rand_state;
}

static void test_cef_fuzz_random_fixtures_preserve_invariants( void )
{
    enum { FIXTURES = 1000, MAX_WIN = 24 };
    struct mock_window wins[MAX_WIN];
    struct fixture fx;
    int run, i;
    unsigned int reason, anchor;

    fuzz_rand_state = 0xC0FFEE;  /* deterministic seed for reproducibility */

    for (run = 0; run < FIXTURES; run++)
    {
        unsigned int count = 2 + (fuzz_rand() % (MAX_WIN - 2));
        unsigned int start = 0x1000 + (fuzz_rand() % count);
        unsigned int qpid  = 1 + (fuzz_rand() % 4);

        for (i = 0; i < (int)count; i++)
        {
            unsigned int r = fuzz_rand();
            wins[i].handle    = 0x1000 + i;
            /* Random owner: either 0 (chain end) or another node */
            wins[i].owner     = (r & 0x3) ? 0x1000 + (r % count) : 0;
            wins[i].parent    = (r >> 4) & 1 ? 0x1000 + ((r >> 8) % count) : 0;
            wins[i].destroyed = ((r >> 16) & 0xF) == 0;  /* ~6% destroyed */
            wins[i].pid       = 1 + ((r >> 20) & 3);
            wins[i].protocol  = (r >> 24) & 1
                                    ? PRESENTATION_PROTOCOL_XDG_FOREIGN_V2
                                    : PRESENTATION_PROTOCOL_NONE;
            wins[i].desktop   = (r >> 25) & 1 ? DESKTOP_A : DESKTOP_B;
        }
        fx.windows = wins;
        fx.count   = count;

        anchor = resolve_presentation_parent( start, qpid, &mock_ops, &fx, &reason );

        /* Invariant 1: EXPORT_FOUND => anchor != 0 */
        if (reason & PRESENTATION_REASON_EXPORT_FOUND) ASSERT( anchor != 0 );
        /* Invariant 2: anchor != 0 => EXPORT_FOUND */
        if (anchor != 0) ASSERT( reason & PRESENTATION_REASON_EXPORT_FOUND );
        /* Invariant 3: OWNER_CHAIN iff EXPORT_FOUND */
        ASSERT( !!(reason & PRESENTATION_REASON_OWNER_CHAIN) ==
                !!(reason & PRESENTATION_REASON_EXPORT_FOUND) );
        /* Invariant 4: SAME_PROCESS => EXPORT_FOUND */
        if (reason & PRESENTATION_REASON_SAME_PROCESS)
            ASSERT( reason & PRESENTATION_REASON_EXPORT_FOUND );
        /* Invariant 5: CYCLE_BROKEN and EXPORT_FOUND are disjoint */
        ASSERT( !((reason & PRESENTATION_REASON_CYCLE_BROKEN) &&
                  (reason & PRESENTATION_REASON_EXPORT_FOUND)) );
    }
}

/* Stress + fuzz combo: 10,000 random fixtures, each resolved 10
 * times - must produce the same answer each time (stateless contract
 * under randomized inputs). */
static void test_cef_fuzz_stateless_under_random_fixtures( void )
{
    enum { FIXTURES = 10000, MAX_WIN = 12, REPEAT = 10 };
    struct mock_window wins[MAX_WIN];
    struct fixture fx;
    int run, rep, i;
    unsigned int reason, anchor, first_anchor, first_reason;

    fuzz_rand_state = 0xBEEFDEAD;

    for (run = 0; run < FIXTURES; run++)
    {
        unsigned int count = 2 + (fuzz_rand() % (MAX_WIN - 2));
        for (i = 0; i < (int)count; i++)
        {
            unsigned int r = fuzz_rand();
            wins[i].handle    = 0x1000 + i;
            wins[i].owner     = (r & 0x3) ? 0x1000 + (r % count) : 0;
            wins[i].parent    = 0;
            wins[i].destroyed = 0;
            wins[i].pid       = 1 + ((r >> 8) & 3);
            wins[i].protocol  = (r >> 16) & 1
                                    ? PRESENTATION_PROTOCOL_XDG_FOREIGN_V2
                                    : PRESENTATION_PROTOCOL_NONE;
            wins[i].desktop   = DESKTOP_A;
        }
        fx.windows = wins;
        fx.count   = count;

        first_anchor = resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx, &first_reason );
        for (rep = 0; rep < REPEAT - 1; rep++)
        {
            anchor = resolve_presentation_parent( 0x1000, 1, &mock_ops, &fx, &reason );
            if (anchor != first_anchor || reason != first_reason)
            {
                ASSERT_EQ_HEX( anchor, first_anchor, "stateless: anchor must be stable" );
                ASSERT_EQ_HEX( reason, first_reason, "stateless: reason must be stable" );
                return;
            }
        }
    }
}

/* Parent-fallback returns a dangling (non-existent in fixture)
 * handle.  Walk lookup fails on that handle, chain terminates
 * cleanly with NO_ANCHOR.  Complements existing dangling-OWNER
 * tests by testing the dangling-PARENT path specifically. */
static void test_cef_parent_fallback_returns_dangling_handle( void )
{
    struct mock_window wins[] = {
        /* start: owner = 0, parent = 0x999 (not in fixture) */
        { .handle = 0x100, .owner = 0, .parent = 0x999, .protocol = PRESENTATION_PROTOCOL_NONE, .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 1 };
    unsigned int reason;
    unsigned int anchor = resolve_presentation_parent( 0x100, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "dangling parent handle terminates walk cleanly" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_CYCLE_BROKEN, "clean end, not cycle" );
    ASSERT_FLAG_CLEAR( reason, PRESENTATION_REASON_EXPORT_FOUND, "no exporter found" );
}

/* Bonus: exporter acquires export, clears, re-acquires.  Three
 * consecutive resolves return Anchor / NO_ANCHOR / Anchor.
 * Validates that the algorithm doesn't carry any per-call state. */
static void test_cef_export_clear_re_export_cycle( void )
{
    struct mock_window wins[] = {
        { .handle = 0x100, .owner = 0,     .protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2, .pid = 1, .desktop = DESKTOP_A },
        { .handle = 0x200, .owner = 0x100, .protocol = PRESENTATION_PROTOCOL_NONE,           .pid = 1, .desktop = DESKTOP_A },
    };
    struct fixture fx = { wins, 2 };
    unsigned int reason, anchor;

    anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "1st resolve: exporter alive -> anchor" );

    wins[0].protocol = PRESENTATION_PROTOCOL_NONE;
    anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0, "2nd resolve: exporter cleared -> no anchor" );

    wins[0].protocol = PRESENTATION_PROTOCOL_XDG_FOREIGN_V2;
    anchor = resolve_presentation_parent( 0x200, 1, &mock_ops, &fx, &reason );
    ASSERT_EQ_HEX( anchor, 0x100, "3rd resolve: exporter re-acquired -> anchor" );
}

int main( void )
{
    RUN_TEST( test_no_owner );
    RUN_TEST( test_simple_owner );
    RUN_TEST( test_skip_invisible_helper );
    RUN_TEST( test_cross_process );
    RUN_TEST( test_dangling_owner );
    RUN_TEST( test_chain_no_exporter );
    RUN_TEST( test_cycle_detection );
    RUN_TEST( test_max_depth );
    RUN_TEST( test_querying_pid_zero );
    RUN_TEST( test_nearest_exporter );
    RUN_TEST( test_bad_inputs );
    RUN_TEST( test_same_process_only_with_export );
    RUN_TEST( test_child_parent_fallback );
    RUN_TEST( test_owner_priority_over_parent );
    RUN_TEST( test_no_parent_callback_owner_only );
    RUN_TEST( test_cross_desktop_rejected );
    RUN_TEST( test_same_desktop_accepted );
    RUN_TEST( test_chain_skips_other_desktop );

    /* Realistic CEF / Chromium broker scenarios. */
    RUN_TEST( test_cef_4_process_modal_chain );
    RUN_TEST( test_cef_sibling_modals_same_anchor );
    RUN_TEST( test_cef_mid_chain_destroyed );
    RUN_TEST( test_cef_destroyed_start_window );
    RUN_TEST( test_cef_xdg_at_max_depth_boundary_succeeds );
    RUN_TEST( test_cef_alternating_proto_picks_nearest_export );
    RUN_TEST( test_cef_pid_zero_anchor_clears_same_process );
    RUN_TEST( test_cef_minimum_viable_ops );
    RUN_TEST( test_cef_cross_process_via_parent_fallback );
    RUN_TEST( test_cef_unknown_desktop_anchor_rejected );
    RUN_TEST( test_cef_parent_then_owner_chain );
    RUN_TEST( test_cef_cycle_via_three_window_loop );

    /* Dynamic mutation scenarios (review batch T31-T42). */
    RUN_TEST( test_cef_owner_to_parent_transition );
    RUN_TEST( test_cef_mid_chain_acquires_export );
    RUN_TEST( test_cef_multiple_intermediates_progressive_export );
    RUN_TEST( test_cef_exporter_becomes_unexported );
    RUN_TEST( test_cef_chain_ends_at_desktop_window );
    RUN_TEST( test_cef_cross_desktop_then_return_to_start_desktop );
    RUN_TEST( test_cef_depth_3_all_none_except_deepest );
    RUN_TEST( test_cef_mid_chain_terminates_with_no_owner_no_parent );
    RUN_TEST( test_cef_destroyed_blocks_walk_to_exporter_above );
    RUN_TEST( test_cef_export_clear_re_export_cycle );

    /* Third pass: algorithm-edge + realistic-pattern scenarios. */
    RUN_TEST( test_cef_self_owner_at_start );
    RUN_TEST( test_cef_self_owner_at_mid_chain_hits_depth_limit );
    RUN_TEST( test_cef_parent_fallback_creates_immediate_cycle );
    RUN_TEST( test_cef_non_xdg_protocol_value_treated_as_anchor );
    RUN_TEST( test_cef_start_desktop_null_disables_cross_desktop_guard );
    RUN_TEST( test_cef_start_has_protocol_is_ignored );
    RUN_TEST( test_cef_all_cross_desktop_anchors_hit_max_depth );
    RUN_TEST( test_cef_stateless_across_repeated_calls );
    RUN_TEST( test_cef_steam_friends_list_4_level_chain );
    RUN_TEST( test_cef_no_get_desktop_callback_accepts_any_anchor );

    /* Fourth pass: required-callback validation + contract pinning +
     * deeper cycle / boundary cases + more realistic patterns. */
    RUN_TEST( test_cef_null_get_owner_callback_rejects );
    RUN_TEST( test_cef_null_get_protocol_callback_rejects );
    RUN_TEST( test_cef_reason_out_overwritten_on_early_return );
    RUN_TEST( test_cef_mid_walk_parent_fallback );
    RUN_TEST( test_cef_cycle_back_to_start_at_depth_3 );
    RUN_TEST( test_cef_querying_pid_coincidental_collision_sets_same_process );
    RUN_TEST( test_cef_browser_autofill_4_level_same_process );
    RUN_TEST( test_cef_modal_password_cascade_3_levels );
    RUN_TEST( test_cef_anchor_just_past_max_depth_fails );
    RUN_TEST( test_cef_cross_desktop_skip_then_cycle_back_to_start );

    /* Fifth pass: invariant pinning between reason flags + anchor value. */
    RUN_TEST( test_cef_both_pids_zero_no_same_process );
    RUN_TEST( test_cef_entry_step_owner_preferred_over_parent );
    RUN_TEST( test_cef_in_loop_owner_preferred_over_parent );
    RUN_TEST( test_cef_anchor_found_with_null_reason_out );
    RUN_TEST( test_cef_invariant_export_found_implies_anchor_nonzero );
    RUN_TEST( test_cef_invariant_owner_chain_iff_export_found );
    RUN_TEST( test_cef_invariant_same_process_implies_export_found );
    RUN_TEST( test_cef_invariant_cycle_broken_excludes_export_found );
    RUN_TEST( test_cef_invariant_anchor_zero_no_export_found );
    RUN_TEST( test_cef_parent_fallback_returns_dangling_handle );

    /* Sixth pass: stress / fuzz / concurrent. */
    RUN_TEST( test_cef_stress_100k_resolves );
    RUN_TEST( test_cef_stress_1000_max_depth_walks );
    RUN_TEST( test_cef_concurrent_resolves_same_fixture );
    RUN_TEST( test_cef_concurrent_resolve_with_mutating_owner );
    RUN_TEST( test_cef_fuzz_random_fixtures_preserve_invariants );
    RUN_TEST( test_cef_fuzz_stateless_under_random_fixtures );

    fprintf( stderr, "%d test(s) run, %d failure(s).\n", tests_run, tests_failed );
    return tests_failed ? 1 : 0;
}
