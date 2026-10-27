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
#include "WebKitRemoteVideoDecoderGStreamer.h"

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include "RemoteGStreamerVideoDecoderManagerProxy.h"
#include <WebCore/GStreamerCommon.h>
#include <WebCore/GStreamerWrappedVideoFrame.h>
#include <WebCore/VideoFrameGStreamer.h>
#include <gst/video/gstvideodecoder.h>
#include <mutex>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/Scope.h>
#include <wtf/WorkQueue.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/WTFGType.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

using namespace WebCore;
using namespace WebKit;

GST_DEBUG_CATEGORY_STATIC(webkit_remote_video_decoder_debug);
#define GST_CAT_DEFAULT webkit_remote_video_decoder_debug

#define WEBKIT_TYPE_REMOTE_VIDEO_DECODER (webkit_remote_video_decoder_get_type())
#define WEBKIT_REMOTE_VIDEO_DECODER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), WEBKIT_TYPE_REMOTE_VIDEO_DECODER, WebKitRemoteVideoDecoder))
#define WEBKIT_REMOTE_VIDEO_DECODER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), WEBKIT_TYPE_REMOTE_VIDEO_DECODER, WebKitRemoteVideoDecoderClass))

typedef struct _WebKitRemoteVideoDecoder WebKitRemoteVideoDecoder;
typedef struct _WebKitRemoteVideoDecoderClass WebKitRemoteVideoDecoderClass;
typedef struct _WebKitRemoteVideoDecoderPrivate WebKitRemoteVideoDecoderPrivate;

struct _WebKitRemoteVideoDecoder {
    GstVideoDecoder parent;
    WebKitRemoteVideoDecoderPrivate* priv;
};

struct _WebKitRemoteVideoDecoderClass {
    GstVideoDecoderClass parentClass;
    // The decoder of the GPU process that this type stands for.
    const char* factoryName;
};

struct _WebKitRemoteVideoDecoderPrivate {
    bool isHarnessed { false };

    // Accessed with the stream lock.
    std::optional<RemoteGStreamerVideoDecoderIdentifier> identifier;
    GstVideoCodecState* inputState { nullptr };
    std::optional<std::tuple<GstVideoFormat, int, int>> outputFormat;
    bool outputsWrappedFrames { false };
    bool needsSyncPoint { false };

    Lock lock;
    Condition condition;
    RefPtr<WorkQueue> outputQueue WTF_GUARDED_BY_LOCK(lock);
    std::optional<uint32_t> consumedFrameNumber WTF_GUARDED_BY_LOCK(lock);
    bool isFlushing WTF_GUARDED_BY_LOCK(lock) { false };
    bool hasFailed WTF_GUARDED_BY_LOCK(lock) { false };
    bool gpuProcessDidExit WTF_GUARDED_BY_LOCK(lock) { false };
};

enum {
    PROP_0,
    PROP_HARNESSED
};

GType webkit_remote_video_decoder_get_type();

WEBKIT_DEFINE_ABSTRACT_TYPE(WebKitRemoteVideoDecoder, webkit_remote_video_decoder, GST_TYPE_VIDEO_DECODER)

static gboolean webkitRemoteVideoDecoderStart(GstVideoDecoder* decoder)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(decoder)->priv;
    Locker locker { priv->lock };
    priv->outputQueue = WorkQueue::create("org.webkit.RemoteVideoDecoder"_s, WorkQueue::QOS::UserInteractive);
    priv->consumedFrameNumber = std::nullopt;
    priv->isFlushing = false;
    priv->hasFailed = false;
    return TRUE;
}

static gboolean webkitRemoteVideoDecoderStop(GstVideoDecoder* decoder)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(decoder)->priv;
    if (auto identifier = std::exchange(priv->identifier, std::nullopt))
        RemoteGStreamerVideoDecoderManagerProxy::singleton().releaseDecoder(*identifier);
    if (auto* inputState = std::exchange(priv->inputState, nullptr))
        gst_video_codec_state_unref(inputState);
    priv->outputFormat = std::nullopt;
    priv->outputsWrappedFrames = false;

    Locker locker { priv->lock };
    priv->outputQueue = nullptr;
    priv->isFlushing = true;
    priv->condition.notifyAll();
    return TRUE;
}

