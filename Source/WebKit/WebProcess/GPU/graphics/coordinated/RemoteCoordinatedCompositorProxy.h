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
#include "CoordinatedCompositor.h"
#include "GPUProcessConnection.h"
#include "MessageReceiver.h"
#include <atomic>
#include <wtf/CheckedPtr.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/WeakPtr.h>
#include <wtf/WorkQueue.h>

namespace WebCore {
class GraphicsContext;
class IntRect;
class Region;
struct CoordinatedLayerProperties;
}

namespace WebKit {
class CoordinatedSceneState;
class LayerTreeHost;
class WebPage;
struct CoordinatedLayerTreeTransaction;

class RemoteCoordinatedCompositorProxy final : public CoordinatedCompositor, public GPUProcessConnection::Client, public IPC::MessageReceiver, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<RemoteCoordinatedCompositorProxy> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteCoordinatedCompositorProxy);
public:
    static Ref<RemoteCoordinatedCompositorProxy> create(WebPage&, LayerTreeHost&, CoordinatedSceneState&);
    ~RemoteCoordinatedCompositorProxy();

    WTF_ABSTRACT_THREAD_SAFE_REF_COUNTED_AND_CAN_MAKE_WEAK_PTR_IMPL;

    uint64_t surfaceID() const final { return m_surfaceID; }
    int maxTextureSize() const final;

    void backgroundColorDidChange() final;
#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
    void preferredBufferFormatsDidChange() final;
#endif
    void pendingTilesDidChange() final { }

    void setSize(const WebCore::IntSize&, float) final;
    void requestCompositionForRenderingUpdate(Function<void()>&&) final;
    void requestComposition(WebCore::CompositionReason) final;
    WTF::RunLoop* runLoop() final { return nullptr; }

    void invalidate() final;
    void suspend() final;
    void resume() final;

    bool isActive() const final { return m_pendingCommits; }
    std::optional<float> fps() const final { return std::nullopt; }

#if ENABLE(DAMAGE_TRACKING)
    void setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>>, unsigned rectangleThreshold) final;
    void enableFrameDamageNotificationForTesting() final;
#endif

    void fillGLInformation(RenderProcessInfo&&, CompletionHandler<void(RenderProcessInfo&&)>&&) final;
    void releaseMemory(WTF::Critical) final { }

    sk_sp<GrContextThreadSafeProxy> threadSafeGrContext() const final { return nullptr; }

    // Records the drawing of a layer's dirty region in the GPU process, which paints the tiles from it.
    uint64_t recordTiles(const WebCore::IntRect& recordRect, unsigned dirtyTilesCount, bool contentsOpaque, float contentsScale, const Function<void(WebCore::GraphicsContext&)>& paint);

private:
    RemoteCoordinatedCompositorProxy(WebPage&, LayerTreeHost&, CoordinatedSceneState&);

    // GPUProcessConnection::Client
    void gpuProcessConnectionDidClose(GPUProcessConnection&) final;

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    void connectToGPUProcess(WebPage&);
    RefPtr<IPC::Connection> gpuConnection() const;
    void didComposite();
    void didCompositeRenderingUpdate();
    void didUpdateFPS(float);
#if ENABLE(DAMAGE_TRACKING)
    void sendDamageSettings();
    void didRenderFrameForTesting(WebCore::Region&&);
#endif

    WeakPtr<WebPage> m_webPage;
    CheckedPtr<LayerTreeHost> m_layerTreeHost;
    ThreadSafeWeakPtr<GPUProcessConnection> m_gpuProcessConnection;
    const Ref<CoordinatedSceneState> m_sceneState;
    const uint64_t m_surfaceID;
    const Ref<WorkQueue> m_commitQueue;
    mutable Lock m_connectionLock;
    RefPtr<IPC::Connection> m_gpuConnection WTF_GUARDED_BY_LOCK(m_connectionLock);
    Lock m_commitLock;
    bool m_needsFullCommit WTF_GUARDED_BY_LOCK(m_commitLock) { false };
    std::atomic<unsigned> m_pendingCommits { 0 };
    Function<void()> m_didCompositeFunction;
    uint64_t m_lastRecordingIdentifier { 0 };
    bool m_hasPendingRecordings { false };
    unsigned m_suspendedCount { 0 };
    mutable int m_maxTextureSize { 0 };
    bool m_isInvalidated { false };
#if ENABLE(DAMAGE_TRACKING)
    struct DamagePropagationSettings {
        std::optional<OptionSet<DamagePropagationFlags>> flags;
        unsigned rectangleThreshold { 0 };
    };
    std::optional<DamagePropagationSettings> m_damagePropagationSettings;
    bool m_shouldNotifyFrameDamageForTesting { false };
#endif
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
