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
#include "ImageBufferRemoteCoordinatedBackend.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)

#include <WebCore/GraphicsContext.h>
#include <WebCore/PixelBuffer.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBufferRemoteCoordinatedBackend);

std::unique_ptr<ImageBufferRemoteCoordinatedBackend> ImageBufferRemoteCoordinatedBackend::create(const ImageBufferParameters& parameters)
{
    return std::unique_ptr<ImageBufferRemoteCoordinatedBackend> { new ImageBufferRemoteCoordinatedBackend { parameters } };
}

ImageBufferRemoteCoordinatedBackend::ImageBufferRemoteCoordinatedBackend(const ImageBufferParameters& parameters)
    : ImageBufferBackend(parameters)
{
}

ImageBufferRemoteCoordinatedBackend::~ImageBufferRemoteCoordinatedBackend() = default;

GraphicsContext& ImageBufferRemoteCoordinatedBackend::context()
{
    RELEASE_ASSERT_NOT_REACHED();
}

RefPtr<NativeImage> ImageBufferRemoteCoordinatedBackend::copyNativeImage()
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<NativeImage> ImageBufferRemoteCoordinatedBackend::createNativeImageReference()
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

void ImageBufferRemoteCoordinatedBackend::getPixelBuffer(const IntRect&, PixelBuffer&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

void ImageBufferRemoteCoordinatedBackend::putPixelBuffer(const PixelBufferSourceView&, const IntRect&, const IntPoint&, AlphaPremultiplication)
{
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned ImageBufferRemoteCoordinatedBackend::bytesPerRow() const
{
    return size().width() * 4;
}

String ImageBufferRemoteCoordinatedBackend::debugDescription() const
{
    TextStream stream;
    stream << "ImageBufferRemoteCoordinatedBackend " << this;
    return stream.release();
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
