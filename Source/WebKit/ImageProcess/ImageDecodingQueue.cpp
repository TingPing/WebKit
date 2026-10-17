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

#include "config.h"
#include "ImageDecodingQueue.h"

#if ENABLE(IMAGE_PROCESS)

#include <algorithm>
#include <wtf/NumberOfCores.h>

namespace WebKit {

// The number of decoding threads. Decoding different images in parallel is
// valuable, but each thread adds memory and scheduling overheads; cap the
// pool size.
constexpr unsigned maximumWorkerCount = 4;

Ref<ImageDecodingQueue> ImageDecodingQueue::create()
{
    unsigned workerCount = std::clamp<unsigned>(WTF::numberOfProcessorCores(), 1, maximumWorkerCount);
    return adoptRef(*new ImageDecodingQueue(workerCount));
}

ImageDecodingQueue::ImageDecodingQueue(unsigned workerCount)
{
    ASSERT(workerCount);
    m_workers.reserveInitialCapacity(workerCount);
    for (unsigned i = 0; i < workerCount; ++i)
        m_workers.append(makeUnique<Worker>(WorkQueue::create("org.webkit.ImageProcess.Decoder"_s, WorkQueue::QOS::Default)));
}

void ImageDecodingQueue::enqueueTask(WebCore::ImageDecoderIdentifier identifier, WTF::Function<void()>&& task, std::optional<uint64_t> frameIndex)
{
    // Always route a decoder's jobs to the same worker so that they are
    // processed in FIFO order.
    unsigned workerIndex = identifier.toUInt64() % m_workers.size();
    auto& worker = *m_workers[workerIndex];

    bool shouldDrain = false;
    {
        Locker locker { worker.lock };
        worker.jobs.append({ identifier, frameIndex, WTF::move(task) });
        if (!worker.isRunning) {
            worker.isRunning = true;
            shouldDrain = true;
        }
    }

    if (shouldDrain)
        worker.workQueue->dispatch([protectedThis = Ref { *this }, workerIndex]() mutable { protectedThis->drainWorker(workerIndex); });
}

void ImageDecodingQueue::cancelTasks(WebCore::ImageDecoderIdentifier identifier, std::optional<uint64_t> frameIndex)
{
    for (auto& worker : m_workers) {
        Locker locker { worker->lock };
        worker->jobs.removeAllMatching([&](const Job& job) {
            return job.identifier == identifier && (!frameIndex || job.frameIndex == frameIndex);
        });
    }
}

void ImageDecodingQueue::drainWorker(unsigned workerIndex)
{
    auto& worker = *m_workers[workerIndex];

    while (true) {
        WTF::Function<void()> task;
        {
            Locker locker { worker.lock };
            if (worker.jobs.isEmpty()) {
                worker.isRunning = false;
                return;
            }
            task = worker.jobs.takeFirst().task;
        }

        task();
    }
}

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
