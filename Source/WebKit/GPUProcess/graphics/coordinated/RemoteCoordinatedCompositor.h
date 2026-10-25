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
#include "MessageReceiver.h"
#include "RendererBufferFormat.h"
#include <WebCore/Color.h>
#include <WebCore/IntSize.h>
#include <WebCore/PlatformLayerIdentifier.h>
#include <WebCore/ProcessIdentifier.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/CompletionHandler.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WeakPtr.h>
#include <wtf/WorkQueue.h>

namespace WebCore {
class SkiaCompositingLayer;
struct CoordinatedLayerProperties;
}

namespace WebKit {
class AcceleratedSurface;
class GPUConnectionToWebProcess;
struct CoordinatedLayerTreeTransaction;
struct CoordinatedLayerUpdate;

class RemoteCoordinatedCompositor final : public IPC::MessageReceiver, public ThreadSafeRefCounted<RemoteCoordinatedCompositor, WTF::DestructionThread::Main> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteCoordinatedCompositor);
public:
    struct Parameters {
        WebCore::IntSize viewportSize;
        float deviceScaleFactor { 1 };
        std::optional<WebCore::Color> backgroundColor;
        bool isVisible { false };
        Vector<RendererBufferFormat> preferredBufferFormats;
        bool usesHardwareBufferTransport { true };
    };
    static Ref<RemoteCoordinatedCompositor> create(GPUConnectionToWebProcess&, uint64_t identifier, Parameters&&);
    ~RemoteCoordinatedCompositor();

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    void invalidate();

private:
    RemoteCoordinatedCompositor(GPUConnectionToWebProcess&, uint64_t identifier, Parameters&&);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    void commit(CoordinatedLayerTreeTransaction&&, CompletionHandler<void()>&&);
    void updatePositions(Vector<WebCore::CoordinatedLayerProperties>&&);
    void setSize(WebCore::IntSize, float deviceScaleFactor);
    void setVisible(bool);
    void backgroundColorDidChange(std::optional<WebCore::Color>&&);
    void preferredBufferFormatsDidChange(Vector<RendererBufferFormat>&&);

    WebCore::SkiaCompositingLayer& ensureLayer(WebCore::PlatformLayerIdentifier);
    void applyLayerProperties(WebCore::SkiaCompositingLayer&, const WebCore::CoordinatedLayerProperties&);
    void applyLayerUpdate(CoordinatedLayerUpdate&&, std::span<const uint8_t> pixels);
    void scheduleRender();
    void render();
    void frameComplete();
    void didComposite();

    WeakPtr<GPUConnectionToWebProcess> m_connectionToWebProcess;
    const WebCore::ProcessIdentifier m_webProcessIdentifier;
    const uint64_t m_identifier;
    const Ref<WorkQueue> m_workQueue;
    RefPtr<AcceleratedSurface> m_surface;

    // Only accessed in the compositor work queue.
    sk_sp<GrContextThreadSafeProxy> m_threadSafeGrContext;
    HashMap<WebCore::PlatformLayerIdentifier, Ref<WebCore::SkiaCompositingLayer>> m_layers;
    RefPtr<WebCore::SkiaCompositingLayer> m_rootLayer;
    WebCore::IntSize m_viewportSize;
    float m_deviceScaleFactor { 1 };
    bool m_isVisible { false };
    bool m_isInvalidated { false };
    bool m_isWaitingForFrameDone { false };
    bool m_needsRender { false };
    Vector<CompletionHandler<void()>> m_pendingCommitCompletionHandlers;
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
