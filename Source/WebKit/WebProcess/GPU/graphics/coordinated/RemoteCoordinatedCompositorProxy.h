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
#include "RendererBufferFormat.h"
#include <WebCore/DMABufBufferAttributes.h>
#include <WebCore/IntRect.h>
#include <WebCore/ShareableBitmapHandle.h>
#include <atomic>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/WeakPtr.h>
#include <wtf/WorkQueue.h>
#include <wtf/unix/UnixFileDescriptor.h>

namespace WebCore {
struct CoordinatedLayerProperties;
}

namespace WebKit {
class CoordinatedSceneState;
class WebPage;
struct CoordinatedLayerTreeTransaction;

class RemoteCoordinatedCompositorProxy final : public CoordinatedCompositor, public GPUProcessConnection::Client, public IPC::MessageReceiver, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<RemoteCoordinatedCompositorProxy> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteCoordinatedCompositorProxy);
public:
    static Ref<RemoteCoordinatedCompositorProxy> create(WebPage&, CoordinatedSceneState&);
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
    void setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>>, unsigned) final { }
    void enableFrameDamageNotificationForTesting() final { }
#endif

    void fillGLInformation(RenderProcessInfo&&, CompletionHandler<void(RenderProcessInfo&&)>&&) final;
    void releaseMemory(WTF::Critical) final { }

    sk_sp<GrContextThreadSafeProxy> threadSafeGrContext() const final { return nullptr; }

private:
    RemoteCoordinatedCompositorProxy(WebPage&, CoordinatedSceneState&);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    // GPUProcessConnection::Client
    void gpuProcessConnectionDidClose(GPUProcessConnection&) final;

    RefPtr<IPC::Connection> gpuConnection() const;
    void didComposite();

    void didCreateDMABufBuffer(uint64_t, WebCore::DMABufBufferAttributes&&, RendererBufferFormat::Usage);
    void didCreateSHMBuffer(uint64_t, WebCore::ShareableBitmapHandle&&);
    void didDestroyBuffer(uint64_t);
    void frame(uint64_t, Vector<WebCore::IntRect, 1>&&, UnixFileDescriptor&&);
    void didChangeBufferConfiguration(uint32_t);
    void releaseBuffer(uint64_t, UnixFileDescriptor&&);
    void frameDone();

    WeakPtr<WebPage> m_webPage;
    const Ref<CoordinatedSceneState> m_sceneState;
    const uint64_t m_surfaceID;
    const Ref<WorkQueue> m_commitQueue;
    const Ref<WorkQueue> m_relayQueue;
    mutable Lock m_connectionLock;
    RefPtr<IPC::Connection> m_gpuConnection WTF_GUARDED_BY_LOCK(m_connectionLock);
    Lock m_commitLock;
    std::atomic<unsigned> m_pendingCommits { 0 };
    Function<void()> m_didCompositeFunction;
    unsigned m_suspendedCount { 0 };
    bool m_isInvalidated { false };
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
