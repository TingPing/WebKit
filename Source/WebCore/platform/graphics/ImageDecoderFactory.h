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

#if ENABLE(IMAGE_PROCESS)

#include <WebCore/ImageDecoder.h>
#include <wtf/Forward.h>
#include <wtf/Lock.h>

namespace WebCore {

// Generic registry that lets the embedding process provide ImageDecoders (e.g. decoders
// that forward decoding to another process). Mirrors ImageDecoderFactoryAVF, which is
// specific to the AVFoundation decoders on Cocoa ports.
class ImageDecoderFactory {
public:
    struct DecoderInterface {
        using SupportsMediaTypeFunc = Function<bool(ImageDecoder::MediaType)>;
        using CreateImageDecoderFunc = Function<RefPtr<ImageDecoder>(FragmentedSharedBuffer&, const String& mimeType, AlphaOption, GammaAndColorProfileOption)>;

        SupportsMediaTypeFunc supportsMediaType;
        CreateImageDecoderFunc createImageDecoder;
    };

    WEBCORE_EXPORT static ImageDecoderFactory& singleton();

    WEBCORE_EXPORT void set(DecoderInterface&&);
    WEBCORE_EXPORT void reset();

    bool supportsMediaType(ImageDecoder::MediaType) const;
    RefPtr<ImageDecoder> createImageDecoder(FragmentedSharedBuffer&, const String& mimeType, AlphaOption, GammaAndColorProfileOption) const;

private:
    friend NeverDestroyed<ImageDecoderFactory>;

    ImageDecoderFactory();

    mutable Lock m_lock;
    DecoderInterface m_decoderInterface WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebCore

#endif // ENABLE(IMAGE_PROCESS)
