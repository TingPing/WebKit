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
#include "RemoteGStreamerVideoDecoderManager.h"

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include "Connection.h"
#include "GPUConnectionToWebProcess.h"
#include "Logging.h"
#include "RemoteGStreamerVideoDecoder.h"
#include "RemoteGStreamerVideoDecoderManagerMessages.h"
#include "RemoteGStreamerVideoDecoderManagerProxyMessages.h"
#include "RemoteVideoFrameObjectHeap.h"
#include <WebCore/GStreamerCommon.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#define MESSAGE_CHECK(assertion) MESSAGE_CHECK_BASE(assertion, m_connection)

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteGStreamerVideoDecoderManager);

Ref<RemoteGStreamerVideoDecoderManager> RemoteGStreamerVideoDecoderManager::create(GPUConnectionToWebProcess& connectionToWebProcess, const Vector<WebCore::GStreamerVideoDecoderDescription>& decoders)
{
    Ref manager = adoptRef(*new RemoteGStreamerVideoDecoderManager(connectionToWebProcess, decoders));
    manager->m_connection->addWorkQueueMessageReceiver(Messages::RemoteGStreamerVideoDecoderManager::messageReceiverName(), manager->m_queue, manager.get());
    return manager;
}

RemoteGStreamerVideoDecoderManager::RemoteGStreamerVideoDecoderManager(GPUConnectionToWebProcess& connectionToWebProcess, const Vector<WebCore::GStreamerVideoDecoderDescription>& decoders)
    : m_connection(connectionToWebProcess.connection())
    , m_queue(WorkQueue::create("org.webkit.RemoteGStreamerVideoDecoderManager"_s, WorkQueue::QOS::UserInteractive))
    , m_videoFrameObjectHeap(connectionToWebProcess.videoFrameObjectHeap())
{
    for (auto& decoder : decoders)
        m_factoryNames.add(decoder.name);
}

RemoteGStreamerVideoDecoderManager::~RemoteGStreamerVideoDecoderManager() = default;

void RemoteGStreamerVideoDecoderManager::close()
{
    m_connection->removeWorkQueueMessageReceiver(Messages::RemoteGStreamerVideoDecoderManager::messageReceiverName());
    m_queue->dispatch([protectedThis = Ref { *this }] {
        assertIsCurrent(protectedThis->workQueue());
        auto decoders = std::exchange(protectedThis->m_decoders, { });
        for (Ref decoder : decoders.values())
            decoder->close();
    });
}

void RemoteGStreamerVideoDecoderManager::createDecoder(RemoteGStreamerVideoDecoderIdentifier identifier, const String& factoryName, const String& caps)
{
    assertIsCurrent(workQueue());
    MESSAGE_CHECK(!m_decoders.contains(identifier));
    // The web process only creates the decoders that this process reported.
    MESSAGE_CHECK(m_factoryNames.contains(factoryName));

    RefPtr decoder = RemoteGStreamerVideoDecoder::create(identifier, factoryName, caps, m_connection, m_videoFrameObjectHeap, m_queue);
    if (!decoder) {
        m_connection->send(Messages::RemoteGStreamerVideoDecoderManagerProxy::DecoderFailed(identifier, makeString("Could not create "_s, factoryName)), 0);
        return;
    }
    m_decoders.add(identifier, decoder.releaseNonNull());
}

void RemoteGStreamerVideoDecoderManager::releaseDecoder(RemoteGStreamerVideoDecoderIdentifier identifier)
{
    assertIsCurrent(workQueue());
    if (RefPtr decoder = m_decoders.take(identifier))
        decoder->close();
}

void RemoteGStreamerVideoDecoderManager::setFormat(RemoteGStreamerVideoDecoderIdentifier identifier, const String& caps)
{
    assertIsCurrent(workQueue());
    if (RefPtr decoder = m_decoders.get(identifier))
        decoder->setFormat(caps);
}

void RemoteGStreamerVideoDecoderManager::decode(RemoteGStreamerVideoDecoderIdentifier identifier, uint32_t frameNumber, std::span<const uint8_t> data, MediaTime presentationTime, MediaTime decodeTime, MediaTime duration, bool isKeyFrame)
{
    assertIsCurrent(workQueue());
    if (RefPtr decoder = m_decoders.get(identifier))
        decoder->decode(frameNumber, data, presentationTime, decodeTime, duration, isKeyFrame);
}

void RemoteGStreamerVideoDecoderManager::flush(RemoteGStreamerVideoDecoderIdentifier identifier)
{
    assertIsCurrent(workQueue());
    if (RefPtr decoder = m_decoders.get(identifier))
        decoder->flush();
}

void RemoteGStreamerVideoDecoderManager::drain(RemoteGStreamerVideoDecoderIdentifier identifier, CompletionHandler<void()>&& completionHandler)
{
    assertIsCurrent(workQueue());
    RefPtr decoder = m_decoders.get(identifier);
    if (!decoder) {
        completionHandler();
        return;
    }
    decoder->drain(WTF::move(completionHandler));
}

} // namespace WebKit

#undef MESSAGE_CHECK

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
