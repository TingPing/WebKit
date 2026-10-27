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

#include "GPUProcessConnection.h"
#include "RemoteGStreamerVideoDecoderIdentifier.h"
#include "RemoteVideoFrameProxy.h"
#include "WorkQueueMessageReceiver.h"
#include <WebCore/GRefPtrGStreamer.h>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MediaTime.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WorkQueue.h>

namespace WebKit {

// The decoders that the GPU process runs for this process, which stand for them in its pipelines.
class RemoteGStreamerVideoDecoderManagerProxy final : public IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any>, public GPUProcessConnection::Client {
    WTF_MAKE_TZONE_ALLOCATED(RemoteGStreamerVideoDecoderManagerProxy);
public:
    static RemoteGStreamerVideoDecoderManagerProxy& singleton();

    // Has the decoders of the GPU process replace the local ones, when it composites the layers.
    static void installDecoderRegistrar();

    void ref() const final { IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any>::ref(); }
    void deref() const final { IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any>::deref(); }
    ThreadSafeWeakPtrControlBlock& controlBlock() const final { return IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any>::controlBlock(); }
    uint32_t weakRefCount() const final { return IPC::WorkQueueMessageReceiver<WTF::DestructionThread::Any>::weakRefCount(); }

    // Called in the streaming threads of the decoders.
    std::optional<RemoteGStreamerVideoDecoderIdentifier> createDecoder(GstElement*, const String& factoryName, const String& caps);
    void releaseDecoder(RemoteGStreamerVideoDecoderIdentifier);
    void setFormat(RemoteGStreamerVideoDecoderIdentifier, const String& caps);
    void decode(RemoteGStreamerVideoDecoderIdentifier, uint32_t frameNumber, std::span<const uint8_t>, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool isKeyFrame);
    void flush(RemoteGStreamerVideoDecoderIdentifier);
    // Returns once the frames decoded so far were given to the decoder, in its output queue.
    void drain(RemoteGStreamerVideoDecoderIdentifier, WorkQueue& outputQueue);

private:
    RemoteGStreamerVideoDecoderManagerProxy();

    static void registerDecoders();

    // IPC::WorkQueueMessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    // GPUProcessConnection::Client
    void gpuProcessConnectionDidClose(GPUProcessConnection&) final;

    void frameDecoded(RemoteGStreamerVideoDecoderIdentifier, std::optional<uint32_t> frameNumber, RemoteVideoFrameProxy::Properties&&);
    void decoderFailed(RemoteGStreamerVideoDecoderIdentifier, String&& message);

    RefPtr<IPC::Connection> connection();
    GRefPtr<GstElement> decoder(RemoteGStreamerVideoDecoderIdentifier);

    const Ref<WorkQueue> m_queue;
    Lock m_lock;
    RefPtr<IPC::Connection> m_connection WTF_GUARDED_BY_LOCK(m_lock);
    RefPtr<RemoteVideoFrameObjectHeapProxy> m_videoFrameObjectHeapProxy WTF_GUARDED_BY_LOCK(m_lock);
    // The decoders remove themselves when they stop.
    HashMap<RemoteGStreamerVideoDecoderIdentifier, GRefPtr<GstElement>> m_decoders WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