static void createGPUProcessDecoder(WebKitRemoteVideoDecoder* self)
{
    auto* priv = self->priv;
    auto* decoder = GST_VIDEO_DECODER(self);
    GUniquePtr<char> caps(gst_caps_to_string(priv->inputState->caps));
    GST_DEBUG_OBJECT(decoder, "Decoding %s in the GPU process", caps.get());
    priv->identifier = RemoteGStreamerVideoDecoderManagerProxy::singleton().createDecoder(GST_ELEMENT_CAST(decoder), String::fromLatin1(WEBKIT_REMOTE_VIDEO_DECODER_GET_CLASS(self)->factoryName),
        String::fromUTF8(caps.get()), gst_video_decoder_get_max_errors(decoder), priv->isHarnessed);
}

static gboolean webkitRemoteVideoDecoderSetFormat(GstVideoDecoder* decoder, GstVideoCodecState* state)
{
    auto* self = WEBKIT_REMOTE_VIDEO_DECODER(decoder);
    auto* priv = self->priv;
    if (priv->inputState)
        gst_video_codec_state_unref(priv->inputState);
    priv->inputState = gst_video_codec_state_ref(state);

    if (priv->identifier) {
        GUniquePtr<char> caps(gst_caps_to_string(state->caps));
        RemoteGStreamerVideoDecoderManagerProxy::singleton().setFormat(*priv->identifier, String::fromUTF8(caps.get()));
        return TRUE;
    }

    // Right after the GPU process exited, the next one isn't connected yet, and the next frame tries again.
    createGPUProcessDecoder(self);
    if (!priv->identifier) {
        Locker locker { priv->lock };
        priv->gpuProcessDidExit = true;
    }
    return TRUE;
}

// The frames sent to a GPU process that exited are lost, and the decoder of the new one starts at a keyframe.
// Returns false while there is no new GPU process to decode in.
static bool restartIfGPUProcessExited(WebKitRemoteVideoDecoder* self, GstVideoCodecFrame* currentFrame)
{
    auto* priv = self->priv;
    {
        Locker locker { priv->lock };
        if (!std::exchange(priv->gpuProcessDidExit, false))
            return true;
    }

    auto* decoder = GST_VIDEO_DECODER(self);
    if (priv->identifier) {
        GST_INFO_OBJECT(decoder, "The GPU process exited, decoding in a new one from the next keyframe");
        priv->identifier = std::nullopt;
        GList* frames = gst_video_decoder_get_frames(decoder);
        for (auto* item = frames; item; item = g_list_next(item)) {
            auto* frame = static_cast<GstVideoCodecFrame*>(item->data);
            if (frame != currentFrame)
                gst_video_decoder_release_frame(decoder, gst_video_codec_frame_ref(frame));
        }
        g_list_free_full(frames, reinterpret_cast<GDestroyNotify>(gst_video_codec_frame_unref));
    }

    priv->needsSyncPoint = true;
    createGPUProcessDecoder(self);
    if (priv->identifier)
        return true;

    Locker locker { priv->lock };
    priv->gpuProcessDidExit = true;
    return false;
}

