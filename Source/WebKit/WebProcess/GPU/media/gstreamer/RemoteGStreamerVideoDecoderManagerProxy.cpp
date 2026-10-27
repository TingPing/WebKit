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
#include "RemoteGStreamerVideoDecoderManagerProxy.h"

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include "RemoteGStreamerVideoDecoderManagerMessages.h"
#include "RemoteGStreamerVideoDecoderManagerProxyMessages.h"
#include "RemoteVideoFrameObjectHeapProxy.h"
#include "WebKitRemoteVideoDecoderGStreamer.h"
#include "WebProcess.h"
#include <WebCore/GStreamerCommon.h>
#include <WebCore/ProcessCapabilities.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/threads/BinarySemaphore.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteGStreamerVideoDecoderManagerProxy);

RemoteGStreamerVideoDecoderManagerProxy& RemoteGStreamerVideoDecoderManagerProxy::singleton()
{
    static NeverDestroyed<Ref<RemoteGStreamerVideoDecoderManagerProxy>> manager = adoptRef(*new RemoteGStreamerVideoDecoderManagerProxy);
    return manager.get();
}

RemoteGStreamerVideoDecoderManagerProxy::RemoteGStreamerVideoDecoderManagerProxy()
    : m_queue(WorkQueue::create("org.webkit.RemoteGStreamerVideoDecoderManagerProxy"_s, WorkQueue::QOS::UserInteractive))
{
}

void RemoteGStreamerVideoDecoderManagerProxy::installDecoderRegistrar()
{
    ASSERT(isMainRunLoop());
    // For debugging until the web process loses access to the GPU.
    auto codecsInGPUProcess = StringView::fromLatin1(g_getenv("WEBKIT_GST_GPU_PROCESS_CODECS"));
    if (codecsInGPUProcess == "0"_s)
        return;
    setGPUProcessVideoDecodersRegistrar(registerDecoders);
}

void RemoteGStreamerVideoDecoderManagerProxy::registerDecoders()
{
    if (!ProcessCapabilities::layersCompositedRemotely())
        return;

    Vector<GStreamerVideoDecoderDescription> decoders;
    callOnMainRunLoopAndWait([&] {
        decoders = protect(WebProcess::singleton().ensureGPUProcessConnection())->hardwareVideoDecoders();
    });
    if (decoders.isEmpty())
        return;

    // Connected now, so that the streaming threads of the decoders don't wait for the main thread.
    singleton().connection();
    registerRemoteVideoDecoders(decoders);
}

RefPtr<IPC::Connection> RemoteGStreamerVideoDecoderManagerProxy::connection()
{
    {
        Locker locker { m_lock };
        if (m_connection)
            return m_connection;
    }

    callOnMainRunLoopAndWait([this] {
        Locker locker { m_lock };
        if (m_connection)
            return;
        Ref gpuProcessConnection = WebProcess::singleton().ensureGPUProcessConnection();
        gpuProcessConnection->addClient(*this);
        m_connection = gpuProcessConnection->connection();
        m_videoFrameObjectHeapProxy = &gpuProcessConnection->videoFrameObjectHeapProxy();
        protect(m_connection)->addWorkQueueMessageReceiver(Messages::RemoteGStreamerVideoDecoderManagerProxy::messageReceiverName(), m_queue, *this);
    });

    Locker locker { m_lock };
    return m_connection;
}

void RemoteGStreamerVideoDecoderManagerProxy::gpuProcessConnectionDidClose(GPUProcessConnection&)
{
    // FIXME: The decoders should restart in the new GPU process instead.
    HashMap<RemoteGStreamerVideoDecoderIdentifier, GRefPtr<GstElement>> decoders;
    {
        Locker locker { m_lock };
        if (RefPtr connection = std::exchange(m_connection, nullptr))
            connection->removeWorkQueueMessageReceiver(Messages::RemoteGStreamerVideoDecoderManagerProxy::messageReceiverName());
        m_videoFrameObjectHeapProxy = nullptr;
        decoders = std::exchange(m_decoders, { });
    }
    m_queue->dispatch([decoders = WTF::move(decoders)] {
        for (auto& decoder : decoders.values())
            remoteVideoDecoderDidFail(decoder.get(), "The GPU process exited"_s);
    });
}

