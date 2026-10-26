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

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "RemoteGraphicsContext.h"
#include <WebCore/IntRect.h>

namespace WebCore {
class SkiaRecordingResult;
}

namespace WebKit {

class RemoteCoordinatedCompositor;

// Plays back the drawing of a layer's dirty region into an SkPicture, which the compositor replays into its tiles.
class RemoteCoordinatedTileRecorder final : public RemoteGraphicsContext {
public:
    static Ref<RemoteCoordinatedTileRecorder> create(RemoteGraphicsContextIdentifier, RemoteRenderingBackend&, RefPtr<RemoteCoordinatedCompositor>&&, const WebCore::IntRect& recordRect, unsigned dirtyTilesCount, bool contentsOpaque, float contentsScale);
    ~RemoteCoordinatedTileRecorder();

    void stopListeningForIPC();

    RemoteCoordinatedCompositor* compositor() const { return m_compositor.get(); }
    Ref<WebCore::SkiaRecordingResult> finishRecording();

private:
    struct Recording;
    RemoteCoordinatedTileRecorder(std::unique_ptr<Recording>&&, RemoteGraphicsContextIdentifier, RemoteRenderingBackend&, RefPtr<RemoteCoordinatedCompositor>&&, const WebCore::IntRect& recordRect, WebCore::RenderingMode, bool contentsOpaque, float contentsScale);

    void startListeningForIPC();

    std::unique_ptr<Recording> m_recording;
    const RemoteGraphicsContextIdentifier m_identifier;
    const RefPtr<RemoteCoordinatedCompositor> m_compositor;
    const WebCore::IntRect m_recordRect;
    const WebCore::RenderingMode m_renderingMode;
    const bool m_contentsOpaque;
    const float m_contentsScale;
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
