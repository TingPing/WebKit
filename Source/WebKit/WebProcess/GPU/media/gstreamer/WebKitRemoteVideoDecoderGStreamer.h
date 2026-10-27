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

#if ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)

#include <WebCore/GStreamerVideoDecoderDescription.h>
#include <gst/gst.h>
#include <optional>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace WebCore {
class VideoFrame;
}

namespace WebKit {

// A GstVideoDecoder in place of each decoder of the GPU process, with its rank and sink caps. It sends
// the frames to decode there, and outputs the decoded frames, which stay there when downstream takes
// memory:WebKitVideoFrame, or are read back otherwise.
void registerRemoteVideoDecoders(const Vector<WebCore::GStreamerVideoDecoderDescription>&);

// Called in the work queue of RemoteGStreamerVideoDecoderManagerProxy.
void remoteVideoDecoderDidDecodeFrame(GstElement*, std::optional<uint32_t> frameNumber, Ref<WebCore::VideoFrame>&&);
void remoteVideoDecoderDidConsumeFrame(GstElement*, uint32_t frameNumber);
void remoteVideoDecoderDidFail(GstElement*, const String& message);

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(VIDEO) && USE(GSTREAMER)
