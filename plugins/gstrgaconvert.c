/* GStreamer
 * Copyright (C) 2021 FIXME <fixme@example.com>
 * Danil intl (c) 2026 intx82@gmail.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstrgaconvert
 *
 * The rgaconvert element converts/scales video frames using Rockchip RGA IM2D.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v \
 *   fakesrc ! video/x-raw,format=NV12,width=1920,height=1080 ! \
 *   rgaconvert ! video/x-raw,format=RGBA,width=640,height=480 ! \
 *   fakesink
 * ]|
 * convert 1920x1080 ---> 640x480 and NV12 ---> RGBA .
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrgaconvert.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/gstvideofilter.h>
#include <gst/video/video.h>

#include <rga/im2d.h>
#include <rga/rga.h>
#include <stdio.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC(gst_rga_convert_debug_category);
#define GST_CAT_DEFAULT gst_rga_convert_debug_category

#define GST_CASE_RETURN(a, b)                                                                                                                                                                          \
    case a:                                                                                                                                                                                            \
        return b

/* prototypes */

typedef enum {
    GST_RGA_MEM_MODE_AUTO = 0,
    GST_RGA_MEM_MODE_HANDLE,
    GST_RGA_MEM_MODE_VIRTUAL,
} GstRgaMemMode;

static gboolean gst_rga_convert_start(GstBaseTransform *trans);
static gboolean gst_rga_convert_stop(GstBaseTransform *trans);

static GstCaps *gst_rga_convert_transform_caps(GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps, GstCaps *filter);

static gboolean gst_rga_convert_set_info(GstVideoFilter *filter, GstCaps *incaps, GstVideoInfo *in_info, GstCaps *outcaps, GstVideoInfo *out_info);

static GstFlowReturn gst_rga_convert_transform_frame(GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe);

/* pad templates */

#define VIDEO_SRC_CAPS                                                                                                                                                                                 \
    "video/x-raw, "                                                                                                                                                                                    \
    "format = (string) "                                                                                                                                                                               \
    "{ I420, YV12, NV12, NV21, Y42B, NV16, NV61, RGB16, RGB15, BGR, RGB, BGRA, RGBA, BGRx, RGBx }"                                                                                                     \
    ", "                                                                                                                                                                                               \
    "width = (int) [ 1, 4096 ] ,"                                                                                                                                                                      \
    "height = (int) [ 1, 4096 ] ,"                                                                                                                                                                     \
    "framerate = (fraction) [ 0, max ]"

#define VIDEO_SINK_CAPS                                                                                                                                                                                \
    "video/x-raw, "                                                                                                                                                                                    \
    "format = (string) "                                                                                                                                                                               \
    "{ I420, YV12, NV12, NV21, Y42B, NV16, NV61, RGB16, RGB15, BGR, RGB, BGRA, RGBA, BGRx, RGBx }"                                                                                                     \
    ", "                                                                                                                                                                                               \
    "width = (int) [ 1, 8192 ] ,"                                                                                                                                                                      \
    "height = (int) [ 1, 8192 ] ,"                                                                                                                                                                     \
    "framerate = (fraction) [ 0, max ]"

typedef struct {
    gboolean mapped;
    GstMapInfo map_info;

    gboolean imported;
    rga_buffer_handle_t handle;

    rga_buffer_t buffer;
} GstRgaIm2dFrame;

/* class initialization */

G_DEFINE_TYPE_WITH_CODE(GstRgaConvert, gst_rga_convert, GST_TYPE_VIDEO_FILTER, GST_DEBUG_CATEGORY_INIT(gst_rga_convert_debug_category, "rgaconvert", 0, "debug category for rgaconvert element"));

static void gst_rga_convert_class_init(GstRgaConvertClass *klass)
{
    GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
    GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS(klass);

    gst_element_class_add_pad_template(GST_ELEMENT_CLASS(klass), gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_from_string(VIDEO_SRC_CAPS)));
    gst_element_class_add_pad_template(GST_ELEMENT_CLASS(klass), gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_from_string(VIDEO_SINK_CAPS)));

    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "Rockchip rga hardware convert", "Generic", "change video streams size and color via Rockchip rga",
                                          "http://github.com/intx82/gstreamer-rgaconvert");

    base_transform_class->passthrough_on_same_caps = TRUE;
    base_transform_class->transform_caps = GST_DEBUG_FUNCPTR(gst_rga_convert_transform_caps);
    base_transform_class->start = GST_DEBUG_FUNCPTR(gst_rga_convert_start);
    base_transform_class->stop = GST_DEBUG_FUNCPTR(gst_rga_convert_stop);

    video_filter_class->set_info = GST_DEBUG_FUNCPTR(gst_rga_convert_set_info);
    video_filter_class->transform_frame = GST_DEBUG_FUNCPTR(gst_rga_convert_transform_frame);
}

