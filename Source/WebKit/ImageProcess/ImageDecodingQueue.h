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

#pragma once

#if ENABLE(IMAGE_PROCESS)

#include <WebCore/ImageDecoderIdentifier.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/WorkQueue.h>

namespace WebKit {

// The Image process-wide decoding queue. All decoder work (encoded data
// updates and frame decodes) is queued here and executed on a fixed pool of
// decoding threads, so decoding never runs on, or blocks, the main run loop.
//
// Jobs for the same decoder always run on the same worker in FIFO order: the
// frames of one image have to be decoded in order, and a decoder is a single
// state machine. Jobs for different decoders are spread over the workers and
// run in parallel.
//
// Queued (not yet started) jobs can be cancelled, e.g. when a frame is no
// longer needed or a decoder is destroyed. Running jobs are not interrupted;
// their results are discarded by the Web process when they complete.
class ImageDecodingQueue final : public ThreadSafeRefCounted<ImageDecodingQueue> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ImageDecodingQueue);
public:
    static Ref<ImageDecodingQueue> create();

    // Queues a task that operates on the decoder with the given identifier.
    // The frame index is only used for cancellation matching.
    void enqueueTask(WebCore::ImageDecoderIdentifier, WTF::Function<void()>&&, std::optional<uint64_t> frameIndex = std::nullopt);

    // Drops queued (not yet started) tasks for the decoder. If a frame index
    // is given, only tasks decoding that frame are dropped.
    void cancelTasks(WebCore::ImageDecoderIdentifier, std::optional<uint64_t> frameIndex = std::nullopt);

private:
    struct Job {
        WebCore::ImageDecoderIdentifier identifier;
        std::optional<uint64_t> frameIndex;
        WTF::Function<void()> task;
    };

    struct Worker {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Worker);

        explicit Worker(Ref<WorkQueue>&& workQueue)
            : workQueue(WTF::move(workQueue))
        {
        }

        const Ref<WorkQueue> workQueue;
        Lock lock;
        Deque<Job> jobs WTF_GUARDED_BY_LOCK(lock);
        bool isRunning WTF_GUARDED_BY_LOCK(lock) { false };
    };

    explicit ImageDecodingQueue(unsigned workerCount);

    void drainWorker(unsigned workerIndex);

    Vector<std::unique_ptr<Worker>> m_workers;
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
