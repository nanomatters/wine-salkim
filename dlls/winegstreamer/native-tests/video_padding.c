/*
 * Native tests for video output padding
 *
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software. You can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation. Either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY. Without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "../wg_transform.c"

GST_DEBUG_CATEGORY(wine);

static void test_bottom_padding(const char *game)
{
    GstVideoInfo src, dst;
    GstVideoAlignment align;
    struct wg_sample sample = {0};
    GstBuffer *buffer;
    GstMapInfo map;
    BYTE *data;
    guint i;

    assert(gst_video_info_set_format(&src, GST_VIDEO_FORMAT_RGBA, 8, 2));
    dst = src;
    gst_video_alignment_reset(&align);
    align.padding_bottom = 2;
    assert(gst_video_info_align(&dst, &align));
    buffer = gst_buffer_new_allocate(NULL, src.size, NULL);
    assert(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    memset(map.data, 0x17, src.stride[0]);
    memset(map.data + src.stride[0], 0x23, src.stride[0]);
    gst_buffer_unmap(buffer, &map);
    data = malloc(dst.size);
    assert(data);
    memset(data, 0xcc, dst.size);
    sample.data = (UINT_PTR)data;
    sample.max_size = dst.size;
    assert(!setenv("SteamGameId", game, 1));
    assert(read_transform_output_video(&sample, buffer, &src, &dst, &align) == STATUS_SUCCESS);
    assert(sample.size == dst.size);
    for (i = 0; i < dst.size; ++i) assert(data[i] == (i < (guint)dst.stride[0] ? 0x17 : 0x23));
    free(data);
    gst_buffer_unref(buffer);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    GST_DEBUG_CATEGORY_INIT(wine, "wine-padding-test", 0, NULL);
    test_bottom_padding("1449280");
    test_bottom_padding("1839950");
    unsetenv("SteamGameId");
    puts("Video padding tests passed.");
    gst_deinit();
    return 0;
}