static void gst_rga_convert_init(GstRgaConvert *rgaconvert)
{
    (void)rgaconvert;
}

static RgaSURF_FORMAT gst_gst_format_to_rga_format(GstVideoFormat format)
{
    switch (format) {
        GST_CASE_RETURN(GST_VIDEO_FORMAT_I420, RK_FORMAT_YCbCr_420_P);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_YV12, RK_FORMAT_YCrCb_420_P);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV12, RK_FORMAT_YCbCr_420_SP);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV21, RK_FORMAT_YCrCb_420_SP);
#ifdef HAVE_NV12_10LE40
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV12_10LE40, RK_FORMAT_YCbCr_420_SP_10B);
#endif
        GST_CASE_RETURN(GST_VIDEO_FORMAT_Y42B, RK_FORMAT_YCbCr_422_P);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV16, RK_FORMAT_YCbCr_422_SP);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV61, RK_FORMAT_YCrCb_422_SP);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB16, RK_FORMAT_RGB_565);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB15, RK_FORMAT_RGBA_5551);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_BGR, RK_FORMAT_BGR_888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB, RK_FORMAT_RGB_888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_BGRA, RK_FORMAT_BGRA_8888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGBA, RK_FORMAT_RGBA_8888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_BGRx, RK_FORMAT_BGRX_8888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGBx, RK_FORMAT_RGBX_8888);
    default:
        return RK_FORMAT_UNKNOWN;
    }
}

static gboolean gst_rga_get_frame_geometry(GstVideoFrame *frame, RgaSURF_FORMAT *rga_format, gint *width, gint *height, gint *hstride, gint *vstride)
{
    RgaSURF_FORMAT fmt;
    gint w;
    gint h;
    gint hs;
    gint vs;

    fmt = gst_gst_format_to_rga_format(GST_VIDEO_FRAME_FORMAT(frame));
    if (fmt == RK_FORMAT_UNKNOWN)
        return FALSE;

    w = GST_VIDEO_FRAME_WIDTH(frame);
    h = GST_VIDEO_FRAME_HEIGHT(frame);
    hs = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);

    if (hs <= 0)
        return FALSE;

    if (GST_VIDEO_FRAME_N_PLANES(frame) == 1) {
        vs = GST_VIDEO_INFO_HEIGHT(&frame->info);
    } else {
        guint plane1_off = GST_VIDEO_INFO_PLANE_OFFSET(&frame->info, 1);
        vs = (gint)(plane1_off / (guint)hs);
    }

    switch (fmt) {
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_422_SP:
    case RK_FORMAT_YCrCb_422_SP:
    case RK_FORMAT_YCbCr_422_P:
    case RK_FORMAT_YCrCb_422_P:
    case RK_FORMAT_YCbCr_420_SP:
    case RK_FORMAT_YCrCb_420_SP:
    case RK_FORMAT_YCbCr_420_P:
    case RK_FORMAT_YCrCb_420_P:
        w &= ~1;
        h &= ~1;
        break;
    default:
        break;
    }

    *rga_format = fmt;
    *width = w;
    *height = h;
    *hstride = hs;
    *vstride = vs;
    return TRUE;
}

static gboolean gst_rga_frame_can_use_handle(GstVideoFrame *frame)
{
    GstBuffer *buffer = frame->buffer;

    if (gst_buffer_n_memory(buffer) != 1) {
        return FALSE;
    }

    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_dmabuf_memory(mem)) {
        return FALSE;
    }

    gsize offset = 0;
    gst_memory_get_sizes(mem, &offset, NULL);
    return offset == 0;
}

