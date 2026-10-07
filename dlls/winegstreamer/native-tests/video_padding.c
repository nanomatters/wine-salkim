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

static void test_right_padding(GstVideoFormat format)
{
    GstVideoInfo info;
    GstVideoAlignment align;
    GstBuffer *buffer;
    BYTE *data;
    guint row, byte, pixel_stride;

    assert(gst_video_info_set_format(&info, format, 1, 2));
    gst_video_alignment_reset(&align);
    align.padding_right = 1;
    assert(gst_video_info_align(&info, &align));
    pixel_stride = info.finfo->pixel_stride[0];
    data = malloc(info.size + 16);
    assert(data);
    memset(data, 0xcc, info.size + 16);
    for (row = 0; row < 2; ++row)
        for (byte = 0; byte < pixel_stride; ++byte)
            data[row * info.stride[0] + byte] = 0x10 * (row + 1) + byte;
    buffer = gst_buffer_new_wrapped_full(0, data, info.size, 0, info.size, NULL, NULL);
    fill_frame_padded_bits(buffer, &align, &info, FILL_RIGHT);
    for (row = 0; row < 2; ++row)
        for (byte = 0; byte < (guint)info.stride[0]; ++byte)
            assert(data[row * info.stride[0] + byte] == 0x10 * (row + 1) + byte % pixel_stride);
    for (byte = info.size; byte < info.size + 16; ++byte) assert(data[byte] == 0xcc);
    gst_buffer_unref(buffer);
    free(data);
}

static void test_invalid_stride(gint stride)
{
    GstVideoInfo info;
    GstVideoAlignment align;
    GstBuffer *buffer;
    BYTE *data;
    guint byte;

    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_BGR, 1, 2));
    info.stride[0] = stride;
    if (stride < 0) info.offset[0] = -stride;
    gst_video_alignment_reset(&align);
    align.padding_right = align.padding_bottom = 1;
    data = malloc(info.size + 16);
    assert(data);
    memset(data, 0xcc, info.size + 16);
    buffer = gst_buffer_new_wrapped_full(0, data, info.size, 0, info.size, NULL, NULL);
    fill_frame_padded_bits(buffer, &align, &info, FILL_RIGHT | FILL_BOTTOM);
    for (byte = 0; byte < info.size + 16; ++byte) assert(data[byte] == 0xcc);
    gst_buffer_unref(buffer);
    free(data);
}

static void test_planar_padding(GstVideoFormat format)
{
    GstVideoInfo info;
    GstVideoAlignment align;
    GstBuffer *buffer;
    BYTE *data;
    guint plane, row, byte, height, padded_height;
    gint comp[GST_VIDEO_MAX_COMPONENTS];

    assert(gst_video_info_set_format(&info, format, 8, 4));
    gst_video_alignment_reset(&align);
    align.padding_bottom = 2;
    assert(gst_video_info_align(&info, &align));
    data = malloc(info.size + 16);
    assert(data);
    memset(data, 0xcc, info.size + 16);
    for (plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); ++plane)
    {
        gst_video_format_info_component(info.finfo, plane, comp);
        height = GST_VIDEO_INFO_COMP_HEIGHT(&info, comp[0]);
        for (row = 0; row < height; ++row)
            memset(data + info.offset[plane] + row * info.stride[plane], 0x10 * (plane + 1) + row,
                    info.stride[plane]);
    }
    buffer = gst_buffer_new_wrapped_full(0, data, info.size, 0, info.size, NULL, NULL);
    fill_frame_padded_bits(buffer, &align, &info, FILL_BOTTOM);
    for (plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); ++plane)
    {
        gst_video_format_info_component(info.finfo, plane, comp);
        height = GST_VIDEO_INFO_COMP_HEIGHT(&info, comp[0]);
        padded_height = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info.finfo, comp[0], info.height + align.padding_bottom);
        for (row = 0; row < padded_height; ++row)
            for (byte = 0; byte < (guint)info.stride[plane]; ++byte)
                assert(data[info.offset[plane] + row * info.stride[plane] + byte]
                        == 0x10 * (plane + 1) + min(row, height - 1));
    }
    for (byte = info.size; byte < info.size + 16; ++byte) assert(data[byte] == 0xcc);
    gst_buffer_unref(buffer);
    free(data);
}

