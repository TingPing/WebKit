/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RemoteVideoFrameProxy.h"

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO)
#include "GPUConnectionToWebProcess.h"
#include "RemoteVideoFrameObjectHeapMessages.h"
#include "RemoteVideoFrameObjectHeapProxy.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/threads/BinarySemaphore.h>

#if PLATFORM(COCOA)
#include "WebProcess.h"
#include <WebCore/CVUtilities.h>
#include <WebCore/RealtimeIncomingVideoSourceCocoa.h>
#include <WebCore/VideoFrameCV.h>
#include <wtf/MainThread.h>
#endif

#if USE(GSTREAMER)
#include "GStreamerVideoFrameLayout.h"
#include <WebCore/VideoFrameGStreamer.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/WorkQueue.h>
#endif

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteVideoFrameProxy);

RemoteVideoFrameProxy::Properties RemoteVideoFrameProxy::properties(WebKit::RemoteVideoFrameReference reference, const WebCore::VideoFrame& videoFrame)
{
    return {
        WTF::move(reference),
        videoFrame.presentationTime(),
        videoFrame.isMirrored(),
        videoFrame.rotation(),
        expandedIntSize(videoFrame.presentationSize()),
        videoFrame.pixelFormat(),
        videoFrame.colorSpace(),
        videoFrame.shouldIgnoreAlpha()
    };
}

Ref<RemoteVideoFrameProxy> RemoteVideoFrameProxy::create(IPC::Connection& connection, RemoteVideoFrameObjectHeapProxy& videoFrameObjectHeapProxy, Properties&& properties)
{
    return adoptRef(*new RemoteVideoFrameProxy(connection, videoFrameObjectHeapProxy, WTF::move(properties)));
}

static void releaseRemoteVideoFrameProxy(IPC::Connection& connection, const RemoteVideoFrameWriteReference& reference)
{
    connection.send(Messages::RemoteVideoFrameObjectHeap::ReleaseVideoFrame(reference), 0, IPC::SendOption::DispatchMessageEvenWhenWaitingForSyncReply);
}

void RemoteVideoFrameProxy::releaseUnused(IPC::Connection& connection, Properties&& properties)
{
    releaseRemoteVideoFrameProxy(connection, { { properties.reference.identifier(), properties.reference.version() }, 0 });
}

RemoteVideoFrameProxy::RemoteVideoFrameProxy(IPC::Connection& connection, RemoteVideoFrameObjectHeapProxy& videoFrameObjectHeapProxy, Properties&& properties)
    : VideoFrame(properties.presentationTime, properties.isMirrored, properties.rotation, WTF::move(properties.colorSpace))
    , m_connection(&connection)
    , m_referenceTracker(properties.reference)
    , m_size(properties.size)
    , m_pixelFormat(properties.pixelFormat)
    , m_shouldIgnoreAlpha(properties.shouldIgnoreAlpha)
    , m_videoFrameObjectHeapProxy(&videoFrameObjectHeapProxy)
{
}

RemoteVideoFrameProxy::RemoteVideoFrameProxy(CloneConstructor, RemoteVideoFrameProxy& baseVideoFrame)
    : VideoFrame(baseVideoFrame.presentationTime(), baseVideoFrame.isMirrored(), baseVideoFrame.rotation(), WebCore::PlatformVideoColorSpace { baseVideoFrame.colorSpace() })
    , m_baseVideoFrame(&baseVideoFrame)
    , m_size(baseVideoFrame.m_size)
    , m_pixelFormat(baseVideoFrame.m_pixelFormat)
    , m_shouldIgnoreAlpha(baseVideoFrame.m_shouldIgnoreAlpha)
{
}

RemoteVideoFrameProxy::~RemoteVideoFrameProxy()
{
    if (m_connection)
        releaseRemoteVideoFrameProxy(*m_connection, m_referenceTracker->write());
}

RemoteVideoFrameIdentifier RemoteVideoFrameProxy::identifier() const
{
    return m_baseVideoFrame ? m_baseVideoFrame->m_referenceTracker->identifier() : m_referenceTracker->identifier();
}

RemoteVideoFrameReadReference RemoteVideoFrameProxy::newReadReference() const
{
    return m_baseVideoFrame ? m_baseVideoFrame->m_referenceTracker->read() : m_referenceTracker->read();
}

