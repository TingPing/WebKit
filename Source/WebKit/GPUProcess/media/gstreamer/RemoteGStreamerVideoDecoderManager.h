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
#include "WorkQueueMessageReceiver.h"
#include <wtf/CompletionHandler.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MediaTime.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WorkQueue.h>

namespace IPC {
class Connection;
}

namespace WebCore {
struct GStreamerVideoDecoderDescription;
}

namespace WebKit {

class GPUConnectionToWebProcess;
class RemoteGStreamerVideoDecoder;
class RemoteVideoFrameObjectHeap;

// The decoders that the GPU process runs for one web process, which are GStreamer elements there.
class RemoteGStreamerVideoDecoderManager final : public IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteGStreamerVideoDecoderManager);
public:
    static Ref<RemoteGStreamerVideoDecoderManager> create(GPUConnectionToWebProcess&, const Vector<WebCore::GStreamerVideoDecoderDescription>&);
    ~RemoteGStreamerVideoDecoderManager();

    void close();

private:
    RemoteGStreamerVideoDecoderManager(GPUConnectionToWebProcess&, const Vector<WebCore::GStreamerVideoDecoderDescription>&);

    // IPC::WorkQueueMessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    void createDecoder(RemoteGStreamerVideoDecoderIdentifier, const String& factoryName, const String& caps);
    void releaseDecoder(RemoteGStreamerVideoDecoderIdentifier);
    void setFormat(RemoteGStreamerVideoDecoderIdentifier, const String& caps);
    void decode(RemoteGStreamerVideoDecoderIdentifier, uint32_t frameNumber, std::span<const uint8_t>, MediaTime presentationTime, MediaTime decodeTime, MediaTime duration, bool isKeyFrame);
    void flush(RemoteGStreamerVideoDecoderIdentifier);
    void drain(RemoteGStreamerVideoDecoderIdentifier, CompletionHandler<void()>&&);

    WorkQueue& workQueue() const { return m_queue; }

    const Ref<IPC::Connection> m_connection;
    const Ref<WorkQueue> m_queue;
    const Ref<RemoteVideoFrameObjectHeap> m_videoFrameObjectHeap;
    HashSet<String> m_factoryNames;
    HashMap<RemoteGStreamerVideoDecoderIdentifier, Ref<RemoteGStreamerVideoDecoder>> m_decoders WTF_GUARDED_BY_CAPABILITY(workQueue());
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