static GstFlowReturn webkitRemoteVideoDecoderHandleFrame(GstVideoDecoder* decoder, GstVideoCodecFrame* frame)
{
    auto* self = WEBKIT_REMOTE_VIDEO_DECODER(decoder);
    auto* priv = self->priv;
    auto releaseFrame = makeScopeExit([frame] {
        gst_video_codec_frame_unref(frame);
    });

    {
        Locker locker { priv->lock };
        if (priv->hasFailed)
            return GST_FLOW_ERROR;
        if (priv->isFlushing)
            return GST_FLOW_FLUSHING;
    }

    if (!restartIfGPUProcessExited(self, frame)) {
        gst_video_decoder_drop_frame(decoder, gst_video_codec_frame_ref(frame));
        return GST_FLOW_OK;
    }
    if (!priv->identifier)
        return GST_FLOW_NOT_NEGOTIATED;
    if (priv->needsSyncPoint) {
        if (!GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame)) {
            gst_video_decoder_request_sync_point(decoder, frame, GST_VIDEO_DECODER_REQUEST_SYNC_POINT_DISCARD_INPUT);
            gst_video_decoder_drop_frame(decoder, gst_video_codec_frame_ref(frame));
            return GST_FLOW_OK;
        }
        priv->needsSyncPoint = false;
    }

    GstMappedBuffer mappedBuffer(frame->input_buffer, GST_MAP_READ);
    if (!mappedBuffer) {
        GST_ELEMENT_ERROR(decoder, STREAM, DECODE, ("Could not map the frame to decode"), (nullptr));
        return GST_FLOW_ERROR;
    }

    // Decode-only frames, like the hidden frames of AV1, have no output.
    bool isDecodeOnly = GST_VIDEO_CODEC_FRAME_IS_DECODE_ONLY(frame);
    auto frameNumber = frame->system_frame_number;
    RemoteGStreamerVideoDecoderManagerProxy::singleton().decode(*priv->identifier, frameNumber, mappedBuffer.span<uint8_t>(),
        fromGstClockTime(frame->pts), fromGstClockTime(frame->dts), fromGstClockTime(frame->duration), GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame), isDecodeOnly);
    if (isDecodeOnly)
        gst_video_decoder_release_frame(decoder, gst_video_codec_frame_ref(frame));

    // Like a local decoder, this returns once the decoder took the frame, after pushing what that output. Decoders
    // hold as many frames as they need, and the GPU process decodes no further ahead. The stream lock is released
    // while waiting, so that the decoded frames can be output.
    bool isFlushing;
    bool hasFailed;
    {
        Locker locker { priv->lock };
        auto isConsumed = [&] {
            return priv->consumedFrameNumber && *priv->consumedFrameNumber >= frameNumber;
        };
        bool isWaiting = false;
        while (!isConsumed() && !priv->isFlushing && !priv->hasFailed && !priv->gpuProcessDidExit) {
            if (!isWaiting) {
                GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
                isWaiting = true;
            }
            priv->condition.wait(priv->lock);
        }
        isFlushing = priv->isFlushing;
        hasFailed = priv->hasFailed;
        if (isWaiting) {
            locker.unlockEarly();
            GST_VIDEO_DECODER_STREAM_LOCK(decoder);
        }
    }
    if (hasFailed)
        return GST_FLOW_ERROR;
    if (isFlushing)
        return GST_FLOW_FLUSHING;
    return GST_FLOW_OK;
}

static gboolean webkitRemoteVideoDecoderFlush(GstVideoDecoder* decoder)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(decoder)->priv;
    GST_DEBUG_OBJECT(decoder, "Flushing");
    // The frames that the GPU process decoded before are dropped when they arrive, as GstVideoDecoder
    // no longer has them.
    if (priv->identifier)
        RemoteGStreamerVideoDecoderManagerProxy::singleton().flush(*priv->identifier);
    return TRUE;
}

static GstFlowReturn webkitRemoteVideoDecoderDrain(GstVideoDecoder* decoder)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(decoder)->priv;
    RefPtr<WorkQueue> outputQueue;
    {
        Locker locker { priv->lock };
        outputQueue = priv->outputQueue;
    }
    if (!priv->identifier || !outputQueue)
        return GST_FLOW_OK;

    GST_DEBUG_OBJECT(decoder, "Draining");
    GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    RemoteGStreamerVideoDecoderManagerProxy::singleton().drain(*priv->identifier, *outputQueue);
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);

    // The frames that the decoder of the GPU process dropped are never output.
    GList* frames = gst_video_decoder_get_frames(decoder);
    for (auto* item = frames; item; item = g_list_next(item))
        gst_video_decoder_release_frame(decoder, gst_video_codec_frame_ref(static_cast<GstVideoCodecFrame*>(item->data)));
    g_list_free_full(frames, reinterpret_cast<GDestroyNotify>(gst_video_codec_frame_unref));

    Locker locker { priv->lock };
    return priv->hasFailed ? GST_FLOW_ERROR : GST_FLOW_OK;
}

static gboolean webkitRemoteVideoDecoderSinkEvent(GstVideoDecoder* decoder, GstEvent* event)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(decoder)->priv;
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_START: {
        Locker locker { priv->lock };
        priv->isFlushing = true;
        priv->condition.notifyAll();
        break;
    }
    case GST_EVENT_FLUSH_STOP: {
        Locker locker { priv->lock };
        priv->isFlushing = false;
        break;
    }
    default:
        break;
    }
    return GST_VIDEO_DECODER_CLASS(webkit_remote_video_decoder_parent_class)->sink_event(decoder, event);
}

static void webkitRemoteVideoDecoderConstructed(GObject* object)
{
    G_OBJECT_CLASS(webkit_remote_video_decoder_parent_class)->constructed(object);
    gst_video_decoder_set_packetized(GST_VIDEO_DECODER(object), TRUE);
}

static void webkitRemoteVideoDecoderSetProperty(GObject* object, guint propertyID, const GValue* value, GParamSpec* pspec)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(object)->priv;
    switch (propertyID) {
    case PROP_HARNESSED:
        priv->isHarnessed = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propertyID, pspec);
        break;
    }
}

