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

#if USE(COORDINATED_GRAPHICS)
#include <WebCore/CoordinatedCompositionReason.h>
#include <optional>
#include <wtf/AbstractRefCounted.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>
#include <wtf/OptionSet.h>

#if !USE(TEXTURE_MAPPER)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebCore {
class IntSize;
}

namespace WTF {
class RunLoop;
enum class Critical : bool;
}

namespace WebKit {
struct RenderProcessInfo;

class CoordinatedCompositor : public AbstractRefCounted {
public:
    virtual ~CoordinatedCompositor() = default;

    virtual uint64_t surfaceID() const = 0;
    virtual int maxTextureSize() const = 0;

    virtual void backgroundColorDidChange() = 0;
#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
    virtual void preferredBufferFormatsDidChange() = 0;
#endif
    virtual void pendingTilesDidChange() = 0;

    virtual void setSize(const WebCore::IntSize&, float) = 0;
    virtual void requestCompositionForRenderingUpdate(Function<void()>&&) = 0;
    virtual void requestComposition(WebCore::CompositionReason) = 0;
    virtual WTF::RunLoop* runLoop() = 0;

    virtual void invalidate() = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;

    virtual bool isActive() const = 0;
    virtual std::optional<float> fps() const = 0;

#if ENABLE(DAMAGE_TRACKING)
    enum class DamagePropagationFlags : uint8_t {
        Unified = 1 << 0,
        UseForCompositing = 1 << 1
    };
    virtual void setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>>, unsigned rectangleThreshold) = 0;
    virtual void enableFrameDamageNotificationForTesting() = 0;
#endif

    virtual void fillGLInformation(RenderProcessInfo&&, CompletionHandler<void(RenderProcessInfo&&)>&&) = 0;
    virtual void releaseMemory(WTF::Critical) = 0;

#if !USE(TEXTURE_MAPPER)
    virtual sk_sp<GrContextThreadSafeProxy> threadSafeGrContext() const = 0;
#endif
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
