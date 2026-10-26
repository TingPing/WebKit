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

#if ENABLE(GPU_PROCESS)

#include <WebCore/IntSize.h>
#include <wtf/Vector.h>

#if ENABLE(VIDEO) && USE(GSTREAMER)
#include <WebCore/VideoFrameGStreamer.h>
#include <span>
#include <wtf/Function.h>

typedef struct _GstBuffer GstBuffer;
#endif

namespace WebKit {

// A video frame in system memory whose planes were copied one after another into shared memory.
// The format is a GstVideoFormat, the offsets and strides are those of each plane in the memory,
// and the color fields are those of its GstVideoColorimetry.
struct GStreamerVideoFrameLayout {
    uint32_t format { 0 };
    WebCore::IntSize size;
    WebCore::IntSize presentationSize;
    Vector<uint64_t> offsets;
    Vector<int32_t> strides;
    uint8_t colorRange { 0 };
    uint8_t colorMatrix { 0 };
    uint8_t colorTransfer { 0 };
    uint8_t colorPrimaries { 0 };

#if ENABLE(VIDEO) && USE(GSTREAMER)
    // Copies the planes into the memory returned by allocate() for their total size.
    static std::optional<GStreamerVideoFrameLayout> write(const WebCore::VideoFrameGStreamer&, NOESCAPE const Function<std::span<uint8_t>(size_t)>& allocate);

    // Returns null when the layout does not fit in the buffer.
    RefPtr<WebCore::VideoFrameGStreamer> createVideoFrame(GRefPtr<GstBuffer>&&, WebCore::VideoFrameGStreamer::CreateOptions&& = { }) const;
#endif
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS)
