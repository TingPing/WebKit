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

#if ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER) && USE(GBM)
#include <WebCore/CoordinatedContentsIdentifier.h>
#include <WebCore/DMABufBuffer.h>
#include <WebCore/GLFence.h>
#include <WebCore/GraphicsContextGLCoordinated.h>
#include <WebCore/ProcessIdentifier.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/unix/UnixFileDescriptor.h>

namespace WebKit {

// The latest frame of each layer contents producer in the GPU process, such as a WebGL context,
// keyed by the web process and the identifier the web process gives the layer contents.
class RemoteCoordinatedContentsRegistry {
public:
    static RemoteCoordinatedContentsRegistry& singleton();

    struct DMABufFrame {
        Ref<WebCore::DMABufBuffer> buffer;
        UnixFileDescriptor fence;
    };
    using TextureFrame = WebCore::GraphicsContextGLCoordinated::DisplayTexture;
    using Frame = Variant<DMABufFrame, TextureFrame>;

    void setFrame(WebCore::ProcessIdentifier, WebCore::CoordinatedContentsIdentifier, Frame&&);
    void remove(WebCore::ProcessIdentifier, WebCore::CoordinatedContentsIdentifier);
    void removeAll(WebCore::ProcessIdentifier);

    // The fence is only returned once, so the compositor doesn't wait for it again.
    std::optional<Frame> takeFrame(WebCore::ProcessIdentifier, WebCore::CoordinatedContentsIdentifier);

private:
    using Key = std::pair<WebCore::ProcessIdentifier, WebCore::CoordinatedContentsIdentifier>;

    Lock m_lock;
    HashMap<Key, std::optional<Frame>> m_frames WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER) && USE(GBM)
