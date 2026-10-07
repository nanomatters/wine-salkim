#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check layered metadata ordering in the actual win32u entry point."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


SOURCE = Path(os.environ.get("WIN32U_LAYERED_SOURCE", str(
    Path(__file__).resolve().parents[2] / "dlls/win32u/window.c")))


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define WINAPI
#define TRUE 1
#define FALSE 0
#define SWP_NOSIZE 1
#define SWP_NOMOVE 2
#define SWP_NOZORDER 4
#define SWP_NOACTIVATE 8
#define SWP_NOREDRAW 16
#define ULW_COLORKEY 1
#define ULW_ALPHA 2
#define ULW_OPAQUE 4
#define ULW_EX_NORESIZE 8
#define WS_EX_LAYERED 0x80000
#define GWL_EXSTYLE (-20)
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INCORRECT_SIZE 1462
#define COORDS_PARENT 0
#define AC_SRC_OVER 0
#define BLACKNESS 0
#define NtGdiDPtoLP 0
#define CLR_INVALID UINT32_MAX
#define TRACE(...) ((void)0)

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint32_t DWORD, COLORREF;
typedef void *HWND, *HDC;
typedef struct { int32_t x, y; } POINT;
typedef struct { int32_t cx, cy; } SIZE;
typedef struct { int32_t left, top, right, bottom; } RECT;
typedef struct { BYTE BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; } BLENDFUNCTION;
struct window_rects { RECT window, client, visible; };
struct window_surface { RECT bounds; void *color_bitmap; DWORD alpha_mask; COLORREF key; };
static struct window_surface dummy_surface, test_surface;
static struct window_surface *returned_surface;
static BOOL layered_style, attributes, fail_dc, fail_blend, expected_layered;
static unsigned int position_calls, metadata_calls, updates, flushes, releases, locks, blends;
static DWORD last_error;
static COLORREF expected_key;
static BYTE expected_alpha;

static DWORD get_window_long(HWND hwnd, int index)
{ (void)hwnd; (void)index; return layered_style ? WS_EX_LAYERED : 0; }
static BOOL NtUserGetLayeredWindowAttributes(HWND hwnd, void *key, void *alpha, void *flags)
{ (void)hwnd; (void)key; (void)alpha; (void)flags; return attributes; }
static void RtlSetLastWin32Error(DWORD error) { last_error = error; }
static unsigned int get_thread_dpi(void) { return 96; }
static void get_window_rects(HWND hwnd, int coords, struct window_rects *rects, unsigned int dpi)
{
    (void)hwnd; (void)coords; (void)dpi;
    rects->window = rects->client = rects->visible = (RECT){0, 0, 100, 100};
}
static void OffsetRect(RECT *rect, int x, int y)
{ rect->left += x; rect->right += x; rect->top += y; rect->bottom += y; }
static struct window_surface *get_window_surface(HWND hwnd, DWORD flags, BOOL create,
        struct window_rects *rects, RECT *surface_rect)
{ (void)hwnd; (void)flags; (void)create; *surface_rect = rects->window; return returned_surface; }
static void window_surface_set_layered(struct window_surface *surface, COLORREF key, int alpha, DWORD mask)
{
    assert(surface && surface != &dummy_surface && alpha == -1 && mask == 0xff000000);
    surface->alpha_mask = mask;
    surface->key = key;
    ++metadata_calls;
}
static void apply_window_pos(HWND hwnd, HWND after, DWORD flags, struct window_surface *surface,
        struct window_rects *rects, void *valid)
{
    (void)hwnd; (void)after; (void)flags; (void)rects; (void)valid;
    ++position_calls;
    assert((!!(surface && surface->alpha_mask)) == expected_layered);
    if (expected_layered) assert(surface->key == expected_key);
}
static void update_layered(HWND hwnd, BYTE alpha, DWORD flags)
{ (void)hwnd; (void)flags; assert(alpha == expected_alpha); ++updates; }
static const struct { void (*pUpdateLayeredWindow)(HWND, BYTE, DWORD); }
    driver = {update_layered}, *user_driver = &driver;
