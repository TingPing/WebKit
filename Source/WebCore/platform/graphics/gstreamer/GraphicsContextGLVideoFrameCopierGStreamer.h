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

#if ENABLE(WEBGL) && ENABLE(VIDEO) && USE(GSTREAMER) && USE(COORDINATED_GRAPHICS)

#include "GraphicsTypesGL.h"
#include <array>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class VideoFrameGStreamer;

// Converts YUV frames in system memory to RGB into WebGL textures, with a context that shares
// the textures of the WebGL one but not its state.
class GraphicsContextGLVideoFrameCopierGStreamer {
    WTF_MAKE_TZONE_ALLOCATED(GraphicsContextGLVideoFrameCopierGStreamer);
    WTF_MAKE_NONCOPYABLE(GraphicsContextGLVideoFrameCopierGStreamer);
public:
    static std::unique_ptr<GraphicsContextGLVideoFrameCopierGStreamer> create(GCGLDisplay, GCGLConfig, GCGLContext ownerContext, GCEGLSurface ownerSurface, bool isForWebGL2);
    ~GraphicsContextGLVideoFrameCopierGStreamer();

    // The owner context must be current, and is current again when this returns.
    bool copy(const VideoFrameGStreamer&, GCGLuint outputTexture, GCGLint level, GCGLenum internalFormat, GCGLenum format, GCGLenum type, bool premultiplyAlpha, bool flipY);

private:
    GraphicsContextGLVideoFrameCopierGStreamer(GCGLDisplay, GCGLContext ownerContext, GCEGLSurface ownerSurface);

    bool initialize(GCGLConfig, bool isForWebGL2);
    bool makeContextCurrent();
    void restoreOwnerContext();

    GCGLDisplay m_display { nullptr };
    GCGLContext m_ownerContext { nullptr };
    GCEGLSurface m_ownerSurface { nullptr };
    GCGLContext m_context { nullptr };
    GCGLuint m_program { 0 };
    GCGLuint m_vertexBuffer { 0 };
    GCGLuint m_framebuffer { 0 };
    std::array<GCGLuint, 4> m_planeTextures { };
    GCGLint m_planeCountLocation { -1 };
    GCGLint m_swapUVLocation { -1 };
    GCGLint m_flipYLocation { -1 };
    GCGLint m_premultiplyAlphaLocation { -1 };
    GCGLint m_colorMatrixLocation { -1 };
    bool m_canUnpackRowLength { false };
};

} // namespace WebCore

#endif // ENABLE(WEBGL) && ENABLE(VIDEO) && USE(GSTREAMER) && USE(COORDINATED_GRAPHICS)