uint32_t RemoteVideoFrameProxy::pixelFormat() const
{
    return m_pixelFormat;
}

#if PLATFORM(COCOA)
CVPixelBufferRef RemoteVideoFrameProxy::pixelBuffer() const
{
    if (m_baseVideoFrame)
        return m_baseVideoFrame->pixelBuffer();

    Locker lock(m_pixelBufferLock);
    if (!m_pixelBuffer && m_videoFrameObjectHeapProxy) {
        auto videoFrameObjectHeapProxy = std::exchange(m_videoFrameObjectHeapProxy, nullptr);

        bool canSendSync = isMainRunLoop(); // FIXME: we should be able to sendSync from other threads too.
        bool canUseIOSurface = !WebProcess::singleton().shouldUseRemoteRenderingForWebGL();
        if (!canUseIOSurface || !canSendSync) {
            Ref protectedThis { *this };
            BinarySemaphore semaphore;
            videoFrameObjectHeapProxy->getVideoFrameBuffer(*this, canUseIOSurface, [&protectedThis, &semaphore](auto pixelBuffer) {
                protectedThis->m_pixelBuffer = WTF::move(pixelBuffer);
                semaphore.signal();
            });
            semaphore.wait();
        } else {
            auto sendResult = m_connection->sendSync(Messages::RemoteVideoFrameObjectHeap::PixelBuffer(newReadReference()), 0, defaultTimeout);
            if (sendResult.succeeded())
                std::tie(m_pixelBuffer) = sendResult.takeReply();
        }
    }
    // FIXME: Some code paths do not like empty pixel buffers.
    if (!m_pixelBuffer)
        m_pixelBuffer = WebCore::createBlackPixelBuffer(static_cast<size_t>(m_size.width()), static_cast<size_t>(m_size.height()));
    return m_pixelBuffer.get();
}
#endif

#if USE(GSTREAMER)
static WorkQueue& readbackQueueSingleton()
{
    static NeverDestroyed queue = WorkQueue::create("org.webkit.RemoteVideoFrameProxy.Readback"_s, WorkQueue::QOS::UserInteractive);
    return queue.get();
}

RefPtr<WebCore::VideoFrameGStreamer> RemoteVideoFrameProxy::asVideoFrameGStreamer() const
{
    if (m_baseVideoFrame)
        return m_baseVideoFrame->asVideoFrameGStreamer();

    Locker locker { m_localFrameLock };
    if (m_localFrame || !m_connection)
        return m_localFrame;

    WebCore::VideoFrameGStreamer::CreateOptions options;
    options.presentationTime = presentationTime();
    options.rotation = rotation();
    options.isMirrored = isMirrored();

    // The reply is handled on another queue, so this can wait for it on any thread.
    RefPtr<WebCore::VideoFrameGStreamer> frame;
    BinarySemaphore semaphore;
    protect(m_connection)->sendWithAsyncReplyOnDispatcher(Messages::RemoteVideoFrameObjectHeap::ReadVideoFrame(newReadReference()), readbackQueueSingleton(), [&](std::optional<WebCore::SharedMemory::Handle>&& handle, GStreamerVideoFrameLayout&& layout) {
        if (RefPtr memory = handle ? WebCore::SharedMemory::map(WTF::move(*handle), WebCore::SharedMemory::Protection::ReadOnly) : nullptr)
            frame = layout.createVideoFrame(memory.releaseNonNull(), WTF::move(options));
        semaphore.signal();
    });
    semaphore.wait();

    m_localFrame = WTF::move(frame);
    return m_localFrame;
}
#endif

Ref<WebCore::VideoFrame> RemoteVideoFrameProxy::clone()
{
    return adoptRef(*new RemoteVideoFrameProxy(cloneConstructor, *this));
}

TextStream& operator<<(TextStream& ts, const RemoteVideoFrameProxy::Properties& properties)
{
    ts << "{ reference="_s << properties.reference
        << ", presentationTime=" << properties.presentationTime
        << ", isMirrored=" << properties.isMirrored
        << ", rotation=" << static_cast<int>(properties.rotation)
        << ", size=" << properties.size
        << ", pixelFormat=" << properties.pixelFormat
        << ", shouldIgnoreAlpha=" << properties.shouldIgnoreAlpha
        << " }";
    return ts;
}

}
#endif
