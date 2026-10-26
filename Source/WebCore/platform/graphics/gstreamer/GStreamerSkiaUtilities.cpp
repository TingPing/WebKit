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
#include "GStreamerSkiaUtilities.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && USE(SKIA)

#include "GStreamerCommon.h"
#include "Logging.h"
#include <array>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkPixmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

static SkYUVColorSpace skiaYUVColorSpace(const GstVideoColorimetry& colorimetry)
{
    bool isFullRange = colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    switch (colorimetry.matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT709:
        return isFullRange ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
        return isFullRange ? kBT2020_8bit_Full_SkYUVColorSpace : kBT2020_8bit_Limited_SkYUVColorSpace;
    case GST_VIDEO_COLOR_MATRIX_SMPTE240M:
        return isFullRange ? kSMPTE240_Full_SkYUVColorSpace : kSMPTE240_Limited_SkYUVColorSpace;
    case GST_VIDEO_COLOR_MATRIX_FCC:
        return isFullRange ? kFCC_Full_SkYUVColorSpace : kFCC_Limited_SkYUVColorSpace;
    default:
        // Default to bt601. This is the same behaviour as GStreamer's glcolorconvert element.
        return isFullRange ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
    }
}

std::optional<SkYUVAInfo> skiaYUVAInfo(GstMappedFrame& mappedFrame)
{
    auto planeConfig = SkYUVAInfo::PlaneConfig::kUnknown;
    auto subsampling = SkYUVAInfo::Subsampling::kUnknown;
    switch (GST_VIDEO_INFO_FORMAT(mappedFrame.info())) {
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGBA:
        planeConfig = SkYUVAInfo::PlaneConfig::kYUVA;
        subsampling = SkYUVAInfo::Subsampling::k444;
        break;
    case GST_VIDEO_FORMAT_I420:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_U_V;
        subsampling = SkYUVAInfo::Subsampling::k420;
        break;
    case GST_VIDEO_FORMAT_YV12:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_V_U;
        subsampling = SkYUVAInfo::Subsampling::k420;
        break;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_UV;
        subsampling = SkYUVAInfo::Subsampling::k420;
        break;
    case GST_VIDEO_FORMAT_NV21:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_VU;
        subsampling = SkYUVAInfo::Subsampling::k420;
        break;
    case GST_VIDEO_FORMAT_Y444:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_U_V;
        subsampling = SkYUVAInfo::Subsampling::k444;
        break;
    case GST_VIDEO_FORMAT_Y41B:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_U_V;
        subsampling = SkYUVAInfo::Subsampling::k411;
        break;
    case GST_VIDEO_FORMAT_Y42B:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_U_V;
        subsampling = SkYUVAInfo::Subsampling::k422;
        break;
    case GST_VIDEO_FORMAT_A420:
        planeConfig = SkYUVAInfo::PlaneConfig::kY_U_V_A;
        subsampling = SkYUVAInfo::Subsampling::k420;
        break;
    default:
        break;
    }

    if (planeConfig == SkYUVAInfo::PlaneConfig::kUnknown) {
        LOG_ERROR("Failed to create Skia image for video buffer: unsupported buffer format");
        return std::nullopt;
    }

    return SkYUVAInfo(SkISize::Make(mappedFrame.width(), mappedFrame.height()), planeConfig, subsampling, skiaYUVColorSpace(GST_VIDEO_INFO_COLORIMETRY(mappedFrame.info())));
}

sk_sp<SkColorSpace> skiaColorSpaceForYUV(const GstVideoInfo* videoInfo)
{
    const auto& colorimetry = GST_VIDEO_INFO_COLORIMETRY(videoInfo);
    if (gst_video_colorimetry_matches(&colorimetry, GST_VIDEO_COLORIMETRY_BT2100_PQ))
        return SkColorSpace::MakeRGB(SkNamedTransferFn::kPQ, SkNamedGamut::kRec2020);
    // The transfer function of SDR frames is not converted, as when GStreamer converts them to RGB.
    return SkColorSpace::MakeSRGB();
}

SkYUVAPixmaps skiaYUVAPixmaps(GstMappedFrame& mappedFrame)
{
    auto info = skiaYUVAInfo(mappedFrame);
    if (!info)
        return { };

    std::array<size_t, SkYUVAInfo::kMaxPlanes> rowBytes { };
    unsigned planeCount = GST_VIDEO_INFO_N_PLANES(mappedFrame.info());
    for (unsigned i = 0; i < planeCount && i < SkYUVAInfo::kMaxPlanes; ++i)
        rowBytes[i] = mappedFrame.planeStride(i);

    SkYUVAPixmapInfo pixmapInfo(*info, SkYUVAPixmapInfo::DataType::kUnorm8, rowBytes.data());
    if (!pixmapInfo.isValid() || pixmapInfo.numPlanes() != static_cast<int>(planeCount)) {
        LOG_ERROR("Failed to create Skia image for YUV video buffer: invalid plane layout");
        return { };
    }

    std::array<SkPixmap, SkYUVAInfo::kMaxPlanes> pixmaps;
    for (unsigned i = 0; i < planeCount; ++i) {
        auto planeData = mappedFrame.planeData(i);
        if (planeData.size() < pixmapInfo.planeInfo(i).computeByteSize(pixmapInfo.rowBytes(i)))
            return { };
        pixmaps[i] = SkPixmap(pixmapInfo.planeInfo(i), planeData.data(), pixmapInfo.rowBytes(i));
    }

    return SkYUVAPixmaps::FromExternalPixmaps(*info, pixmaps.data());
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && USE(SKIA)
