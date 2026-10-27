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

#pragma once

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include "RemoteGStreamerVideoDecoderIdentifier.h"
#include <WebCore/GRefPtrGStreamer.h>
#include <span>
#include <wtf/CompletionHandler.h>
#include <wtf/MediaTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WorkQueue.h>

namespace IPC {
class Connection;
}

namespace WebKit {

class RemoteVideoFrameObjectHeap;

// One decoder element of the GPU process, in its own pipeline: appsrc ! decoder ! videoconvert ! appsink.
// It decodes the frames that the proxy element of the web process sends, and puts the frames it
// outputs into the RemoteVideoFrameObjectHeap.
class RemoteGStreamerVideoDecoder final : public ThreadSafeRefCounted<RemoteGStreamerVideoDecoder> {
public:
    static RefPtr<RemoteGStreamerVideoDecoder> create(RemoteGStreamerVideoDecoderIdentifier, const String& factoryName, const String& caps, IPC::Connection&, RemoteVideoFrameObjectHeap&, WorkQueue&);
    ~RemoteGStreamerVideoDecoder();

    // Called in the work queue.
    void setFormat(const String& caps);
    void decode(uint32_t frameNumber, std::span<const uint8_t>, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool isKeyFrame);
    void flush();
    void drain(CompletionHandler<void()>&&);
    void close();

private:
    RemoteGStreamerVideoDecoder(RemoteGStreamerVideoDecoderIdentifier, IPC::Connection&, RemoteVideoFrameObjectHeap&, WorkQueue&);

    bool initialize(const String& factoryName, const String& caps);
    void restart();

    // Called in the streaming threads.
    void sampleAvailable(GRefPtr<GstSample>&&);
    void didReachEndOfStream();
    void didFail(GstMessage*);

    const RemoteGStreamerVideoDecoderIdentifier m_identifier;
    const Ref<IPC::Connection> m_connection;
    const Ref<RemoteVideoFrameObjectHeap> m_videoFrameObjectHeap;
    const Ref<WorkQueue> m_queue;
    GRefPtr<GstElement> m_pipeline;
    GRefPtr<GstElement> m_source;
    CompletionHandler<void()> m_drainCompletionHandler;
    bool m_isDraining { false };
    bool m_hasFailed { false };
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
