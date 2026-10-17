/*
 * Copyright (C) 2024-2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImageFrameWorkQueue.h"

#include "BitmapImageSource.h"
#include "ImageDecoder.h"
#include "Logging.h"
#include <wtf/SystemTracing.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

Ref<ImageFrameWorkQueue> ImageFrameWorkQueue::create(BitmapImageSource& source)
{
    return adoptRef(*new ImageFrameWorkQueue(source));
}

std::atomic<unsigned> ImageFrameWorkQueue::s_pendingDecodeCount { 0 };

ImageFrameWorkQueue::ImageFrameWorkQueue(BitmapImageSource& source)
    : m_source(source)
{
}

ImageFrameWorkQueue::~ImageFrameWorkQueue()
{
    decrementPendingAsynchronousDecodeCount(m_pendingAsynchronousDecodeCount);
}

void ImageFrameWorkQueue::decrementPendingAsynchronousDecodeCount(unsigned count)
{
    count = std::min(count, m_pendingAsynchronousDecodeCount);
    m_pendingAsynchronousDecodeCount -= count;
    s_pendingDecodeCount -= count;
}

ImageFrameWorkQueue::RequestQueue& ImageFrameWorkQueue::requestQueue()
{
    if (!m_requestQueue)
        m_requestQueue = RequestQueue::create();

    return *m_requestQueue;
}

void ImageFrameWorkQueue::start()
{
    if (m_workQueue)
        return;

    RefPtr decoder = m_source.get()->decoder();
    if (!decoder)
        return;

    m_workQueue = WorkQueue::create("org.webkit.ImageDecoder"_s, WorkQueue::QOS::Default);

    protect(m_workQueue)->dispatch([protectedThis = Ref { *this }, weakRunLoop = ThreadSafeWeakPtr { RunLoop::currentSingleton() }, protectedWorkQueue = Ref { *m_workQueue }, protectedSource = m_source.get(), protectedDecoder = Ref { *decoder }, protectedRequestQueue = Ref { requestQueue() }] () mutable {
        Request request;
        while (protectedRequestQueue->dequeue(request)) {
            TraceScope tracingScope(AsyncImageDecodeStart, AsyncImageDecodeEnd);

            auto minimumDecodingDuration = protectedThis->minimumDecodingDurationForTesting();

            MonotonicTime startingTime;
            if (minimumDecodingDuration > 0_s)
                startingTime = MonotonicTime::now();

            RefPtr<NativeImage> nativeImage;
            DecodingDestination decodingDestination = request.options.decodingDestination();

            if (auto result = protectedDecoder->createNativeImageAtIndex(request.index, request.subsamplingLevel, request.options)) {
                nativeImage = WTF::move(std::get<Ref<NativeImage>>(*result));
                decodingDestination = std::get<DecodingDestination>(*result);
            }

            request.options = { request.options.decodingMode(), decodingDestination, request.options.sizeForDrawing() };

            // Pretend as if decoding the frame took minimumDecodingDuration.
            if (minimumDecodingDuration > 0_s) {
                auto actualDecodingDuration = MonotonicTime::now() - startingTime;
                if (minimumDecodingDuration > actualDecodingDuration)
                    sleep(minimumDecodingDuration - actualDecodingDuration);
            }

            if (RefPtr protectedRunLoop = weakRunLoop.get()) {
                // Even if we fail to decode the frame, it is important to sync the creation thread with this result.
                callOnRunLoop(*protectedRunLoop, [protectedThis, protectedWorkQueue, protectedSource, request, nativeImage = WTF::move(nativeImage)] () mutable {
                    // The WorkQueue may have been recreated before the frame was decoded.
                    if (protectedWorkQueue.ptr() != protectedThis->m_workQueue || protectedSource.ptr() != protectedThis->m_source.get().ptr()) {
                        LOG(Images, "AsyncImageDecoder::%s - %p. WorkQueue was recreated at index = %d.", __FUNCTION__, protectedThis.ptr(), request.index);
                        return;
                    }

                    protectedThis->didFinishDecoding(protectedSource, request, WTF::move(nativeImage));
                });
            }
        }

        if (RefPtr protectedRunLoop = weakRunLoop.get()) {
            // Ensure destruction happens on creation thread.
            callOnRunLoop(*protectedRunLoop, [protectedThis = WTF::move(protectedThis), weakRunLoop = WTF::move(weakRunLoop), protectedWorkQueue = WTF::move(protectedWorkQueue), protectedSource = WTF::move(protectedSource)] () mutable { });
        }
    });
}

bool ImageFrameWorkQueue::didFinishDecoding(BitmapImageSource& source, const Request& request, RefPtr<NativeImage>&& nativeImage)
{
    // The DecodeQueue may have been cleared before the frame was decoded.
    if (decodeQueue().isEmpty() || !request.isCompatibleWith(decodeQueue().first())) {
        LOG(Images, "AsyncImageDecoder::%s - %p. DecodeQueue was cleared at index = %d.", __FUNCTION__, this, request.index);
        return false;
    }

    decodeQueue().removeFirst();
    source.imageFrameDecodeAtIndexHasFinished(request.index, request.subsamplingLevel, request.animatingState, request.options, WTF::move(nativeImage));
    return true;
}

void ImageFrameWorkQueue::dispatch(const Request& request)
{
    Ref source = m_source.get();
    if (source->decodesAsynchronously()) {
        dispatchToAsynchronousDecoder(*source->decoderIfExists(), request);
        return;
    }

    protect(requestQueue())->enqueue(request);
    decodeQueue().append(request);

    start();
}

void ImageFrameWorkQueue::dispatchToAsynchronousDecoder(ImageDecoder& decoder, const Request& request)
{
    // The decoder decodes elsewhere and calls back on this run loop, so no decoding thread is needed.
    decodeQueue().append(request);

    // These are the decodes that would otherwise be missing from test snapshots.
    ++m_pendingAsynchronousDecodeCount;
    ++s_pendingDecodeCount;

    decoder.decodeFrameAsynchronously(request.index, request.subsamplingLevel, request.options, [protectedThis = Ref { *this }, weakSource = ThreadSafeWeakPtr<BitmapImageSource> { m_source.get() }, request](RefPtr<NativeImage>&& nativeImage) mutable {
        RefPtr source = weakSource.get();
        if (!source || source.get() != protectedThis->m_source.get().ptr())
            return;

        if (protectedThis->didFinishDecoding(*source, request, WTF::move(nativeImage)))
            protectedThis->decrementPendingAsynchronousDecodeCount(1);
    });
}

void ImageFrameWorkQueue::stop()
{
    Ref source = m_source.get();

    for (auto& request : m_decodeQueue) {
        LOG(Images, "AsyncImageDecoder::%s - %p. Decoding was cancelled for frame at index = %d.", __FUNCTION__, this, request.index);
        source->destroyNativeImageAtIndex(request.index);
    }

    if (m_requestQueue) {
        protect(m_requestQueue)->close();
        m_requestQueue = nullptr;
    }

    decrementPendingAsynchronousDecodeCount(m_pendingAsynchronousDecodeCount);
    m_decodeQueue.clear();
    m_workQueue = nullptr;
}

bool ImageFrameWorkQueue::isPendingDecodingAtIndex(unsigned index, SubsamplingLevel subsamplingLevel, const DecodingOptions& options) const
{
    auto it = std::find_if(m_decodeQueue.begin(), m_decodeQueue.end(), [index, subsamplingLevel, &options](const Request& request) {
        return request.index == index && subsamplingLevel >= request.subsamplingLevel && request.options.isCompatibleWith(options);
    });
    return it != m_decodeQueue.end();
}

void ImageFrameWorkQueue::dump(TextStream& ts) const
{
    if (isIdle())
        return;

    ts.dumpProperty("pending-for-decoding"_s, m_decodeQueue.size());
}

} // namespace WebCore