GRefPtr<GstElement> RemoteGStreamerVideoDecoderManagerProxy::decoder(RemoteGStreamerVideoDecoderIdentifier identifier)
{
    Locker locker { m_lock };
    return m_decoders.get(identifier);
}

std::optional<RemoteGStreamerVideoDecoderIdentifier> RemoteGStreamerVideoDecoderManagerProxy::createDecoder(GstElement* decoder, const String& factoryName, const String& caps)
{
    RefPtr connection = this->connection();
    if (!connection)
        return std::nullopt;

    auto identifier = RemoteGStreamerVideoDecoderIdentifier::generate();
    {
        Locker locker { m_lock };
        m_decoders.add(identifier, decoder);
    }
    connection->send(Messages::RemoteGStreamerVideoDecoderManager::CreateDecoder(identifier, factoryName, caps), 0);
    return identifier;
}

void RemoteGStreamerVideoDecoderManagerProxy::releaseDecoder(RemoteGStreamerVideoDecoderIdentifier identifier)
{
    RefPtr<IPC::Connection> connection;
    {
        Locker locker { m_lock };
        m_decoders.remove(identifier);
        connection = m_connection;
    }
    if (connection)
        connection->send(Messages::RemoteGStreamerVideoDecoderManager::ReleaseDecoder(identifier), 0);
}

void RemoteGStreamerVideoDecoderManagerProxy::setFormat(RemoteGStreamerVideoDecoderIdentifier identifier, const String& caps)
{
    if (RefPtr connection = this->connection())
        connection->send(Messages::RemoteGStreamerVideoDecoderManager::SetFormat(identifier, caps), 0);
}

void RemoteGStreamerVideoDecoderManagerProxy::decode(RemoteGStreamerVideoDecoderIdentifier identifier, uint32_t frameNumber, std::span<const uint8_t> data, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool isKeyFrame)
{
    if (RefPtr connection = this->connection())
        connection->send(Messages::RemoteGStreamerVideoDecoderManager::Decode(identifier, frameNumber, data, presentationTime, decodeTime, duration, isKeyFrame), 0);
}

void RemoteGStreamerVideoDecoderManagerProxy::flush(RemoteGStreamerVideoDecoderIdentifier identifier)
{
    if (RefPtr connection = this->connection())
        connection->send(Messages::RemoteGStreamerVideoDecoderManager::Flush(identifier), 0);
}

void RemoteGStreamerVideoDecoderManagerProxy::drain(RemoteGStreamerVideoDecoderIdentifier identifier, WorkQueue& outputQueue)
{
    RefPtr connection = this->connection();
    if (!connection)
        return;

    // The reply comes after the frames, which this queue gives to the output queue before it.
    BinarySemaphore semaphore;
    connection->sendWithAsyncReplyOnDispatcher(Messages::RemoteGStreamerVideoDecoderManager::Drain(identifier), m_queue, [&semaphore, outputQueue = Ref { outputQueue }] {
        outputQueue->dispatch([&semaphore] {
            semaphore.signal();
        });
    });
    semaphore.wait();
}

void RemoteGStreamerVideoDecoderManagerProxy::frameDecoded(RemoteGStreamerVideoDecoderIdentifier identifier, std::optional<uint32_t> frameNumber, RemoteVideoFrameProxy::Properties&& properties)
{
    RefPtr<IPC::Connection> connection;
    RefPtr<RemoteVideoFrameObjectHeapProxy> videoFrameObjectHeapProxy;
    {
        Locker locker { m_lock };
        connection = m_connection;
        videoFrameObjectHeapProxy = m_videoFrameObjectHeapProxy;
    }
    if (!connection || !videoFrameObjectHeapProxy)
        return;

    // The proxy releases the frame of the GPU process, also when no decoder outputs it anymore.
    Ref<VideoFrame> frame = RemoteVideoFrameProxy::create(*connection, *videoFrameObjectHeapProxy, WTF::move(properties));
    if (auto decoder = this->decoder(identifier))
        remoteVideoDecoderDidDecodeFrame(decoder.get(), frameNumber, WTF::move(frame));
}

void RemoteGStreamerVideoDecoderManagerProxy::decoderFailed(RemoteGStreamerVideoDecoderIdentifier identifier, String&& message)
{
    if (auto decoder = this->decoder(identifier))
        remoteVideoDecoderDidFail(decoder.get(), message);
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