static void test_aperture_padding(GstVideoFormat format, guint left, guint top, guint visible_height,
        enum fill_action action)
{
    GstVideoInfo info;
    GstVideoAlignment align;
    GstBuffer *buffer;
    BYTE *data, *expected;
    guint plane, row, byte, width, height, left_bytes, top_rows, total_rows;
    gint comp[GST_VIDEO_MAX_COMPONENTS], stride, pixel_stride;

    assert(gst_video_info_set_format(&info, format, 3, visible_height));
    gst_video_alignment_reset(&align);
    align.padding_left = left;
    align.padding_top = top;
    align.padding_right = 1;
    align.padding_bottom = 1;
    assert(gst_video_info_align(&info, &align));
    data = malloc(info.size + 16);
    expected = malloc(info.size + 16);
    assert(data && expected);
    memset(data, 0xcc, info.size + 16);
    memset(expected, 0xcc, info.size + 16);
    for (plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); ++plane)
    {
        BYTE *plane_start;

        gst_video_format_info_component(info.finfo, plane, comp);
        stride = info.stride[plane];
        pixel_stride = info.finfo->pixel_stride[comp[0]];
        width = GST_VIDEO_INFO_COMP_WIDTH(&info, comp[0]) * pixel_stride;
        height = GST_VIDEO_INFO_COMP_HEIGHT(&info, comp[0]);
        left_bytes = GST_VIDEO_FORMAT_INFO_SCALE_WIDTH(info.finfo, comp[0], align.padding_left) * pixel_stride;
        top_rows = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info.finfo, comp[0], align.padding_top);
        total_rows = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info.finfo, comp[0],
                align.padding_top + info.height + align.padding_bottom);
        plane_start = expected + info.offset[plane] - top_rows * stride - left_bytes;
        for (row = 0; row < height; ++row)
            for (byte = 0; byte < width; ++byte)
                data[info.offset[plane] + row * stride + byte] = 0x10 * (plane + 1) + row + byte % pixel_stride;
        for (row = top_rows; row < total_rows; ++row)
            for (byte = left_bytes; byte < (guint)stride; ++byte)
            {
                guint visible_row = min(row - top_rows, height - 1);

                if (row >= top_rows + height && !(action & FILL_BOTTOM)) continue;
                if (byte >= left_bytes + width && !(action & FILL_RIGHT)) continue;
                plane_start[row * stride + byte] = 0x10 * (plane + 1) + visible_row
                        + (byte - left_bytes) % pixel_stride;
            }
    }
    buffer = gst_buffer_new_wrapped_full(0, data, info.size, 0, info.size, NULL, NULL);
    fill_frame_padded_bits(buffer, &align, &info, action);
    assert(!memcmp(data, expected, info.size + 16));
    gst_buffer_unref(buffer);
    free(expected);
    free(data);
}

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

static void test_shared_buffer(const char *game, guint bottom, guint right, BOOL fill)
{
    GstVideoInfo info;
    GstVideoAlignment align;
    struct wg_sample sample = {0};
    GstBuffer *buffer, *extra_ref;
    BYTE *data;
    guint row, byte, stride, height;

    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 8, 2));
    gst_video_alignment_reset(&align);
    align.padding_bottom = bottom;
    align.padding_right = right;
    assert(gst_video_info_align(&info, &align));
    stride = info.stride[0];
    height = info.height + bottom;
    data = malloc(info.size + 16);
    assert(data);
    memset(data, 0xcc, info.size + 16);
    for (row = 0; row < 2; ++row) memset(data + row * stride, 0x10 * (row + 1), 32);
    sample.data = (UINT_PTR)data;
    sample.max_size = info.size;
    buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, data, info.size, 0, info.size, NULL, NULL);
    extra_ref = gst_buffer_ref(buffer);
    assert(!gst_buffer_is_writable(buffer));
    GST_BUFFER_PTS(buffer) = 10000;
    GST_BUFFER_DURATION(buffer) = 20000;
    if (game) assert(!setenv("SteamGameId", game, 1));
    else unsetenv("SteamGameId");
    assert(read_transform_output_video(&sample, buffer, &info, &info, &align) == STATUS_SUCCESS);
    assert(sample.size == info.size && sample.pts == 100 && sample.duration == 200);
    assert((sample.flags & (WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION))
            == (WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION));
    assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 2);
    for (row = 0; row < height; ++row)
        for (byte = 0; byte < stride; ++byte)
        {
            BYTE expected = row < 2 && byte < 32 ? 0x10 * (row + 1) : 0xcc;
            if (fill && bottom && row >= 2) expected = 0x20;
            if (fill && right && byte >= 32) expected = 0x10 * (row + 1);
            assert(data[row * stride + byte] == expected);
        }
    for (byte = info.size; byte < info.size + 16; ++byte) assert(data[byte] == 0xcc);
    gst_buffer_unref(extra_ref);
    assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 1);
    gst_buffer_unref(buffer);
    free(data);
}

