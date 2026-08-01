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
#include "DecompressionStreamDecoder.h"

#if USE(BROTLI)

#include "ExceptionOr.h"
#include "SharedBuffer.h"
#include <brotli/decode.h>

namespace WebCore {

// The decompression algorithm is broken up into 2 steps.
// 1. Decompression of Data
// 2. Flush Remaining Data
//
// Unlike zlib, the Brotli decoder reports the reason it stopped directly, so the
// result is enough to tell "this chunk is done" from "the stream ended" and from
// "the input was truncated".
ExceptionOr<Ref<JSC::ArrayBuffer>> DecompressionStreamDecoder::decompressBrotli(std::span<const uint8_t> input)
{
    size_t allocateSize = startingAllocationSize;
    auto storage = SharedBufferBuilder();

    if (!m_compressionStream.initializeIfNecessary(CompressionStream::Algorithm::Brotli, CompressionStream::Operation::Decompression))
        return Exception { ExceptionCode::TypeError, "Initialization Failed."_s };

    auto* state = m_compressionStream.brotliDecoderState();
    auto availableIn = input.size();
    auto* nextIn = input.data();

    bool shouldDecompress = true;
    while (shouldDecompress) {
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

        auto result = BrotliDecoderDecompressStream(state, &availableIn, &nextIn, &availableOut, &nextOut, nullptr);

        switch (result) {
        case BROTLI_DECODER_RESULT_ERROR:
            return Exception { ExceptionCode::TypeError, "Failed to Decode Data."_s };

        case BROTLI_DECODER_RESULT_SUCCESS:
            // The stream ended, so anything still unread is trailing garbage.
            if (availableIn)
                m_didDetectExtraBytes = true;
            shouldDecompress = false;
            break;

        case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
            // More input can only arrive if the caller has not flushed yet.
            if (m_didFinish)
                return Exception { ExceptionCode::TypeError, "Incomplete compressed input."_s };
            shouldDecompress = false;
            break;

        case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
            if (allocateSize < maxAllocationSize)
                allocateSize *= 2;
            break;
        }

        output.shrink(output.size() - availableOut);
        storage.append(output);
    }

    RefPtr decompressedData = storage.takeBufferAsArrayBuffer();
    if (!decompressedData)
        return Exception { ExceptionCode::OutOfMemoryError };

    return decompressedData.releaseNonNull();
}

} // namespace WebCore

#endif // USE(BROTLI)
