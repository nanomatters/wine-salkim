/*
 * Server-side presentation-relation broker
 *
 * Cross-process transient-parent resolution for the Wayland driver.
 * The wineserver already owns the authoritative HWND owner tree;
 * this module exposes that tree to per-process Wayland drivers via
 * tokens stored on each window (see struct presentation_export in
 * window.c).
 *
 * The core resolver is split out from window.c so it can be unit-tested
 * with mocked window data. Callbacks abstract away struct window so the
 * test harness can plug in fixture data.
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

#ifndef __WINE_SERVER_PRESENTATION_H
#define __WINE_SERVER_PRESENTATION_H

/* These constants are also defined in server/protocol.def - the
 * generated server_protocol.h provides them as enum values. The
 * fallback below is for the standalone unit test, which doesn't pull
 * in server_protocol.h. The values must stay in lock-step with
 * protocol.def. */
#ifndef __WINE_WINE_SERVER_PROTOCOL_H
#define PRESENTATION_PROTOCOL_NONE              0
#define PRESENTATION_PROTOCOL_XDG_FOREIGN_V2    1

#define PRESENTATION_REASON_NO_ANCHOR     0x00000000
#define PRESENTATION_REASON_OWNER_CHAIN   0x00000001  /* anchor reached via GW_OWNER walk */
#define PRESENTATION_REASON_EXPORT_FOUND  0x00000002  /* anchor has a non-NONE protocol */
#define PRESENTATION_REASON_SAME_PROCESS  0x00000004  /* anchor is in the querying window's process */
#define PRESENTATION_REASON_CYCLE_BROKEN  0x00000008  /* depth limit hit; tree had a cycle */
#endif

/* Maximum depth of an owner chain before we declare a cycle. CEF and
 * Win32 apps in the wild rarely exceed 4 levels; 32 is a safe upper
 * bound that any real chain will never legitimately reach. */
#define PRESENTATION_MAX_DEPTH 32

/* Callback interface used by resolve_presentation_parent so the
 * algorithm can be unit-tested with fixture data. Each callback
 * takes an opaque "node token" - production sets this to a
 * struct window pointer; tests set it to an array index, etc.
 *
 * All callbacks must be cheap and side-effect-free. None of them are
 * called from a path that holds extra resources. */
struct presentation_query_ops
{
    /* Look up a node by handle. Returns NULL if no such node exists.
     * For production, this is `get_window( handle )` cast to void *. */
    void *(*lookup)( unsigned int handle, void *ctx );

    /* Get the owner handle stored on a node. Returns 0 if the node has
     * no owner. */
    unsigned int (*get_owner)( void *node, void *ctx );

    /* Get the presentation protocol of a node. Returns
     * PRESENTATION_PROTOCOL_NONE if the node has no presentation_export. */
    unsigned int (*get_protocol)( void *node, void *ctx );

    /* Get the process id associated with a node. Used solely for
     * SAME_PROCESS detection. Returns 0 if not known. */
    unsigned int (*get_pid)( void *node, void *ctx );

    /* Get the GW_CHILD parent handle of a node, or 0 if the node is
     * not a WS_CHILD window (or has no parent in the broker's sense).
     *
     * Used as a fallback when an owner-chain step yields 0 (no
     * GW_OWNER): the resolver retries with GW_CHILD's parent so
     * popups/menus that hang off WS_CHILD frames still resolve to a
     * viable anchor. May be NULL; callers that don't supply this
     * callback only walk the owner chain.
     *
     * Production must return 0 for desktop windows and for non-child
     * windows (those have an irrelevant parent pointer that would
     * cause the resolver to walk into the desktop tree). */
    unsigned int (*get_parent)( void *node, void *ctx );

    /* Get an opaque "desktop identity" pointer for a node. The resolver
     * compares it for equality only - never dereferences.
     *
     * Used to reject cross-desktop anchors: a window on the secondary
     * desktop must not resolve to an anchor on the primary desktop,
     * and vice versa. Returns NULL for nodes whose desktop is unknown.
     *
     * May itself be NULL; callers without multi-desktop concerns can
     * leave this slot empty and the resolver accepts any anchor
     * regardless of desktop membership. */
    const void *(*get_desktop)( void *node, void *ctx );
};

/* Walk the owner chain of `start_handle` looking for the first
 * ancestor whose presentation_export protocol is not PROTOCOL_NONE.
 *
 * Ancestors with no export or PROTOCOL_NONE are skipped. Cycles are
 * detected via PRESENTATION_MAX_DEPTH.
 *
 * Returns the resolved anchor handle (or 0 if none found) and writes
 * a bitmask of PRESENTATION_REASON_* flags to *reason_out describing
 * how the answer was reached.
 *
 * `querying_pid` is used only to set PRESENTATION_REASON_SAME_PROCESS
 * when the resolved anchor is in the same process. Pass 0 to disable
 * that check.
 *
 * `ops` and `ctx` provide the abstract node lookup. `ctx` is passed
 * unchanged to every callback. */
extern unsigned int resolve_presentation_parent(
    unsigned int start_handle,
    unsigned int querying_pid,
    const struct presentation_query_ops *ops,
    void *ctx,
    unsigned int *reason_out );

/* Forward declaration so the wrapper below can be typed without
 * pulling in window.c's full struct definition. presentation_test.c
 * sees only the incomplete type, which is fine: tests call the
 * abstract resolve_presentation_parent, not this wrapper. */
struct window;

/* Production wrapper around resolve_presentation_parent - wires the
 * server's struct window to the callback API. Defined in window.c.
 * Used by the get_window_presentation_parent request handler. */
extern struct window *resolve_window_presentation_parent_handle( unsigned int handle,
                                                                 unsigned int *reason_out );

#endif  /* __WINE_SERVER_PRESENTATION_H */
