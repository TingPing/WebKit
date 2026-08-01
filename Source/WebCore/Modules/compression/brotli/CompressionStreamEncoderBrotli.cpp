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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CompressionStreamEncoder.h"

#if USE(BROTLI)

#include "ExceptionOr.h"
#include "SharedBuffer.h"
#include <brotli/encode.h>

namespace WebCore {

// The compression algorithm is broken up into 2 steps.
// 1. Compression of Data
// 2. Flush Remaining Data
//
// Brotli buffers aggressively, so a call that consumes all of its input may still
// produce no output at all. Once the input is drained we are done for this chunk,
// unless we are flushing, in which case we keep going until the encoder reports the
// stream is finished.
bool CompressionStreamEncoder::didDeflateFinishBrotli(size_t availableIn) const
{
    auto* state = m_compressionStream.brotliEncoderState();
    if (availableIn || BrotliEncoderHasMoreOutput(state))
        return false;
    return !m_didFinish || BrotliEncoderIsFinished(state);
}

ExceptionOr<Ref<JSC::ArrayBuffer>> CompressionStreamEncoder::compressBrotli(std::span<const uint8_t> input)
{
    size_t allocateSize = std::max(input.size(), startingAllocationSize);
    auto storage = SharedBufferBuilder();

    if (!m_compressionStream.initializeIfNecessary(CompressionStream::Algorithm::Brotli, CompressionStream::Operation::Compression))
        return Exception { ExceptionCode::TypeError, "Initialization Failed."_s };

    auto* state = m_compressionStream.brotliEncoderState();
    auto availableIn = input.size();
    auto* nextIn = input.data();

    bool shouldCompress = true;
    while (shouldCompress) {
        Vector<uint8_t> output;
        if (!output.tryReserveInitialCapacity(allocateSize)) {
            allocateSize /= 4;

            if (allocateSize < startingAllocationSize)
                return Exception { ExceptionCode::OutOfMemoryError };

            continue;
        }

        output.grow(allocateSize);

        auto availableOut = output.size();
        auto* nextOut = output.mutableSpan().data();

        if (!BrotliEncoderCompressStream(state, m_didFinish ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS, &availableIn, &nextIn, &availableOut, &nextOut, nullptr))
            return Exception { ExceptionCode::TypeError, "Failed to Encode Data."_s };

        if (didDeflateFinishBrotli(availableIn))
            shouldCompress = false;
        else {
            if (allocateSize < maxAllocationSize)
                allocateSize *= 2;
        }

        output.shrink(output.size() - availableOut);
        storage.append(output);
    }

    RefPtr compressedData = storage.takeBufferAsArrayBuffer();
    if (!compressedData)
        return Exception { ExceptionCode::OutOfMemoryError };

    return compressedData.releaseNonNull();
}

} // namespace WebCore

#endif // USE(BROTLI)
