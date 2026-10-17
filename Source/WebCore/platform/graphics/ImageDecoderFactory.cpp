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
#include "ImageDecoderFactory.h"

#if ENABLE(IMAGE_PROCESS)

#include <wtf/NeverDestroyed.h>

namespace WebCore {

ImageDecoderFactory& ImageDecoderFactory::singleton()
{
    static NeverDestroyed<ImageDecoderFactory> factory;
    return factory.get();
}

ImageDecoderFactory::ImageDecoderFactory() = default;

void ImageDecoderFactory::set(DecoderInterface&& decoderInterface)
{
    Locker locker { m_lock };
    m_decoderInterface = WTF::move(decoderInterface);
}

void ImageDecoderFactory::reset()
{
    Locker locker { m_lock };
    m_decoderInterface = { };
}

bool ImageDecoderFactory::supportsMediaType(ImageDecoder::MediaType type) const
{
    Locker locker { m_lock };
    return m_decoderInterface.supportsMediaType && m_decoderInterface.supportsMediaType(type);
}

RefPtr<ImageDecoder> ImageDecoderFactory::createImageDecoder(FragmentedSharedBuffer& data, const String& mimeType, AlphaOption alphaOption, GammaAndColorProfileOption gammaAndColorProfileOption) const
{
    Locker locker { m_lock };
    if (!m_decoderInterface.createImageDecoder)
        return nullptr;
    return m_decoderInterface.createImageDecoder(data, mimeType, alphaOption, gammaAndColorProfileOption);
}

} // namespace WebCore

#endif // ENABLE(IMAGE_PROCESS)
