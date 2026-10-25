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

#if USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "CoordinatedPlatformLayerBuffer.h"

namespace WebCore {

// Contents produced in the process that composites the layer, referenced by the identifier of their producer there.
class CoordinatedPlatformLayerBufferRemote final : public CoordinatedPlatformLayerBuffer {
public:
    static std::unique_ptr<CoordinatedPlatformLayerBufferRemote> create(uint64_t identifier, AlphaMode alphaMode, Origin origin)
    {
        return std::unique_ptr<CoordinatedPlatformLayerBufferRemote>(new CoordinatedPlatformLayerBufferRemote(identifier, alphaMode, origin));
    }

    uint64_t identifier() const { return m_identifier; }
    AlphaMode alphaMode() const { return m_alphaMode; }
    Origin origin() const { return m_origin; }

private:
    CoordinatedPlatformLayerBufferRemote(uint64_t identifier, AlphaMode alphaMode, Origin origin)
        : CoordinatedPlatformLayerBuffer(Type::Remote, { }, alphaMode)
        , m_identifier(identifier)
        , m_origin(origin)
    {
    }

    uint64_t m_identifier { 0 };
    Origin m_origin { Origin::TopLeft };
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_COORDINATED_PLATFORM_LAYER_BUFFER_TYPE(CoordinatedPlatformLayerBufferRemote, Type::Remote)

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