static BOOL intersect_rect(RECT *out, const RECT *a, const RECT *b)
{ (void)b; *out = *a; return TRUE; }
static HDC NtGdiCreateCompatibleDC(HDC input)
{ (void)input; return fail_dc ? NULL : (HDC)(uintptr_t)2; }
static void window_surface_lock(struct window_surface *surface) { (void)surface; ++locks; }
static void window_surface_unlock(struct window_surface *surface) { (void)surface; --locks; }
static void NtGdiSelectBitmap(HDC dc, void *bitmap) { (void)dc; (void)bitmap; }
static BOOL NtGdiPatBlt(HDC dc, int x, int y, int width, int height, DWORD op)
{ (void)dc; (void)x; (void)y; (void)width; (void)height; (void)op; return TRUE; }
static BOOL NtGdiTransformPoints(HDC dc, POINT *input, POINT *output, int count, int op)
{ (void)dc; (void)input; (void)output; (void)count; (void)op; return TRUE; }
static BOOL NtGdiAlphaBlend(HDC dst, int x, int y, int width, int height,
        HDC src, int sx, int sy, int sw, int sh, DWORD blend, void *transform)
{
    (void)dst; (void)x; (void)y; (void)width; (void)height;
    (void)src; (void)sx; (void)sy; (void)sw; (void)sh; (void)blend; (void)transform;
    ++blends;
    return !fail_blend;
}
static void window_surface_add_app_paint_rect(struct window_surface *surface, const RECT *rect)
{ (void)surface; (void)rect; }
static void add_bounds_rect(RECT *bounds, const RECT *rect) { *bounds = *rect; }
static void NtGdiDeleteObjectApp(HDC dc) { (void)dc; }
static void window_surface_flush(struct window_surface *surface) { (void)surface; ++flushes; }
static void window_surface_release(struct window_surface *surface) { assert(surface); ++releases; }

static void reset(void)
{
    memset(&test_surface, 0, sizeof(test_surface));
    memset(&dummy_surface, 0, sizeof(dummy_surface));
    returned_surface = &test_surface;
    layered_style = TRUE;
    attributes = fail_dc = fail_blend = expected_layered = FALSE;
    position_calls = metadata_calls = updates = flushes = releases = locks = blends = 0;
    last_error = 0;
    expected_key = CLR_INVALID;
    expected_alpha = 255;
}
"""


CASES = r"""
int main(void)
{
    HWND hwnd = (HWND)(uintptr_t)1;
    HDC src = (HDC)(uintptr_t)3;
    DWORD flags;
    SIZE invalid = {0, 100}, resized = {101, 100};
    BLENDFUNCTION blend = {0, 0, 123, 0};

    for (flags = 0; flags < 8; ++flags)
    {
        reset();
        expected_layered = TRUE;
        expected_key = flags & ULW_COLORKEY ? 0x123456 : CLR_INVALID;
        expected_alpha = 123;
        assert(NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL,
                0x123456, &blend, flags, NULL));
        assert(position_calls == 1 && metadata_calls == 1 && updates == 1);
        assert(flushes == 1 && releases == 1 && locks == 0 && blends == 1);
    }

    reset();
    assert(NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, NULL, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(position_calls == 1 && metadata_calls == 0 && updates == 1 && releases == 1);

    reset();
    returned_surface = &dummy_surface;
    assert(NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(position_calls == 1 && metadata_calls == 0 && updates == 1 && releases == 1);

    reset();
    returned_surface = NULL;
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(position_calls == 1 && metadata_calls == 0 && updates == 0 && releases == 0);

    reset();
    fail_dc = expected_layered = TRUE;
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(position_calls == 1 && metadata_calls == 1 && updates == 0 && releases == 1 && locks == 0);

    reset();
    fail_blend = expected_layered = TRUE;
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(metadata_calls == 1 && updates == 1 && flushes == 1 && releases == 1 && locks == 0);

    reset();
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, 0x10, NULL));
    assert(last_error == ERROR_INVALID_PARAMETER && !position_calls && !metadata_calls);

    reset();
    layered_style = FALSE;
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(last_error == ERROR_INVALID_PARAMETER && !position_calls && !metadata_calls);

    reset();
    attributes = TRUE;
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, NULL, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(last_error == ERROR_INVALID_PARAMETER && !position_calls && !metadata_calls);

    reset();
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, &invalid, src, NULL, 0, NULL, ULW_ALPHA, NULL));
    assert(last_error == ERROR_INVALID_PARAMETER && !position_calls && !metadata_calls);

    reset();
    assert(!NtUserUpdateLayeredWindow(hwnd, NULL, NULL, &resized, src, NULL, 0, NULL,
                ULW_ALPHA | ULW_EX_NORESIZE, NULL));
    assert(last_error == ERROR_INCORRECT_SIZE && !position_calls && !metadata_calls);
    return 0;
}
"""


class LayeredWindowTests(unittest.TestCase):
    def test_metadata_before_position_notification(self):
        helper = extract_function(SOURCE.read_text(), "BOOL WINAPI NtUserUpdateLayeredWindow(")
        with tempfile.TemporaryDirectory(prefix="win32u-layered-") as temporary:
            source = Path(temporary) / "test.c"
            binary = Path(temporary) / "test"
            source.write_text(STUBS + helper + CASES)
            command = shlex.split(os.environ.get("CC", "cc"))
            command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g"]
            if os.environ.get("WIN32U_LAYERED_SANITIZERS"):
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command + [str(source), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