static void test_aperture_alias(GstVideoFormat format)
{
    GstVideoInfo info;
    GstVideoAlignment align;
    struct wg_sample sample = {0};
    GstBuffer *buffer, *extra_ref;
    BYTE *data;
    guint plane, row, byte, width, height, left_bytes;
    gint comp[GST_VIDEO_MAX_COMPONENTS], pixel_stride;

    assert(gst_video_info_set_format(&info, format, 8, 2));
    gst_video_alignment_reset(&align);
    align.padding_left = align.padding_right = 1;
    assert(gst_video_info_align(&info, &align));
    data = malloc(info.size);
    assert(data);
    memset(data, 0xcc, info.size);
    for (plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); ++plane)
    {
        gst_video_format_info_component(info.finfo, plane, comp);
        pixel_stride = info.finfo->pixel_stride[comp[0]];
        width = GST_VIDEO_INFO_COMP_WIDTH(&info, comp[0]) * pixel_stride;
        height = GST_VIDEO_INFO_COMP_HEIGHT(&info, comp[0]);
        for (row = 0; row < height; ++row)
            for (byte = 0; byte < width; ++byte)
                data[info.offset[plane] + row * info.stride[plane] + byte] = 0x10 * (plane + 1) + row + byte % pixel_stride;
    }
    sample.data = (UINT_PTR)data;
    sample.max_size = info.size;
    buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, data, info.size, 0, info.size, NULL, NULL);
    extra_ref = gst_buffer_ref(buffer);
    assert(!setenv("SteamGameId", "536280", 1));
    assert(read_transform_output_video(&sample, buffer, &info, &info, &align) == STATUS_SUCCESS);
    assert(sample.size == info.size && GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 2);
    for (plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); ++plane)
    {
        gst_video_format_info_component(info.finfo, plane, comp);
        pixel_stride = info.finfo->pixel_stride[comp[0]];
        height = GST_VIDEO_INFO_COMP_HEIGHT(&info, comp[0]);
        left_bytes = GST_VIDEO_FORMAT_INFO_SCALE_WIDTH(info.finfo, comp[0], align.padding_left) * pixel_stride;
        for (row = 0; row < height; ++row)
            for (byte = 0; byte < (guint)info.stride[plane]; ++byte)
            {
                BYTE expected = byte < left_bytes ? 0xcc : 0x10 * (plane + 1) + row + (byte - left_bytes) % pixel_stride;

                assert(data[info.offset[plane] - left_bytes + row * info.stride[plane] + byte] == expected);
            }
    }
    gst_buffer_unref(extra_ref);
    assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 1);
    gst_buffer_unref(buffer);
    free(data);
}

static void test_alias_validation(BOOL wrong_format)
{
    GstVideoInfo src, dst;
    struct wg_sample sample = {0};
    GstBuffer *buffer, *output = NULL;
    BYTE *data;
    gsize total_size;

    assert(gst_video_info_set_format(&src, GST_VIDEO_FORMAT_RGBA, 8, 2));
    assert(gst_video_info_set_format(&dst, wrong_format ? GST_VIDEO_FORMAT_BGRA : GST_VIDEO_FORMAT_RGBA,
            8, wrong_format ? 2 : 3));
    data = malloc(dst.size);
    assert(data);
    memset(data, 0xcc, dst.size);
    sample.data = (UINT_PTR)data;
    sample.max_size = dst.size;
    buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, data, dst.size, 0, dst.size, NULL, NULL);
    assert(copy_video_buffer(buffer, &src, &dst, &sample, &total_size, &output) == STATUS_UNSUCCESSFUL);
    assert(!output && GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 1);
    gst_buffer_unref(buffer);
    free(data);
}

