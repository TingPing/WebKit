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
#include "RemoteGStreamerVideoDecoder.h"

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include "Connection.h"
#include "RemoteGStreamerVideoDecoderManagerProxyMessages.h"
#include "RemoteVideoFrameObjectHeap.h"
#include <WebCore/GStreamerCommon.h>
#include <WebCore/GUniquePtrGStreamer.h>
#include <WebCore/VideoFrameGStreamer.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <mutex>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/MakeString.h>

GST_DEBUG_CATEGORY_STATIC(webkit_remote_video_decoder_debug);
#define GST_CAT_DEFAULT webkit_remote_video_decoder_debug

namespace WebKit {
using namespace WebCore;

// The number of the frame in the web process. GstVideoDecoder copies the metas without tags from the
// input to the output buffers. The web process matches the frames of other decoders by timestamp.
struct FrameNumberMeta {
    GstMeta parent;
    uint32_t frameNumber;
};

static GType frameNumberMetaAPIGetType()
{
    static GType type;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        static const char* tags[] = { nullptr };
        type = gst_meta_api_type_register("WebKitRemoteVideoDecoderFrameNumberMetaAPI", tags);
    });
    return type;
}

static const GstMetaInfo* frameNumberMetaGetInfo();

static void addFrameNumberMeta(GstBuffer* buffer, uint32_t frameNumber)
{
    auto* meta = reinterpret_cast<FrameNumberMeta*>(gst_buffer_add_meta(buffer, frameNumberMetaGetInfo(), nullptr));
    meta->frameNumber = frameNumber;
}

static const GstMetaInfo* frameNumberMetaGetInfo()
{
    static const GstMetaInfo* info;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        info = gst_meta_register(frameNumberMetaAPIGetType(), "WebKitRemoteVideoDecoderFrameNumberMeta", sizeof(FrameNumberMeta),
            [](GstMeta* meta, gpointer, GstBuffer*) -> gboolean {
                reinterpret_cast<FrameNumberMeta*>(meta)->frameNumber = 0;
                return TRUE;
            }, nullptr,
            [](GstBuffer* destination, GstMeta* meta, GstBuffer*, GQuark type, gpointer) -> gboolean {
                if (!GST_META_TRANSFORM_IS_COPY(type))
                    return FALSE;
                addFrameNumberMeta(destination, reinterpret_cast<FrameNumberMeta*>(meta)->frameNumber);
                return TRUE;
            });
    });
    return info;
}

RefPtr<RemoteGStreamerVideoDecoder> RemoteGStreamerVideoDecoder::create(RemoteGStreamerVideoDecoderIdentifier identifier, const String& factoryName, const String& caps, IPC::Connection& connection, RemoteVideoFrameObjectHeap& videoFrameObjectHeap, WorkQueue& queue)
{
    Ref decoder = adoptRef(*new RemoteGStreamerVideoDecoder(identifier, connection, videoFrameObjectHeap, queue));
    if (!decoder->initialize(factoryName, caps)) {
        decoder->close();
        return nullptr;
    }
    return decoder;
}

RemoteGStreamerVideoDecoder::RemoteGStreamerVideoDecoder(RemoteGStreamerVideoDecoderIdentifier identifier, IPC::Connection& connection, RemoteVideoFrameObjectHeap& videoFrameObjectHeap, WorkQueue& queue)
    : m_identifier(identifier)
    , m_connection(connection)
    , m_videoFrameObjectHeap(videoFrameObjectHeap)
    , m_queue(queue)
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_remote_video_decoder_debug, "webkitremotevideodecoder", 0, "WebKit GPU process video decoder");
    });
}

RemoteGStreamerVideoDecoder::~RemoteGStreamerVideoDecoder()
{
    ASSERT(!m_pipeline);
}