static gboolean gst_rga_import_video_frame_im2d(GstVideoFrame *frame, GstMapFlags map_flags, GstRgaMemMode mem_mode, GstRgaIm2dFrame *out)
{
    GstBuffer *buffer;
    RgaSURF_FORMAT fmt;
    gint width;
    gint height;
    gint hstride;
    gint vstride;
    gsize buf_size;
    gint fd = -1;

#ifdef RGA_IMPORT_USE_PARAM
    im_handle_param_t param;
#endif

    memset(out, 0, sizeof(*out));

    if (!gst_rga_get_frame_geometry(frame, &fmt, &width, &height, &hstride, &vstride))
        return FALSE;

    buffer = frame->buffer;
    buf_size = gst_buffer_get_size(buffer);

    if (gst_buffer_n_memory(buffer) == 1) {
        GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
        gsize offset = 0;

        if (gst_is_dmabuf_memory(mem)) {
            gst_memory_get_sizes(mem, &offset, NULL);
            if (offset == 0)
                fd = gst_dmabuf_memory_get_fd(mem);
        }
    }

#ifdef RGA_IMPORT_USE_PARAM
    memset(&param, 0, sizeof(param));
    param.width = (uint32_t)width;
    param.height = (uint32_t)height;
    param.format = (uint32_t)fmt;
#endif

    if (mem_mode == GST_RGA_MEM_MODE_HANDLE) {
        if (fd < 0) {
            GST_ERROR("HANDLE mode requested but no importable dmabuf fd is available");
            return FALSE;
        }

#ifdef RGA_IMPORT_USE_PARAM
        out->handle = importbuffer_fd(fd, &param);
#else
        out->handle = importbuffer_fd(fd, (int)buf_size);
#endif
        if (!out->handle) {
            GST_ERROR("importbuffer_fd(%d, %zu) failed", fd, (size_t)buf_size);
            return FALSE;
        }

        out->imported = TRUE;
        out->mapped = FALSE;
        out->buffer = wrapbuffer_handle(out->handle, width, height, fmt, hstride, vstride);
        return TRUE;
    }

    if (mem_mode == GST_RGA_MEM_MODE_VIRTUAL || mem_mode == GST_RGA_MEM_MODE_AUTO) {
        if (!gst_buffer_map(buffer, &out->map_info, map_flags)) {
            GST_ERROR("gst_buffer_map failed");
            return FALSE;
        }

        out->mapped = TRUE;
        out->imported = FALSE;

        if (!out->map_info.data) {
            GST_ERROR("gst_buffer_map returned NULL data");
            gst_buffer_unmap(buffer, &out->map_info);
            out->mapped = FALSE;
            return FALSE;
        }

        out->buffer = wrapbuffer_virtualaddr_t(out->map_info.data, width, height, hstride, vstride, fmt);
        return TRUE;
    }

    GST_ERROR("Unknown memory mode %d", (int)mem_mode);
    return FALSE;
}

static void gst_rga_release_video_frame_im2d(GstVideoFrame *frame, GstRgaIm2dFrame *im2d_frame)
{
    if (im2d_frame->imported) {
        releasebuffer_handle(im2d_frame->handle);
        im2d_frame->handle = 0;
        im2d_frame->imported = FALSE;
    }

    if (im2d_frame->mapped) {
        gst_buffer_unmap(frame->buffer, &im2d_frame->map_info);
        im2d_frame->mapped = FALSE;
    }
}

static gboolean gst_rga_convert_start(GstBaseTransform *trans)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(trans);
    GST_DEBUG_OBJECT(rgaconvert, "start");
    return TRUE;
}

static gboolean gst_rga_convert_stop(GstBaseTransform *trans)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(trans);
    GST_DEBUG_OBJECT(rgaconvert, "stop");
    return TRUE;
}

static GstCaps *gst_rga_convert_transform_caps(GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps, GstCaps *filter)
{
    GstCaps *ret;
    GstStructure *structure;
    GstCapsFeatures *features;
    gint i, n;

    GST_DEBUG_OBJECT(trans, "transform direction %s : caps=%" GST_PTR_FORMAT "    filter=%" GST_PTR_FORMAT, direction == GST_PAD_SINK ? "sink" : "src", caps, filter);

    ret = gst_caps_new_empty();
    n = gst_caps_get_size(caps);

    for (i = 0; i < n; i++) {
        structure = gst_caps_get_structure(caps, i);
        features = gst_caps_get_features(caps, i);

        if (i > 0 && gst_caps_is_subset_structure_full(ret, structure, features))
            continue;

        structure = gst_structure_copy(structure);

        if (direction == GST_PAD_SRC) {
            gst_structure_set(structure, "width", GST_TYPE_INT_RANGE, 1, 4096, "height", GST_TYPE_INT_RANGE, 1, 4096, NULL);
        } else {
            gst_structure_set(structure, "width", GST_TYPE_INT_RANGE, 1, 8192, "height", GST_TYPE_INT_RANGE, 1, 8192, NULL);
        }

        if (!gst_caps_features_is_any(features))
            gst_structure_remove_fields(structure, "format", "colorimetry", "chroma-site", NULL);

        gst_caps_append_structure_full(ret, structure, gst_caps_features_copy(features));
    }

    if (filter) {
        GstCaps *intersection = gst_caps_intersect_full(filter, ret, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(ret);
        ret = intersection;
    }

    GST_DEBUG_OBJECT(trans, "returning caps: %" GST_PTR_FORMAT, ret);

    return ret;
}

static gboolean gst_rga_convert_set_info(GstVideoFilter *filter, GstCaps *incaps, GstVideoInfo *in_info, GstCaps *outcaps, GstVideoInfo *out_info)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(filter);
    GstVideoFormat in_format;
    GstVideoFormat out_format;

    (void)incaps;
    (void)outcaps;

    GST_DEBUG_OBJECT(rgaconvert, "set_info");

    in_format = GST_VIDEO_INFO_FORMAT(in_info);
    out_format = GST_VIDEO_INFO_FORMAT(out_info);

    if (gst_gst_format_to_rga_format(in_format) == RK_FORMAT_UNKNOWN || gst_gst_format_to_rga_format(out_format) == RK_FORMAT_UNKNOWN) {
        GST_INFO_OBJECT(filter, "don't support format. in format=%d,out format=%d", in_format, out_format);
        return FALSE;
    }

    return TRUE;
}