static void test_copy_failure(gsize source_size, gsize destination_size, NTSTATUS expected)
{
    GstVideoInfo src, dst;
    GstVideoAlignment align;
    struct wg_sample sample = {0};
    GstBuffer *buffer, *extra_ref;
    BYTE *data;
    guint byte;

    assert(gst_video_info_set_format(&src, GST_VIDEO_FORMAT_RGBA, 8, 2));
    dst = src;
    gst_video_alignment_reset(&align);
    align.padding_bottom = 2;
    assert(gst_video_info_align(&dst, &align));
    buffer = gst_buffer_new_allocate(NULL, source_size, NULL);
    assert(buffer);
    extra_ref = gst_buffer_ref(buffer);
    data = malloc(dst.size + 16);
    assert(data);
    memset(data, 0xcc, dst.size + 16);
    sample.data = (UINT_PTR)data;
    sample.max_size = destination_size;
    sample.size = 123;
    assert(!setenv("SteamGameId", "1839950", 1));
    assert(read_transform_output_video(&sample, buffer, &src, &dst, &align) == expected);
    assert(!sample.size && !sample.flags);
    assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 2);
    for (byte = 0; byte < dst.size + 16; ++byte) assert(data[byte] == 0xcc);
    gst_buffer_unref(extra_ref);
    gst_buffer_unref(buffer);
    free(data);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    GST_DEBUG_CATEGORY_INIT(wine, "wine-padding-test", 0, NULL);
    test_bottom_padding("1449280");
    test_bottom_padding("1839950");
    test_right_padding(GST_VIDEO_FORMAT_BGR);
    test_right_padding(GST_VIDEO_FORMAT_RGBA);
    test_invalid_stride(-4);
    test_invalid_stride(0);
    test_invalid_stride(2);
    test_planar_padding(GST_VIDEO_FORMAT_NV12);
    test_planar_padding(GST_VIDEO_FORMAT_I420);
    test_aperture_padding(GST_VIDEO_FORMAT_BGR, 1, 0, 3, FILL_RIGHT);
    test_aperture_padding(GST_VIDEO_FORMAT_RGBA, 1, 0, 3, FILL_RIGHT);
    test_aperture_padding(GST_VIDEO_FORMAT_NV12, 1, 0, 3, FILL_RIGHT);
    test_aperture_padding(GST_VIDEO_FORMAT_I420, 1, 0, 3, FILL_RIGHT);
    test_aperture_padding(GST_VIDEO_FORMAT_BGR, 1, 1, 3, FILL_RIGHT | FILL_BOTTOM);
    test_aperture_padding(GST_VIDEO_FORMAT_RGBA, 1, 1, 3, FILL_RIGHT | FILL_BOTTOM);
    test_aperture_padding(GST_VIDEO_FORMAT_NV12, 1, 1, 3, FILL_RIGHT | FILL_BOTTOM);
    test_aperture_padding(GST_VIDEO_FORMAT_I420, 1, 1, 3, FILL_RIGHT | FILL_BOTTOM);
    test_aperture_padding(GST_VIDEO_FORMAT_NV12, 1, 1, 4, FILL_RIGHT | FILL_BOTTOM);
    test_aperture_padding(GST_VIDEO_FORMAT_I420, 1, 1, 4, FILL_RIGHT | FILL_BOTTOM);
    test_shared_buffer("1839950", 2, 0, TRUE);
    test_shared_buffer("1449280", 2, 0, TRUE);
    test_shared_buffer("536280", 0, 1, TRUE);
    test_shared_buffer("1839950", 0, 0, FALSE);
    test_shared_buffer("1839950", 0, 1, FALSE);
    test_shared_buffer("536280", 2, 0, FALSE);
    test_shared_buffer("unlisted", 2, 0, FALSE);
    test_shared_buffer(NULL, 2, 0, FALSE);
    test_aperture_alias(GST_VIDEO_FORMAT_RGBA);
    test_aperture_alias(GST_VIDEO_FORMAT_BGR);
    test_aperture_alias(GST_VIDEO_FORMAT_NV12);
    test_aperture_alias(GST_VIDEO_FORMAT_I420);
    test_alias_validation(TRUE);
    test_alias_validation(FALSE);
    test_copy_failure(64, 127, STATUS_BUFFER_TOO_SMALL);
    test_copy_failure(63, 128, STATUS_UNSUCCESSFUL);
    test_copy_failure(0, 128, STATUS_UNSUCCESSFUL);
    unsetenv("SteamGameId");
    puts("Video padding tests passed.");
    gst_deinit();
    return 0;
}
