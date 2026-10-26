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
#include "SharedVideoFrame.h"

#if ENABLE(GPU_PROCESS) && USE(GSTREAMER) && ENABLE(VIDEO)

#include "Logging.h"
#include "RemoteVideoFrameObjectHeap.h"
#include "RemoteVideoFrameProxy.h"
#include <WebCore/GStreamerCommon.h>
#include <WebCore/VideoFrameGStreamer.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedVideoFrameWriter);

SharedVideoFrameWriter::SharedVideoFrameWriter()
    : m_semaphore(makeUniqueRef<IPC::Semaphore>())
{
}

bool SharedVideoFrameWriter::wait(NOESCAPE const Function<void(IPC::Semaphore&)>& newSemaphoreCallback)
{
    if (!m_isSemaphoreInUse) {
        m_isSemaphoreInUse = true;
        newSemaphoreCallback(m_semaphore.get());
        return true;
    }
    return !WTF::atomicLoad(&m_isDisabled, std::memory_order_relaxed) && m_semaphore->waitFor(defaultTimeout);
}

bool SharedVideoFrameWriter::allocateStorage(size_t size, NOESCAPE const Function<void(SharedMemory::Handle&&)>& newMemoryCallback)
{
    RefPtr storage = SharedMemory::allocate(size);
    m_storage = storage;
    if (!storage)
        return false;

    auto handle = storage->createHandle(SharedMemory::Protection::ReadOnly);
    if (!handle)
        return false;

    newMemoryCallback(WTF::move(*handle));
    return true;
}

bool SharedVideoFrameWriter::prepareWriting(size_t size, NOESCAPE const Function<void(IPC::Semaphore&)>& newSemaphoreCallback, NOESCAPE const Function<void(SharedMemory::Handle&&)>& newMemoryCallback)
{
    if (!wait(newSemaphoreCallback)) {
        RELEASE_LOG_ERROR(Media, "SharedVideoFrameWriter::prepareWriting wait failed");
        return false;
    }
    m_shouldSignalInCaseOfError = true;

    if (!m_storage || m_storage->size() < size) {
        if (!allocateStorage(size, newMemoryCallback)) {
            RELEASE_LOG_ERROR(Media, "SharedVideoFrameWriter::prepareWriting allocation failed");
            return false;
        }
    }
    return true;
}

std::optional<SharedVideoFrame> SharedVideoFrameWriter::write(const VideoFrame& frame, NOESCAPE const Function<void(IPC::Semaphore&)>& newSemaphoreCallback, NOESCAPE const Function<void(SharedMemory::Handle&&)>& newMemoryCallback)
{
    auto buffer = writeBuffer(frame, newSemaphoreCallback, newMemoryCallback);
    if (!buffer)
        return { };
    return SharedVideoFrame { frame.presentationTime(), frame.isMirrored(), frame.rotation(), frame.colorSpace(), frame.shouldIgnoreAlpha(), WTF::move(*buffer) };
}

std::optional<SharedVideoFrame::Buffer> SharedVideoFrameWriter::writeBuffer(const VideoFrame& frame, NOESCAPE const Function<void(IPC::Semaphore&)>& newSemaphoreCallback, NOESCAPE const Function<void(SharedMemory::Handle&&)>& newMemoryCallback)
{
    if (auto* frameProxy = dynamicDowncast<RemoteVideoFrameProxy>(frame))
        return frameProxy->newReadReference();

    auto* gstreamerFrame = dynamicDowncast<VideoFrameGStreamer>(frame);
    if (!gstreamerFrame)
        return { };

    auto scope = makeScopeExit([this] {
        signalInCaseOfError();
    });
    auto layout = GStreamerVideoFrameLayout::write(*gstreamerFrame, [&](size_t size) -> std::span<uint8_t> {
        if (!prepareWriting(size, newSemaphoreCallback, newMemoryCallback))
            return { };
        return m_storage->mutableSpan();
    });
    if (!layout)
        return { };

    scope.release();
    return WTF::move(*layout);
}

void SharedVideoFrameWriter::signalInCaseOfError()
{
    if (!m_shouldSignalInCaseOfError)
        return;
    m_shouldSignalInCaseOfError = false;
    m_semaphore->signal();
}

void SharedVideoFrameWriter::disable()
{
    WTF::atomicStore(&m_isDisabled, true, std::memory_order_relaxed);
    m_semaphore->signal();
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedVideoFrameReader);

SharedVideoFrameReader::SharedVideoFrameReader(RefPtr<RemoteVideoFrameObjectHeap>&& objectHeap, const ProcessIdentity&)
    : m_objectHeap(WTF::move(objectHeap))
{
}

SharedVideoFrameReader::SharedVideoFrameReader() = default;

SharedVideoFrameReader::~SharedVideoFrameReader() = default;

static VideoFrameGStreamer::CreateOptions createOptions(const SharedVideoFrame& frame)
{
    VideoFrameGStreamer::CreateOptions options;
    options.presentationTime = frame.time;
    options.rotation = frame.rotation;
    options.isMirrored = frame.mirrored;
    return options;
}

RefPtr<VideoFrame> SharedVideoFrameReader::readFromSharedMemory(const GStreamerVideoFrameLayout& layout, SharedVideoFrame& frame)
{
    if (!m_storage) {
        RELEASE_LOG_ERROR(Media, "SharedVideoFrameReader::readFromSharedMemory no storage");
        return nullptr;
    }

    // The writer reuses the memory once signaled, so the planes are copied first.
    auto scope = makeScopeExit([&] {
        if (m_semaphore)
            m_semaphore->signal();
    });

    auto data = m_storage->span();
    GRefPtr<GstBuffer> buffer = adoptGRef(gst_buffer_new_memdup(data.data(), data.size()));
    RefPtr videoFrame = layout.createVideoFrame(WTF::move(buffer), createOptions(frame));
    if (!videoFrame)
        RELEASE_LOG_ERROR(Media, "SharedVideoFrameReader::readFromSharedMemory invalid layout");
    return videoFrame;
}

RefPtr<VideoFrame> SharedVideoFrameReader::read(SharedVideoFrame&& sharedVideoFrame)
{
    return switchOn(WTF::move(sharedVideoFrame.buffer),
    [&](GStreamerVideoFrameLayout&& layout) -> RefPtr<VideoFrame> {
        return readFromSharedMemory(layout, sharedVideoFrame);
    }, [&](RemoteVideoFrameReadReference&& reference) -> RefPtr<VideoFrame> {
        if (!m_objectHeap) {
            RELEASE_LOG_ERROR(Media, "SharedVideoFrameReader::read no object heap");
            return nullptr;
        }

        RefPtr frame = dynamicDowncast<VideoFrameGStreamer>(m_objectHeap->get(WTF::move(reference)));
        if (!frame) {
            RELEASE_LOG_ERROR(Media, "SharedVideoFrameReader::read no frame");
            return nullptr;
        }

        auto options = createOptions(sharedVideoFrame);
        options.presentationSize = frame->presentationSize();
        return VideoFrameGStreamer::create(GRefPtr { frame->sample() }, options);
    });
}

bool SharedVideoFrameReader::setSharedMemory(SharedMemory::Handle&& handle)
{
    m_storage = SharedMemory::map(WTF::move(handle), SharedMemory::Protection::ReadOnly);
    return !!m_storage;
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(GSTREAMER) && ENABLE(VIDEO)
