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
#include "CoordinatedVideoFrameSlotIdentifier.h"
#include "MessageReceiver.h"
#include "RendererBufferFormat.h"
#include "WebPageProxyIdentifier.h"
#include <WebCore/Color.h>
#include <WebCore/CoordinatedBackingStoreProxy.h>
#include <WebCore/CoordinatedTileRecordingIdentifier.h>
#include <WebCore/FloatRect.h>
#include <WebCore/IntRect.h>
#include <WebCore/IntSize.h>
#include <WebCore/PlatformLayerIdentifier.h>
#include <WebCore/ProcessIdentifier.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/CompletionHandler.h>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WeakPtr.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/CString.h>

class SkCanvas;

namespace WebCore {
class CoordinatedPlatformLayerBuffer;
class CoordinatedTileBuffer;
class CoordinatedTileManager;
class SharedMemory;
class SkiaCompositingLayer;
class VideoFrameGStreamer;
class SkiaRecordingResult;
struct CoordinatedLayerProperties;
}

namespace WebKit {
class AcceleratedSurface;
class GPUConnectionToWebProcess;
class RemoteVideoFrameObjectHeap;
struct CoordinatedLayerTreeTransaction;
struct CoordinatedLayerUpdate;
struct CoordinatedSharedVideoFrame;
struct CoordinatedVideoFrame;

class RemoteCoordinatedCompositor final : public IPC::MessageReceiver, public ThreadSafeRefCounted<RemoteCoordinatedCompositor, WTF::DestructionThread::Main> {
    WTF_MAKE_TZONE_ALLOCATED(RemoteCoordinatedCompositor);
public:
    struct Parameters {
        WebPageProxyIdentifier pageIdentifier;
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

    // Called from the rendering backend work queues.
    const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContextForRecording() const { return m_threadSafeGrContextForRecording; }
    void didRecordTiles(WebCore::CoordinatedTileRecordingIdentifier, Ref<WebCore::SkiaRecordingResult>&&);

private:
    RemoteCoordinatedCompositor(GPUConnectionToWebProcess&, uint64_t identifier, Parameters&&);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    void commit(CoordinatedLayerTreeTransaction&&, CompletionHandler<void()>&&);
    void updateLayers(CoordinatedLayerTreeTransaction&&);
    void setSize(WebCore::IntSize, float deviceScaleFactor);
    void setVisible(bool);
    void backgroundColorDidChange(std::optional<WebCore::Color>&&);
#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
    void preferredBufferFormatsDidChange(Vector<RendererBufferFormat>&&);
#endif
#if ENABLE(DAMAGE_TRACKING)
    void setDamagePropagationSettings(bool propagationEnabled, bool unified, bool usedForCompositing, uint32_t rectangleThreshold);
    void enableFrameDamageNotificationForTesting();
#endif

    using TileUpdates = Vector<WebCore::CoordinatedBackingStoreProxy::Update::TileUpdate>;

    WebCore::SkiaCompositingLayer& ensureLayer(WebCore::PlatformLayerIdentifier);
    void applyLayerProperties(WebCore::SkiaCompositingLayer&, const WebCore::CoordinatedLayerProperties&);
    TileUpdates scheduleTileReplays(const CoordinatedLayerUpdate&, Vector<Ref<WebCore::CoordinatedTileBuffer>>& paintingBuffers);
    void applyLayerUpdate(CoordinatedLayerUpdate&&, std::span<const uint8_t> pixels, TileUpdates&&);
#if ENABLE(VIDEO) && USE(GSTREAMER)
    RefPtr<WebCore::SharedMemory> videoFrameSlot(CoordinatedSharedVideoFrame&);
    RefPtr<WebCore::VideoFrameGStreamer> sharedVideoFrame(CoordinatedSharedVideoFrame&&);
    std::unique_ptr<WebCore::CoordinatedPlatformLayerBuffer> videoFrameBuffer(CoordinatedVideoFrame&&);
#endif
    void runWhenTilesArePainted(Function<void()>&&);
    void commitWhenTilesArePainted(CoordinatedLayerTreeTransaction&&, CompletionHandler<void()>&&);
    void applyCommit(CoordinatedLayerTreeTransaction&&, Vector<TileUpdates>&&, CompletionHandler<void()>&&);
    void scheduleRender();
    void render();
    void updateFPSCounter();
    void updateFPSCounterGeometry();
#if ENABLE(DAMAGE_TRACKING)
    WebCore::IntRect takeFPSCounterDamage();
#endif
    void drawFPSCounter(SkCanvas&);
    void frameComplete();
    void didComposite();

    WeakPtr<GPUConnectionToWebProcess> m_connectionToWebProcess;
    const Ref<IPC::Connection> m_webProcessConnection;
    const WebCore::ProcessIdentifier m_webProcessIdentifier;
    const uint64_t m_identifier;
    const Ref<WorkQueue> m_workQueue;
    const Ref<WorkQueue> m_tilePaintingQueue;
    RefPtr<AcceleratedSurface> m_surface;
    sk_sp<GrContextThreadSafeProxy> m_threadSafeGrContextForRecording;

    // Only accessed in the compositor work queue.
    sk_sp<GrContextThreadSafeProxy> m_threadSafeGrContext;
    std::unique_ptr<WebCore::CoordinatedTileManager> m_tileManager;
    HashMap<WebCore::CoordinatedTileRecordingIdentifier, Ref<WebCore::SkiaRecordingResult>> m_recordings;
    bool m_isWaitingForTiles { false };
    Deque<Function<void()>> m_tasksWaitingForTiles;
    HashMap<WebCore::PlatformLayerIdentifier, Ref<WebCore::SkiaCompositingLayer>> m_layers;
    RefPtr<WebCore::SkiaCompositingLayer> m_rootLayer;
#if ENABLE(VIDEO) && USE(GSTREAMER)
    const Ref<RemoteVideoFrameObjectHeap> m_videoFrameObjectHeap;
    HashMap<CoordinatedVideoFrameSlotIdentifier, Ref<WebCore::SharedMemory>> m_videoFrameSlots;
#endif
    WebCore::IntSize m_viewportSize;
    float m_deviceScaleFactor { 1 };
    bool m_isVisible { false };
    bool m_isInvalidated { false };
    bool m_isWaitingForFrameDone { false };
    bool m_needsRender { false };
    Vector<CompletionHandler<void()>> m_pendingCommitCompletionHandlers;
    struct {
        bool exposesFPS { false };
        bool drawsFPS { false };
        Seconds calculationInterval { 1_s };
        MonotonicTime lastCalculationTimestamp;
        unsigned frameCount { 0 };
        int lastFPS { 0 };
        int displayedFPS { -1 };
        ASCIICString text;
        WebCore::FloatRect rect;
        float textBaseline { 0 };
        WebCore::IntRect lastDrawnRect;
    } m_fpsCounter;
#if ENABLE(DAMAGE_TRACKING)
    struct {
        bool propagationEnabled { false };
        bool unified { false };
        bool usedForCompositing { false };
        bool showsOverlay { false };
        bool shouldNotifyFrameDamageForTesting { false };
    } m_damage;
#endif
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