static void webkitRemoteVideoDecoderGetProperty(GObject* object, guint propertyID, GValue* value, GParamSpec* pspec)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(object)->priv;
    switch (propertyID) {
    case PROP_HARNESSED:
        g_value_set_boolean(value, priv->isHarnessed);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propertyID, pspec);
        break;
    }
}

static void webkit_remote_video_decoder_class_init(WebKitRemoteVideoDecoderClass* klass)
{
    auto* objectClass = G_OBJECT_CLASS(klass);
    objectClass->constructed = webkitRemoteVideoDecoderConstructed;
    objectClass->set_property = webkitRemoteVideoDecoderSetProperty;
    objectClass->get_property = webkitRemoteVideoDecoderGetProperty;

    g_object_class_install_property(objectClass, PROP_HARNESSED, g_param_spec_boolean("harnessed", nullptr, nullptr, FALSE,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    auto* decoderClass = GST_VIDEO_DECODER_CLASS(klass);
    decoderClass->start = webkitRemoteVideoDecoderStart;
    decoderClass->stop = webkitRemoteVideoDecoderStop;
    decoderClass->set_format = webkitRemoteVideoDecoderSetFormat;
    decoderClass->handle_frame = webkitRemoteVideoDecoderHandleFrame;
    decoderClass->flush = webkitRemoteVideoDecoderFlush;
    decoderClass->drain = webkitRemoteVideoDecoderDrain;
    decoderClass->finish = webkitRemoteVideoDecoderDrain;
    decoderClass->sink_event = webkitRemoteVideoDecoderSinkEvent;
}

static GstVideoCodecFrame* frameForOutput(GstVideoDecoder* decoder, std::optional<uint32_t> frameNumber, const MediaTime& presentationTime)
{
    if (frameNumber)
        return gst_video_decoder_get_frame(decoder, *frameNumber);

    GstVideoCodecFrame* result = nullptr;
    auto timestamp = toGstClockTime(presentationTime);
    GList* frames = gst_video_decoder_get_frames(decoder);
    for (auto* item = frames; item && !result; item = g_list_next(item)) {
        auto* frame = static_cast<GstVideoCodecFrame*>(item->data);
        if (frame->pts == timestamp)
            result = gst_video_codec_frame_ref(frame);
    }
    g_list_free_full(frames, reinterpret_cast<GDestroyNotify>(gst_video_codec_frame_unref));
    return result ? result : gst_video_decoder_get_oldest_frame(decoder);
}

// A frame decoded before and presented before the one that is output was dropped by the decoder.
static void releaseDroppedFrames(GstVideoDecoder* decoder, GstVideoCodecFrame* outputFrame)
{
    if (!GST_CLOCK_TIME_IS_VALID(outputFrame->pts))
        return;

    GList* frames = gst_video_decoder_get_frames(decoder);
    for (auto* item = frames; item; item = g_list_next(item)) {
        auto* frame = static_cast<GstVideoCodecFrame*>(item->data);
        if (frame == outputFrame || frame->system_frame_number >= outputFrame->system_frame_number)
            continue;
        if (!GST_CLOCK_TIME_IS_VALID(frame->pts) || frame->pts >= outputFrame->pts)
            continue;
        GST_DEBUG_OBJECT(decoder, "Releasing frame %u, which the GPU process dropped", frame->system_frame_number);
        gst_video_decoder_release_frame(decoder, gst_video_codec_frame_ref(frame));
    }
    g_list_free_full(frames, reinterpret_cast<GDestroyNotify>(gst_video_codec_frame_unref));
}

// Downstream gets the frames of the GPU process when it takes the memory:WebKitVideoFrame feature,
// like the video sink when the GPU process composites the layers, and their pixels otherwise. Taking
// any caps, as the image decoder does, isn't taking the feature.
static bool ensureOutputState(WebKitRemoteVideoDecoder* self, VideoFrame& frame, IntSize size)
{
    auto* priv = self->priv;
    auto* decoder = GST_VIDEO_DECODER(self);
    auto format = static_cast<GstVideoFormat>(frame.pixelFormat());
    auto outputFormat = std::tuple { format, size.width(), size.height() };
    if (priv->outputFormat == outputFormat)
        return true;

    if (format == GST_VIDEO_FORMAT_UNKNOWN || format == GST_VIDEO_FORMAT_ENCODED)
        return false;

    // The pixel aspect ratio comes from the input.
    auto* state = gst_video_decoder_set_output_state(decoder, format, size.width(), size.height(), priv->inputState);
    if (!state)
        return false;
    GRefPtr<GstCaps> wrappedCaps = capsWithVideoFrameFeature(state->info);
    GRefPtr<GstCaps> peerCaps = adoptGRef(gst_pad_peer_query_caps(GST_VIDEO_DECODER_SRC_PAD(decoder), nullptr));
    priv->outputsWrappedFrames = wrappedCaps && peerCaps && !gst_caps_is_any(peerCaps.get()) && gst_pad_peer_query_accept_caps(GST_VIDEO_DECODER_SRC_PAD(decoder), wrappedCaps.get());
    if (priv->outputsWrappedFrames)
        state->caps = wrappedCaps.leakRef();
    gst_video_codec_state_unref(state);
    GST_DEBUG_OBJECT(decoder, "Outputting %s %dx%d %s", gst_video_format_to_string(format), size.width(), size.height(), priv->outputsWrappedFrames ? "frames of the GPU process" : "pixels read back");

    priv->outputFormat = outputFormat;
    return gst_video_decoder_negotiate(decoder);
}

static void finishFrame(WebKitRemoteVideoDecoder* self, std::optional<uint32_t> frameNumber, Ref<VideoFrame>&& videoFrame, IntSize decodedSize)
{
    auto* decoder = GST_VIDEO_DECODER(self);
    GST_VIDEO_DECODER_STREAM_LOCK(decoder);
    auto unlock = makeScopeExit([decoder] {
        GST_VIDEO_DECODER_STREAM_UNLOCK(decoder);
    });

    auto* frame = frameForOutput(decoder, frameNumber, videoFrame->presentationTime());
    if (!frame)
        return;

    releaseDroppedFrames(decoder, frame);

    if (!ensureOutputState(self, videoFrame, decodedSize)) {
        GST_WARNING_OBJECT(decoder, "Could not negotiate the output for frame %u", frame->system_frame_number);
        gst_video_decoder_drop_frame(decoder, frame);
        return;
    }

    GRefPtr<GstBuffer> buffer;
    if (self->priv->outputsWrappedFrames)
        buffer = bufferWrappingVideoFrame(videoFrame);
    else if (RefPtr localFrame = videoFrame->asVideoFrameGStreamer())
        buffer = gst_sample_get_buffer(localFrame->sample().get());
    if (!buffer) {
        gst_video_decoder_drop_frame(decoder, frame);
        return;
    }

    frame->output_buffer = buffer.leakRef();
    gst_video_decoder_finish_frame(decoder, frame);
}

namespace WebKit {

void remoteVideoDecoderDidDecodeFrame(GstElement* element, std::optional<uint32_t> frameNumber, Ref<VideoFrame>&& videoFrame, IntSize decodedSize)
{
    auto* self = WEBKIT_REMOTE_VIDEO_DECODER(element);
    RefPtr<WorkQueue> outputQueue;
    {
        Locker locker { self->priv->lock };
        outputQueue = self->priv->outputQueue;
    }
    if (!outputQueue)
        return;

    // Output in a queue of each decoder, since pushing downstream can block until the frame is presented.
    outputQueue->dispatch([element = GRefPtr<GstElement> { element }, frameNumber, videoFrame = WTF::move(videoFrame), decodedSize] mutable {
        finishFrame(WEBKIT_REMOTE_VIDEO_DECODER(element.get()), frameNumber, WTF::move(videoFrame), decodedSize);
    });
}

void remoteVideoDecoderDidConsumeFrame(GstElement* element, uint32_t frameNumber)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(element)->priv;
    RefPtr<WorkQueue> outputQueue;
    {
        Locker locker { priv->lock };
        outputQueue = priv->outputQueue;
    }
    if (!outputQueue)
        return;

    // After the frames that the decoder output while taking this one.
    outputQueue->dispatch([element = GRefPtr<GstElement> { element }, frameNumber] {
        auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(element.get())->priv;
        Locker locker { priv->lock };
        priv->consumedFrameNumber = frameNumber;
        priv->condition.notifyAll();
    });
}

void remoteVideoDecoderGPUProcessDidExit(GstElement* element)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(element)->priv;
    // WebCodecs decoders close with an error, as they do when decoding fails.
    if (priv->isHarnessed) {
        remoteVideoDecoderDidFail(element, "The GPU process exited"_s);
        return;
    }

    {
        Locker locker { priv->lock };
        priv->gpuProcessDidExit = true;
        priv->condition.notifyAll();
    }
    // Players restart decoding at the keyframe before the current position, instead of waiting for the next one.
    gst_element_post_message(element, gst_message_new_element(GST_OBJECT_CAST(element), gst_structure_new_empty("webkit-video-decoder-needs-keyframe")));
}

void remoteVideoDecoderDidFail(GstElement* element, const String& message)
{
    auto* priv = WEBKIT_REMOTE_VIDEO_DECODER(element)->priv;
    {
        Locker locker { priv->lock };
        priv->hasFailed = true;
        priv->condition.notifyAll();
    }
    GST_ELEMENT_ERROR(element, STREAM, DECODE, ("The GPU process could not decode the video"), ("%s", message.utf8().legacyCStringPointer()));
}

struct RemoteVideoDecoderClassData {
    UTF8CString factoryName;
    UTF8CString sinkCaps;
    bool isHardwareAccelerated;
};

static void webkitRemoteVideoDecoderSubclassInit(gpointer klass, gpointer data)
{
    auto* classData = static_cast<RemoteVideoDecoderClassData*>(data);
    auto* elementClass = GST_ELEMENT_CLASS(klass);

    GRefPtr<GstCaps> sinkCaps = adoptGRef(gst_caps_from_string(classData->sinkCaps.legacyCStringPointer()));
    gst_element_class_add_pad_template(elementClass, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sinkCaps.get()));
    GRefPtr<GstCaps> sourceCaps = adoptGRef(gst_caps_from_string("video/x-raw(" WEBKIT_GST_CAPS_FEATURE_MEMORY_VIDEO_FRAME "); video/x-raw"));
    gst_element_class_add_pad_template(elementClass, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, sourceCaps.get()));

    auto longName = makeString("WebKit GPU process "_s, String::fromLatin1(classData->factoryName.legacyCStringPointer()));
    // The proxy is hardware accelerated as the decoder it stands for, for the users that choose by that.
    gst_element_class_set_metadata(elementClass, longName.utf8().legacyCStringPointer(), classData->isHardwareAccelerated ? "Codec/Decoder/Video/Hardware" : "Codec/Decoder/Video", "Decodes video in the GPU process", "Igalia");
    reinterpret_cast<WebKitRemoteVideoDecoderClass*>(klass)->factoryName = classData->factoryName.legacyCStringPointer();
}