bool RemoteGStreamerVideoDecoder::initialize(const String& factoryName, const String& capsString)
{
    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_from_string(capsString.utf8().legacyCStringPointer()));
    if (!caps || !gst_caps_is_fixed(caps.get())) {
        GST_WARNING("Invalid input caps %s", capsString.utf8().legacyCStringPointer());
        return false;
    }

    m_pipeline = gst_pipeline_new(makeString("remote-video-decoder-"_s, m_identifier.toUInt64()).utf8().legacyCStringPointer());
    m_source = makeGStreamerElement("appsrc"_s);
    GRefPtr<GstElement> decoder = gst_element_factory_make(factoryName.utf8().legacyCStringPointer(), nullptr);
    GRefPtr<GstElement> converter = makeGStreamerElement("videoconvert"_s);
    GRefPtr<GstElement> sink = makeGStreamerElement("appsink"_s);
    if (!m_source || !decoder || !converter || !sink)
        return false;

    gst_bin_add_many(GST_BIN_CAST(m_pipeline.get()), m_source.get(), decoder.get(), converter.get(), sink.get(), nullptr);
    if (!gst_element_link_many(m_source.get(), decoder.get(), converter.get(), sink.get(), nullptr))
        return false;

    g_object_set(m_source.get(), "caps", caps.get(), "format", GST_FORMAT_TIME, "max-bytes", static_cast<guint64>(0), nullptr);

    // The formats that the compositor, canvas and WebGL take in system memory.
    GRefPtr<GstCaps> outputCaps = adoptGRef(gst_caps_from_string("video/x-raw, format=(string){ I420, YV12, NV12, NV21, Y444, Y42B, Y41B, A420, BGRx, BGRA, RGBx, RGBA }"));
    g_object_set(sink.get(), "caps", outputCaps.get(), "sync", FALSE, "enable-last-sample", FALSE, nullptr);

    static GstAppSinkCallbacks callbacks = [] {
        GstAppSinkCallbacks callbacks { };
        callbacks.eos = [](GstAppSink*, gpointer userData) {
            static_cast<RemoteGStreamerVideoDecoder*>(userData)->didReachEndOfStream();
        };
        callbacks.new_sample = [](GstAppSink* sink, gpointer userData) -> GstFlowReturn {
            static_cast<RemoteGStreamerVideoDecoder*>(userData)->sampleAvailable(adoptGRef(gst_app_sink_pull_sample(sink)));
            return GST_FLOW_OK;
        };
        return callbacks;
    }();
    gst_app_sink_set_callbacks(GST_APP_SINK(sink.get()), &callbacks, this, nullptr);

    GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
    gst_bus_set_sync_handler(bus.get(), [](GstBus*, GstMessage* message, gpointer userData) -> GstBusSyncReply {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            static_cast<RemoteGStreamerVideoDecoder*>(userData)->didFail(message);
        return GST_BUS_DROP;
    }, this, nullptr);

    GST_DEBUG_OBJECT(m_pipeline.get(), "Decoding %" GST_PTR_FORMAT " with %s", caps.get(), factoryName.utf8().legacyCStringPointer());
    return gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
}

void RemoteGStreamerVideoDecoder::setFormat(const String& capsString)
{
    assertIsCurrent(m_queue.get());
    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_from_string(capsString.utf8().legacyCStringPointer()));
    if (!caps || !gst_caps_is_fixed(caps.get()))
        return;
    g_object_set(m_source.get(), "caps", caps.get(), nullptr);
}

void RemoteGStreamerVideoDecoder::decode(uint32_t frameNumber, std::span<const uint8_t> data, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool isKeyFrame)
{
    assertIsCurrent(m_queue.get());
    if (m_hasFailed)
        return;

    GRefPtr<GstBuffer> buffer = adoptGRef(gst_buffer_new_memdup(data.data(), data.size()));
    GST_BUFFER_PTS(buffer.get()) = toGstClockTime(presentationTime);
    GST_BUFFER_DTS(buffer.get()) = toGstClockTime(decodeTime);
    GST_BUFFER_DURATION(buffer.get()) = toGstClockTime(duration);
    if (!isKeyFrame)
        GST_BUFFER_FLAG_SET(buffer.get(), GST_BUFFER_FLAG_DELTA_UNIT);
    addFrameNumberMeta(buffer.get(), frameNumber);
    gst_app_src_push_buffer(GST_APP_SRC(m_source.get()), buffer.leakRef());
}

