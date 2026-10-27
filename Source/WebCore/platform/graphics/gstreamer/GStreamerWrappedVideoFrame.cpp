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
#include "GStreamerWrappedVideoFrame.h"

#if ENABLE(VIDEO) && USE(GSTREAMER)

#include "GStreamerCommon.h"
#include "VideoFrame.h"
#include <mutex>

namespace WebCore {

struct WebKitVideoFrameMeta {
    GstMeta parent;
    VideoFrame* frame;
};

static GType webkitVideoFrameMetaAPIGetType()
{
    static GType type;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        static const char* tags[] = { nullptr };
        type = gst_meta_api_type_register("WebKitVideoFrameMetaAPI", tags);
    });
    return type;
}

static const GstMetaInfo* webkitVideoFrameMetaGetInfo();

static WebKitVideoFrameMeta* addVideoFrameMeta(GstBuffer* buffer, VideoFrame& frame)
{
    auto* meta = reinterpret_cast<WebKitVideoFrameMeta*>(gst_buffer_add_meta(buffer, webkitVideoFrameMetaGetInfo(), nullptr));
    frame.ref();
    meta->frame = &frame;
    return meta;
}

static const GstMetaInfo* webkitVideoFrameMetaGetInfo()
{
    static const GstMetaInfo* info;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        info = gst_meta_register(webkitVideoFrameMetaAPIGetType(), "WebKitVideoFrameMeta", sizeof(WebKitVideoFrameMeta),
            [](GstMeta* meta, gpointer, GstBuffer*) -> gboolean {
                reinterpret_cast<WebKitVideoFrameMeta*>(meta)->frame = nullptr;
                return TRUE;
            },
            [](GstMeta* meta, GstBuffer*) {
                if (auto* frame = std::exchange(reinterpret_cast<WebKitVideoFrameMeta*>(meta)->frame, nullptr))
                    frame->deref();
            },
            [](GstBuffer* destination, GstMeta* meta, GstBuffer*, GQuark type, gpointer) -> gboolean {
                if (!GST_META_TRANSFORM_IS_COPY(type))
                    return FALSE;
                if (auto* frame = reinterpret_cast<WebKitVideoFrameMeta*>(meta)->frame)
                    addVideoFrameMeta(destination, *frame);
                return TRUE;
            });
    });
    return info;
}

GRefPtr<GstCaps> capsWithVideoFrameFeature(const GstVideoInfo& info)
{
    // Complete caps, which converters in the pipeline fixate to themselves and pass through.
    GRefPtr<GstCaps> caps = adoptGRef(gst_video_info_to_caps(&info));
    if (!caps)
        return nullptr;
    gst_caps_set_features(caps.get(), 0, gst_caps_features_new(WEBKIT_GST_CAPS_FEATURE_MEMORY_VIDEO_FRAME, nullptr));
    return caps;
}

GRefPtr<GstBuffer> bufferWrappingVideoFrame(VideoFrame& frame)
{
    GRefPtr<GstBuffer> buffer = adoptGRef(gst_buffer_new());
    addVideoFrameMeta(buffer.get(), frame);
    if (frame.presentationTime().isValid())
        GST_BUFFER_PTS(buffer.get()) = GST_BUFFER_DTS(buffer.get()) = toGstClockTime(frame.presentationTime());
    return buffer;
}

GRefPtr<GstSample> sampleWrappingVideoFrame(VideoFrame& frame)
{
    auto format = static_cast<GstVideoFormat>(frame.pixelFormat());
    if (format == GST_VIDEO_FORMAT_UNKNOWN || format == GST_VIDEO_FORMAT_ENCODED)
        return nullptr;

    GstVideoInfo info;
    auto size = frame.presentationSize();
    if (!gst_video_info_set_format(&info, format, size.width(), size.height()))
        return nullptr;
    GRefPtr<GstCaps> caps = capsWithVideoFrameFeature(info);
    if (!caps)
        return nullptr;
    return adoptGRef(gst_sample_new(bufferWrappingVideoFrame(frame).get(), caps.get(), nullptr, nullptr));
}

RefPtr<VideoFrame> videoFrameWrappedInSample(GstSample* sample)
{
    auto* buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    if (!buffer)
        return nullptr;
    auto* meta = reinterpret_cast<WebKitVideoFrameMeta*>(gst_buffer_get_meta(buffer, webkitVideoFrameMetaAPIGetType()));
    return meta ? meta->frame : nullptr;
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER)