void registerRemoteVideoDecoders(const Vector<GStreamerVideoDecoderDescription>& decoders)
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_remote_video_decoder_debug, "webkitremotevideodecoder", 0, "WebKit video decoder of the GPU process");
    });

    for (auto& description : decoders) {
        StringBuilder typeName;
        typeName.append("WebKitRemoteVideoDecoder_"_s);
        for (auto character : StringView(description.name).codeUnits())
            typeName.append(isASCIIAlphanumeric(character) ? character : static_cast<char16_t>('_'));
        auto typeNameString = typeName.toString().utf8();
        if (g_type_from_name(typeNameString.legacyCStringPointer()))
            continue;

        // The types are never unregistered, nor their data freed.
        auto* classData = new RemoteVideoDecoderClassData { description.name.utf8(), description.sinkCaps.utf8(), description.isHardwareAccelerated };
        GTypeInfo typeInfo { };
        typeInfo.class_size = sizeof(WebKitRemoteVideoDecoderClass);
        typeInfo.class_init = webkitRemoteVideoDecoderSubclassInit;
        typeInfo.class_data = classData;
        typeInfo.instance_size = sizeof(WebKitRemoteVideoDecoder);
        GType type = g_type_register_static(WEBKIT_TYPE_REMOTE_VIDEO_DECODER, typeNameString.legacyCStringPointer(), &typeInfo, static_cast<GTypeFlags>(0));

        auto elementName = makeString(WEBKIT_GPU_PROCESS_VIDEO_DECODER_PREFIX ""_s, description.name).utf8();
        if (!gst_element_register(nullptr, elementName.legacyCStringPointer(), description.rank, type))
            continue;

        // The proxy takes the place of exactly the decoder it stands for, and of no other.
        if (GRefPtr factory = adoptGRef(gst_element_factory_find(classData->factoryName.legacyCStringPointer())))
            gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE_CAST(factory.get()), GST_RANK_NONE);
        GST_INFO("%s decodes with the GPU process, at rank %u", elementName.legacyCStringPointer(), description.rank);
    }
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
