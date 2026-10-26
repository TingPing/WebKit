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

#include <WebCore/ImageBufferBackend.h>
#include <wtf/TZoneMalloc.h>

namespace WebKit {

// An accelerated image buffer drawn in the GPU process, which is shown by the GPU
// process compositor and whose pixels this process can only read back over IPC.
class ImageBufferRemoteCoordinatedBackend final : public WebCore::ImageBufferBackend {
    WTF_MAKE_TZONE_ALLOCATED(ImageBufferRemoteCoordinatedBackend);
    WTF_MAKE_NONCOPYABLE(ImageBufferRemoteCoordinatedBackend);
public:
    static std::unique_ptr<ImageBufferRemoteCoordinatedBackend> create(const WebCore::ImageBufferParameters&);

    ~ImageBufferRemoteCoordinatedBackend();

    WebCore::RenderingMode renderingMode() const final { return WebCore::RenderingMode::Accelerated; }
    size_t memoryCost() const final { return WebCore::ImageBufferBackend::calculateMemoryCost(size(), bytesPerRow()); }

private:
    explicit ImageBufferRemoteCoordinatedBackend(const WebCore::ImageBufferParameters&);

    WebCore::GraphicsContext& context() final;
    RefPtr<WebCore::NativeImage> copyNativeImage() final;
    RefPtr<WebCore::NativeImage> createNativeImageReference() final;
    void getPixelBuffer(const WebCore::IntRect&, WebCore::PixelBuffer&) final;
    void putPixelBuffer(const WebCore::PixelBufferSourceView&, const WebCore::IntRect&, const WebCore::IntPoint&, WebCore::AlphaPremultiplication) final;
    bool canMapBackingStore() const final { return false; }
    unsigned bytesPerRow() const final;
    String debugDescription() const final;
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
