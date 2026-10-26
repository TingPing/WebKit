/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GStreamerVideoFrameLayout.h"

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include <WebCore/GStreamerCommon.h>
#include <algorithm>
#include <gst/video/gstvideometa.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

namespace WebKit {
using namespace WebCore;

std::optional<GStreamerVideoFrameLayout> GStreamerVideoFrameLayout::write(const VideoFrameGStreamer& frame, NOESCAPE const Function<std::span<uint8_t>(size_t)>& allocate)
{
    GstMappedFrame mappedFrame(frame.sample(), GST_MAP_READ);
    if (!mappedFrame.isValid())
        return std::nullopt;

    auto* info = mappedFrame.info();
    unsigned planeCount = GST_VIDEO_INFO_N_PLANES(info);
    Vector<uint64_t> offsets;
    Vector<int32_t> strides;
    size_t size = 0;
    for (unsigned i = 0; i < planeCount; ++i) {
        offsets.append(size);
        strides.append(mappedFrame.planeStride(i));
        size += mappedFrame.planeData(i).size();
    }
    if (!size)
        return std::nullopt;

    auto destination = allocate(size);
    if (destination.size() < size)
        return std::nullopt;

    for (unsigned i = 0; i < planeCount; ++i) {
        auto plane = mappedFrame.planeData(i);
        memcpySpan(destination.subspan(offsets[i], plane.size()), plane);
    }

    const auto& colorimetry = GST_VIDEO_INFO_COLORIMETRY(info);
    return GStreamerVideoFrameLayout {
        .format = static_cast<uint32_t>(GST_VIDEO_INFO_FORMAT(info)),
        .size = { mappedFrame.width(), mappedFrame.height() },
        .presentationSize = frame.presentationSize(),
        .offsets = WTF::move(offsets),
        .strides = WTF::move(strides),
        .colorRange = static_cast<uint8_t>(colorimetry.range),
        .colorMatrix = static_cast<uint8_t>(colorimetry.matrix),
        .colorTransfer = static_cast<uint8_t>(colorimetry.transfer),
        .colorPrimaries = static_cast<uint8_t>(colorimetry.primaries),
    };
}

// The planes of these formats can be validated with their pixel strides and subsampling alone.
static bool isSupportedFormat(GstVideoFormat format)
{
    unsigned rawFormatCount = 0;
    auto* rawFormats = gst_video_formats_raw(&rawFormatCount);
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN; // GLib port
    std::span formats { rawFormats, rawFormatCount };
    if (std::ranges::find(formats, format) == formats.end())
        return false;

    auto* formatInfo = gst_video_format_get_info(format);
    if (GST_VIDEO_FORMAT_INFO_IS_COMPLEX(formatInfo) || GST_VIDEO_FORMAT_INFO_IS_TILED(formatInfo) || GST_VIDEO_FORMAT_INFO_HAS_PALETTE(formatInfo))
        return false;
    for (unsigned i = 0; i < GST_VIDEO_FORMAT_INFO_N_COMPONENTS(formatInfo); ++i) {
        if (GST_VIDEO_FORMAT_INFO_PSTRIDE(formatInfo, i) <= 0)
            return false;
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    return true;
}

static bool isSupportedSize(const IntSize& size)
{
    static constexpr int maxDimension = 16384;
    return !size.isEmpty() && size.width() <= maxDimension && size.height() <= maxDimension;
}

RefPtr<VideoFrameGStreamer> GStreamerVideoFrameLayout::createVideoFrame(GRefPtr<GstBuffer>&& buffer, VideoFrameGStreamer::CreateOptions&& options) const
{
    if (!isSupportedSize(size) || !isSupportedSize(presentationSize))
        return nullptr;

    auto videoFormat = static_cast<GstVideoFormat>(format);
    if (!isSupportedFormat(videoFormat))
        return nullptr;

    GstVideoInfo info;
    if (!gst_video_info_set_format(&info, videoFormat, size.width(), size.height()))
        return nullptr;

    unsigned planeCount = GST_VIDEO_INFO_N_PLANES(&info);
    if (offsets.size() != planeCount || strides.size() != planeCount)
        return nullptr;

    size_t dataSize = gst_buffer_get_size(buffer.get());
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN; // GLib port
    for (unsigned i = 0; i < planeCount; ++i) {
        std::array<gint, GST_VIDEO_MAX_COMPONENTS> components;
        gst_video_format_info_component(info.finfo, i, components.data());
        size_t planeHeight = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info.finfo, components[0], size.height());
        auto minimumStride = CheckedInt32(GST_VIDEO_FORMAT_INFO_SCALE_WIDTH(info.finfo, components[0], size.width())) * GST_VIDEO_FORMAT_INFO_PSTRIDE(info.finfo, components[0]);
        if (minimumStride.hasOverflowed() || strides[i] < minimumStride.value())
            return nullptr;
        auto planeEnd = CheckedSize(strides[i]) * planeHeight + offsets[i];
        if (planeEnd.hasOverflowed() || planeEnd.value() > dataSize)
            return nullptr;
        GST_VIDEO_INFO_PLANE_OFFSET(&info, i) = offsets[i];
        GST_VIDEO_INFO_PLANE_STRIDE(&info, i) = strides[i];
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    info.size = dataSize;

    auto& colorimetry = GST_VIDEO_INFO_COLORIMETRY(&info);
    colorimetry.range = colorRange <= GST_VIDEO_COLOR_RANGE_16_235 ? static_cast<GstVideoColorRange>(colorRange) : GST_VIDEO_COLOR_RANGE_UNKNOWN;
    colorimetry.matrix = colorMatrix <= GST_VIDEO_COLOR_MATRIX_BT2020 ? static_cast<GstVideoColorMatrix>(colorMatrix) : GST_VIDEO_COLOR_MATRIX_UNKNOWN;
    colorimetry.transfer = colorTransfer <= GST_VIDEO_TRANSFER_BT601 ? static_cast<GstVideoTransferFunction>(colorTransfer) : GST_VIDEO_TRANSFER_UNKNOWN;
    colorimetry.primaries = colorPrimaries <= GST_VIDEO_COLOR_PRIMARIES_EBU3213 ? static_cast<GstVideoColorPrimaries>(colorPrimaries) : GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;

    gst_buffer_add_video_meta_full(buffer.get(), GST_VIDEO_FRAME_FLAG_NONE, videoFormat, size.width(), size.height(), planeCount, info.offset, info.stride);
    GRefPtr<GstCaps> caps = adoptGRef(gst_video_info_to_caps(&info));
    if (!caps)
        return nullptr;

    GRefPtr<GstSample> sample = adoptGRef(gst_sample_new(buffer.get(), caps.get(), nullptr, nullptr));
    buffer = nullptr;
    options.presentationSize = presentationSize;
    return VideoFrameGStreamer::create(WTF::move(sample), options);
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