static GstFlowReturn gst_rga_convert_transform_frame(GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(filter);
    GstRgaIm2dFrame src;
    GstRgaIm2dFrame dst;
    RgaSURF_FORMAT src_fmt;
    RgaSURF_FORMAT dst_fmt;
    gint src_w, src_h, src_hs, src_vs;
    gint dst_w, dst_h, dst_hs, dst_vs;
    im_rect src_rect;
    im_rect dst_rect;
    IM_STATUS status;
    GstRgaMemMode mem_mode = GST_RGA_MEM_MODE_VIRTUAL;

    GST_DEBUG_OBJECT(rgaconvert, "transform_frame");

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    memset(&src_rect, 0, sizeof(src_rect));
    memset(&dst_rect, 0, sizeof(dst_rect));

    if (!gst_rga_get_frame_geometry(inframe, &src_fmt, &src_w, &src_h, &src_hs, &src_vs)) {
        return GST_FLOW_ERROR;
    }

    if (!gst_rga_get_frame_geometry(outframe, &dst_fmt, &dst_w, &dst_h, &dst_hs, &dst_vs)) {
        return GST_FLOW_ERROR;
    }

    if (gst_rga_frame_can_use_handle(inframe) && gst_rga_frame_can_use_handle(outframe)) {
        mem_mode = GST_RGA_MEM_MODE_HANDLE;
    }

    if (!gst_rga_import_video_frame_im2d(inframe, GST_MAP_READ, mem_mode, &src)) {
        return GST_FLOW_ERROR;
    }

    if (!gst_rga_import_video_frame_im2d(outframe, GST_MAP_WRITE, mem_mode, &dst)) {
        gst_rga_release_video_frame_im2d(inframe, &src);
        return GST_FLOW_ERROR;
    }

    src_rect.x = 0;
    src_rect.y = 0;
    src_rect.width = src_w;
    src_rect.height = src_h;

    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.width = dst_w;
    dst_rect.height = dst_h;

    status = imcheck(src.buffer, dst.buffer, src_rect, dst_rect);
    if ((status != IM_STATUS_SUCCESS) && (status != IM_STATUS_NOERROR)) {
        GST_ERROR_OBJECT(filter, "imcheck failed: %s", imStrError(status));
        gst_rga_release_video_frame_im2d(outframe, &dst);
        gst_rga_release_video_frame_im2d(inframe, &src);
        return GST_FLOW_ERROR;
    }

#ifdef RGA_IMPROCESS_USE_PAT
    {
        rga_buffer_t pat;
        im_rect prect;

        memset(&pat, 0, sizeof(pat));
        memset(&prect, 0, sizeof(prect));

        status = improcess(src.buffer, dst.buffer, pat, src_rect, dst_rect, prect, IM_SYNC);
    }
#else
    status = improcess(src.buffer, dst.buffer, src_rect, dst_rect, IM_SYNC);
#endif

    if ((status != IM_STATUS_SUCCESS) && (status != IM_STATUS_NOERROR)) {
        GST_ERROR_OBJECT(filter, "improcess failed: %s", imStrError(status));
        gst_rga_release_video_frame_im2d(outframe, &dst);
        gst_rga_release_video_frame_im2d(inframe, &src);
        return GST_FLOW_ERROR;
    }

    gst_rga_release_video_frame_im2d(outframe, &dst);
    gst_rga_release_video_frame_im2d(inframe, &src);

    return GST_FLOW_OK;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "rgaconvert", GST_RANK_NONE, GST_TYPE_RGA_CONVERT);
}

#ifndef VERSION
#define VERSION "1.0.0"
#endif
#ifndef PACKAGE
#define PACKAGE "rga_convert"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "rga_convert"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/intx82/gstreamer-rgaconvert.git"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, rgaconvert, "video size colorspace convert for rockchip rga hardware", plugin_init, VERSION, "GPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)