/*
 * Server-side presentation-relation broker - resolution algorithm
 *
 * resolve_presentation_parent is split out from window.c so it can be
 * unit-tested without spinning up a wineserver instance. The algorithm
 * walks the owner chain via callback-supplied lookups; production
 * wires the callbacks to struct window and the test harness wires
 * them to a fixture array.
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

#include "presentation.h"

#include <stddef.h>

unsigned int resolve_presentation_parent(
    unsigned int start_handle,
    unsigned int querying_pid,
    const struct presentation_query_ops *ops,
    void *ctx,
    unsigned int *reason_out )
{
    void *cursor;
    unsigned int cursor_handle;
    unsigned int reason = PRESENTATION_REASON_NO_ANCHOR;
    const void *start_desktop = NULL;
    int depth = 0;

    if (reason_out) *reason_out = reason;
    if (!start_handle || !ops || !ops->lookup || !ops->get_owner || !ops->get_protocol)
        return 0;

    /* Step from `start` to its owner; we never resolve to `start` itself.
     *
     * When the owner chain runs dry (GW_OWNER == 0) and the cursor is
     * a WS_CHILD window, fall back to the GW_CHILD parent before
     * giving up. This catches CEF/Chromium frames where the popup is
     * attached as a child of a child rather than owned by the top-level
     * frame. Callers that don't supply get_parent get the original
     * owner-only behaviour. */
    cursor = ops->lookup( start_handle, ctx );
    if (!cursor) return 0;

    /* Snapshot the start window's desktop for cross-desktop validation.
     * Callers that don't supply get_desktop accept any anchor. */
    if (ops->get_desktop) start_desktop = ops->get_desktop( cursor, ctx );

    cursor_handle = ops->get_owner( cursor, ctx );
    if (!cursor_handle && ops->get_parent)
        cursor_handle = ops->get_parent( cursor, ctx );

    while (cursor_handle && depth < PRESENTATION_MAX_DEPTH)
    {
        /* Self-cycle short-circuit: if the owner chain comes back to
         * the start window, we have a cycle in the data. set_window_owner
         * already rejects cycles, but defend against bad state. */
        if (cursor_handle == start_handle)
        {
            reason |= PRESENTATION_REASON_CYCLE_BROKEN;
            break;
        }

        cursor = ops->lookup( cursor_handle, ctx );
        if (!cursor) break;  /* dangling owner handle - treat as chain end */

        if (ops->get_protocol( cursor, ctx ) != PRESENTATION_PROTOCOL_NONE)
        {
            /* Cross-desktop guard: a window must not resolve to an
             * anchor on a different desktop. Treat the candidate as if
             * it had PROTOCOL_NONE and keep walking; the chain might
             * still cross back into the start's desktop at a higher
             * level (rare but legal under owner-tree mutation). */
            if (start_desktop && ops->get_desktop &&
                ops->get_desktop( cursor, ctx ) != start_desktop)
                goto skip_anchor;

            /* Found a viable anchor. */
            reason = PRESENTATION_REASON_OWNER_CHAIN |
                     PRESENTATION_REASON_EXPORT_FOUND;

            /* SAME_PROCESS is a hint for the driver to skip the
             * xdg_foreign import path and link directly. Only set it
             * when an anchor was actually found - on NO_ANCHOR there
             * is nothing to link to, so the flag is meaningless and
             * would just be noise for callers. */
            if (querying_pid && ops->get_pid)
            {
                unsigned int anchor_pid = ops->get_pid( cursor, ctx );
                if (anchor_pid && anchor_pid == querying_pid)
                    reason |= PRESENTATION_REASON_SAME_PROCESS;
            }

            if (reason_out) *reason_out = reason;
            return cursor_handle;
        }

skip_anchor:
        /* Walk past invisible/no-export ancestors. Same owner-then-
         * parent fallback as the entry step. */
        cursor_handle = ops->get_owner( cursor, ctx );
        if (!cursor_handle && ops->get_parent)
            cursor_handle = ops->get_parent( cursor, ctx );
        depth++;
    }

    if (depth >= PRESENTATION_MAX_DEPTH)
        reason |= PRESENTATION_REASON_CYCLE_BROKEN;

    if (reason_out) *reason_out = reason;
    return 0;
}