void RemoteGStreamerVideoDecoder::restart()
{
    assertIsCurrent(m_queue.get());
    if (!m_pipeline || m_hasFailed)
        return;

    // The decoder drops the frames it holds when it goes to READY, and keeps its configuration.
    gst_element_set_state(m_pipeline.get(), GST_STATE_READY);
    gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);
}

void RemoteGStreamerVideoDecoder::flush()
{
    assertIsCurrent(m_queue.get());
    GST_DEBUG_OBJECT(m_pipeline.get(), "Flushing");
    restart();
    m_isDraining = false;
    if (auto completionHandler = std::exchange(m_drainCompletionHandler, nullptr))
        completionHandler();
}

void RemoteGStreamerVideoDecoder::drain(CompletionHandler<void()>&& completionHandler)
{
    assertIsCurrent(m_queue.get());
    if (m_hasFailed || m_isDraining) {
        completionHandler();
        return;
    }

    // The frames reach the web process before the reply, which is sent when the end of the stream
    // went through the decoder.
    GST_DEBUG_OBJECT(m_pipeline.get(), "Draining");
    m_isDraining = true;
    m_drainCompletionHandler = WTF::move(completionHandler);
    gst_app_src_end_of_stream(GST_APP_SRC(m_source.get()));
}

void RemoteGStreamerVideoDecoder::close()
{
    assertIsCurrent(m_queue.get());
    if (auto pipeline = std::exchange(m_pipeline, nullptr)) {
        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
        GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(pipeline.get())));
        gst_bus_set_sync_handler(bus.get(), nullptr, nullptr, nullptr);
    }
    m_source = nullptr;
    if (auto completionHandler = std::exchange(m_drainCompletionHandler, nullptr))
        completionHandler();
}

void RemoteGStreamerVideoDecoder::sampleAvailable(GRefPtr<GstSample>&& sample)
{
    auto* buffer = sample ? gst_sample_get_buffer(sample.get()) : nullptr;
    if (!buffer)
        return;

    std::optional<uint32_t> frameNumber;
    if (auto* meta = reinterpret_cast<FrameNumberMeta*>(gst_buffer_get_meta(buffer, frameNumberMetaAPIGetType())))
        frameNumber = meta->frameNumber;

    VideoFrameGStreamer::CreateOptions options;
    options.presentationTime = fromGstClockTime(GST_BUFFER_PTS(buffer));
    Ref frame = VideoFrameGStreamer::createWrappedSample(sample, options);
    m_connection->send(Messages::RemoteGStreamerVideoDecoderManagerProxy::FrameDecoded(m_identifier, frameNumber, m_videoFrameObjectHeap->add(WTF::move(frame))), 0);
}

void RemoteGStreamerVideoDecoder::didReachEndOfStream()
{
    m_queue->dispatch([protectedThis = Ref { *this }] {
        assertIsCurrent(protectedThis->m_queue.get());
        if (!protectedThis->m_isDraining)
            return;
        protectedThis->m_isDraining = false;
        // A drained decoder takes more frames after it restarts.
        protectedThis->restart();
        if (auto completionHandler = std::exchange(protectedThis->m_drainCompletionHandler, nullptr))
            completionHandler();
    });
}

void RemoteGStreamerVideoDecoder::didFail(GstMessage* message)
{
    GUniqueOutPtr<GError> error;
    GUniqueOutPtr<char> debug;
    gst_message_parse_error(message, &error.outPtr(), &debug.outPtr());
    GST_WARNING("Decoder failed: %s (%s)", error->message, debug.get());
    m_connection->send(Messages::RemoteGStreamerVideoDecoderManagerProxy::DecoderFailed(m_identifier, String::fromUTF8(error->message)), 0);

    m_queue->dispatch([protectedThis = Ref { *this }] {
        assertIsCurrent(protectedThis->m_queue.get());
        protectedThis->m_hasFailed = true;
        protectedThis->m_isDraining = false;
        if (auto completionHandler = std::exchange(protectedThis->m_drainCompletionHandler, nullptr))
            completionHandler();
    });
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
