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
#include "RemoteCoordinatedCompositor.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "AcceleratedSurface.h"
#include "CoordinatedLayerTreeTransaction.h"
#include "GPUConnectionToWebProcess.h"
#include "GPUProcess.h"
#include "GPUProcessProxyMessages.h"
#include "RemoteCoordinatedCompositorMessages.h"
#include "RemoteCoordinatedCompositorProxyMessages.h"
#include "RemoteCoordinatedContentsRegistry.h"
#include <WebCore/CoordinatedBackingStoreProxy.h>
#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/CoordinatedPlatformLayerBufferDMABuf.h>
#include <WebCore/CoordinatedPlatformLayerBufferSkiaImage.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/CoordinatedTileManager.h>
#include <WebCore/Damage.h>
#include <WebCore/GLContext.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/SkiaCompositingLayer.h>
#include <WebCore/SkiaRecordingResult.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteCoordinatedCompositor);

Ref<RemoteCoordinatedCompositor> RemoteCoordinatedCompositor::create(GPUConnectionToWebProcess& connectionToWebProcess, uint64_t identifier, Parameters&& parameters)
{
    return adoptRef(*new RemoteCoordinatedCompositor(connectionToWebProcess, identifier, WTF::move(parameters)));
}

RemoteCoordinatedCompositor::RemoteCoordinatedCompositor(GPUConnectionToWebProcess& connectionToWebProcess, uint64_t identifier, Parameters&& parameters)
    : m_connectionToWebProcess(connectionToWebProcess)
    , m_webProcessConnection(connectionToWebProcess.connection())
    , m_webProcessIdentifier(connectionToWebProcess.webProcessIdentifier())
    , m_identifier(identifier)
    , m_workQueue(WorkQueue::create("org.webkit.RemoteCoordinatedCompositor"_s))
    , m_viewportSize(parameters.viewportSize)
    , m_deviceScaleFactor(parameters.deviceScaleFactor)
    , m_isVisible(parameters.isVisible)
{
    ASSERT(RunLoop::isMain());

    AcceleratedSurface::Parameters surfaceParameters {
        .backgroundColor = WTF::move(parameters.backgroundColor),
        .isVisible = parameters.isVisible,
        .preferredBufferFormats = WTF::move(parameters.preferredBufferFormats),
        .rendererBufferTransportMode = parameters.usesHardwareBufferTransport ? RendererBufferTransportMode::Hardware : RendererBufferTransportMode::SharedMemory,
    };
    // Frames go to the UI process directly. It has to know the surface before any of its buffers.
    Ref connectionToUIProcess = *GPUProcess::singleton().parentProcessConnection();
    connectionToUIProcess->send(Messages::GPUProcessProxy::DidCreateCoordinatedSurface(m_webProcessIdentifier, parameters.pageIdentifier, m_identifier), 0);
    m_surface = AcceleratedSurface::create(connectionToUIProcess, m_identifier, WTF::move(surfaceParameters), [this] {
        frameComplete();
    });
    m_surface->didCreateCompositingRunLoop(m_workQueue->runLoop());

    connectionToWebProcess.messageReceiverMap().addMessageReceiver(Messages::RemoteCoordinatedCompositor::messageReceiverName(), m_identifier, *this);

    m_workQueue->dispatchSync([this] {
        auto& display = PlatformDisplay::sharedDisplay();
        auto context = GLContext::create(display, 0);
        if (!context || !context->makeContextCurrent())
            return;

        display.setSkiaGLContextForCurrentThread(WTF::move(context));
        m_threadSafeGrContext = display.skiaGrContext()->threadSafeProxy();
        m_tileManager = CoordinatedTileManager::create(sk_sp<GrContextThreadSafeProxy> { m_threadSafeGrContext });
    });

    // Tiles are painted on the CPU when asked to, as the web process does when it paints them.
    const char* enableCPURendering = getenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING");
    if (!enableCPURendering || StringView::fromLatin1(enableCPURendering) == "0"_s)
        m_threadSafeGrContextForRecording = m_threadSafeGrContext;
}

RemoteCoordinatedCompositor::~RemoteCoordinatedCompositor()
{
    ASSERT(!m_surface);
}

void RemoteCoordinatedCompositor::invalidate()
{
    ASSERT(RunLoop::isMain());
    if (RefPtr connectionToWebProcess = m_connectionToWebProcess)
        connectionToWebProcess->messageReceiverMap().removeMessageReceiver(Messages::RemoteCoordinatedCompositor::messageReceiverName(), m_identifier);

    m_workQueue->dispatchSync([this] {
        m_isInvalidated = true;
        m_recordings.clear();
        m_paintingTileBuffers.clear();
        m_tileManager = nullptr;
        for (Ref layer : m_layers.values())
            layer->invalidate();
        m_layers.clear();
        m_rootLayer = nullptr;
        m_surface->willDestroyGLContext();
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(nullptr);
        m_threadSafeGrContext = nullptr;
        didComposite();
    });

    m_surface->willDestroyCompositingRunLoop();
    m_surface = nullptr;
}

void RemoteCoordinatedCompositor::didRecordTiles(uint64_t recordingIdentifier, Ref<SkiaRecordingResult>&& recording)
{
    // A commit using the recording is sent once the web process knows it was recorded, so it is queued after this.
    m_workQueue->dispatch([protectedThis = Ref { *this }, recordingIdentifier, recording = WTF::move(recording)] mutable {
        if (!protectedThis->m_isInvalidated)
            protectedThis->m_recordings.set(recordingIdentifier, WTF::move(recording));
    });
}

SkiaCompositingLayer& RemoteCoordinatedCompositor::ensureLayer(PlatformLayerIdentifier identifier)
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    return m_layers.ensure(identifier, [&] {
        Ref layer = SkiaCompositingLayer::create();
#if ENABLE(DAMAGE_TRACKING)
        layer->setDamagePropagationEnabled(m_damage.propagationEnabled);
#endif
        return layer;
    }).iterator->value.get();
}

void RemoteCoordinatedCompositor::applyLayerProperties(SkiaCompositingLayer& layer, const CoordinatedLayerProperties& properties)
{
    auto changes = properties.changes;
    if (changes.contains(CoordinatedLayerChange::Position))
        layer.setPosition(properties.position);
    if (changes.contains(CoordinatedLayerChange::BoundsOrigin))
        layer.setBoundsOrigin(properties.boundsOrigin);
    if (changes.contains(CoordinatedLayerChange::AnchorPoint))
        layer.setAnchorPoint(properties.anchorPoint);
    if (changes.contains(CoordinatedLayerChange::Size))
        layer.setSize(properties.size);
    if (changes.contains(CoordinatedLayerChange::Transform))
        layer.setTransform(properties.transform);
    if (changes.contains(CoordinatedLayerChange::ChildrenTransform))
        layer.setChildrenTransform(properties.childrenTransform);
    if (changes.contains(CoordinatedLayerChange::Preserves3D))
        layer.setPreserves3D(properties.preserves3D);
    if (changes.contains(CoordinatedLayerChange::MasksToBounds))
        layer.setMasksToBounds(properties.masksToBounds);
    if (changes.contains(CoordinatedLayerChange::BackfaceVisibility))
        layer.setBackfaceVisibility(properties.backfaceVisibility);
    if (changes.contains(CoordinatedLayerChange::BackgroundColor))
        layer.setBackgroundColor(properties.backgroundColor);
    if (changes.contains(CoordinatedLayerChange::Opacity))
        layer.setOpacity(properties.opacity);
    if (changes.contains(CoordinatedLayerChange::BlendMode))
        layer.setBlendMode(properties.blendMode);
    if (changes.contains(CoordinatedLayerChange::BackingStore))
        layer.setUseBackingStore(properties.hasBackingStore);
    if (changes.contains(CoordinatedLayerChange::ContentsVisible))
        layer.setContentsVisible(properties.contentsVisible);
    if (changes.contains(CoordinatedLayerChange::ContentsOpaque))
        layer.setContentsOpaque(properties.contentsOpaque);
    if (changes.contains(CoordinatedLayerChange::ContentsRect))
        layer.setContentsRect(properties.contentsRect);
    if (changes.contains(CoordinatedLayerChange::ContentsRectClipsDescendants))
        layer.setContentsRectClipsDescendants(properties.contentsRectClipsDescendants);
    if (changes.contains(CoordinatedLayerChange::ContentsClippingRect))
        layer.setContentsClippingRect(properties.contentsClippingRect);
    if (changes.contains(CoordinatedLayerChange::ContentsClipShapePath)) {
        if (properties.contentsClipShapePath.isEmpty())
            layer.setContentsClipPath(std::nullopt);
        else {
            auto clipPath = *properties.contentsClipShapePath.platformPath();
            clipPath.setFillType(SkPathFillType::kWinding);
            layer.setContentsClipPath(WTF::move(clipPath));
        }
    }
    if (changes.contains(CoordinatedLayerChange::ContentsTileSize))
        layer.setContentsTiling(properties.contentsTileSize, properties.contentsTilePhase);
    if (changes.contains(CoordinatedLayerChange::ContentsColor))
        layer.setContentsSolidColor(properties.contentsColor);
    if (changes.contains(CoordinatedLayerChange::ClipPath)) {
        auto clipPath = *properties.clipPath.platformPath();
        clipPath.setFillType(properties.clipPathWindRule == WindRule::EvenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
        layer.setClipPath(WTF::move(clipPath));
    }
    if (changes.contains(CoordinatedLayerChange::Filters))
        layer.setFilters(properties.filters);
    if (changes.contains(CoordinatedLayerChange::Mask))
        layer.setMask(properties.mask ? RefPtr { &ensureLayer(*properties.mask) } : nullptr);
    if (changes.contains(CoordinatedLayerChange::Replica))
        layer.setReplica(properties.replica ? RefPtr { &ensureLayer(*properties.replica) } : nullptr);
    if (changes.contains(CoordinatedLayerChange::BackdropFilters))
        layer.setBackdropFilters(properties.backdropFilters);
    if (changes.contains(CoordinatedLayerChange::BackdropRect))
        layer.setBackdropFiltersRect(properties.backdropRect);
    if (changes.contains(CoordinatedLayerChange::BackdropShapePath)) {
        if (properties.backdropShapePath.isEmpty())
            layer.setBackdropFiltersClipPath(std::nullopt);
        else {
            auto clipPath = *properties.backdropShapePath.platformPath();
            clipPath.setFillType(SkPathFillType::kWinding);
            layer.setBackdropFiltersClipPath(WTF::move(clipPath));
        }
    }
    if (changes.contains(CoordinatedLayerChange::BackdropRoot))
        layer.setIsBackdropRoot(properties.isBackdropRoot);
    if (changes.contains(CoordinatedLayerChange::DebugBorderColor))
        layer.setDebugIndicators(Color { properties.debugBorderColor }, properties.debugBorderWidth, properties.repaintCount);
    if (changes.contains(CoordinatedLayerChange::Children)) {
        layer.setChildren(WTF::map(properties.children, [&](auto identifier) {
            return Ref { ensureLayer(identifier) };
        }));
    }
    if (changes.contains(CoordinatedLayerChange::Animations))
        layer.setAnimations(properties.animations);
#if ENABLE(DAMAGE_TRACKING)
    if (changes.contains(CoordinatedLayerChange::Damage))
        layer.addDamage(properties.damage.span());
#endif
}

static std::span<const uint8_t> pixelsSpan(std::span<const uint8_t> pixels, const CoordinatedPixels& location)
{
    if (location.size.isEmpty())
        return { };

    auto size = CheckedSize(location.size.width()) * static_cast<size_t>(location.size.height()) * static_cast<size_t>(4);
    if (size.hasOverflowed() || location.offset > pixels.size() || size.value() > pixels.size() - location.offset)
        return { };

    return pixels.subspan(location.offset, size.value());
}

static sk_sp<SkImage> rasterImage(std::span<const uint8_t> pixels, const CoordinatedPixels& location)
{
    auto data = pixelsSpan(pixels, location);
    if (data.empty())
        return nullptr;

    auto info = SkImageInfo::Make(location.size.width(), location.size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    return SkImages::RasterFromData(info, SkData::MakeWithCopy(data.data(), data.size()), info.minRowBytes());
}

void RemoteCoordinatedCompositor::applyLayerUpdate(CoordinatedLayerUpdate&& update, std::span<const uint8_t> pixels)
{
    Ref layer = ensureLayer(update.properties.id);
    applyLayerProperties(layer.get(), update.properties);

    if (update.properties.changes.contains(CoordinatedLayerChange::ContentsBuffer)) {
        std::unique_ptr<CoordinatedPlatformLayerBuffer> buffer;
        if (update.contentsBuffer) {
            if (auto image = rasterImage(pixels, *update.contentsBuffer))
                buffer = CoordinatedPlatformLayerBufferSkiaImage::create(image, m_threadSafeGrContext);
        }
#if ENABLE(WEBGL) && USE(GBM)
        if (const auto& contents = update.properties.contentsBuffer) {
            if (auto frame = RemoteCoordinatedContentsRegistry::singleton().takeFrame(m_webProcessIdentifier, contents->identifier)) {
                buffer = WTF::switchOn(WTF::move(*frame),
                    [&](RemoteCoordinatedContentsRegistry::DMABufFrame&& dmabufFrame) -> std::unique_ptr<CoordinatedPlatformLayerBuffer> {
                        return CoordinatedPlatformLayerBufferDMABuf::create(WTF::move(dmabufFrame.buffer), contents->alphaMode, contents->origin, WTF::move(dmabufFrame.fence), m_threadSafeGrContext);
                    },
                    [&](RemoteCoordinatedContentsRegistry::TextureFrame&& textureFrame) -> std::unique_ptr<CoordinatedPlatformLayerBuffer> {
                        return CoordinatedPlatformLayerBufferSkiaImage::create(WTF::move(textureFrame.texture), textureFrame.size, contents->alphaMode, contents->origin, WTF::move(textureFrame.fence), m_threadSafeGrContext);
                    });
            }
        }
#endif
        layer->setContentsBuffer(WTF::move(buffer));
    }

    if (update.properties.changes.contains(CoordinatedLayerChange::ContentsImage)) {
        RefPtr<CoordinatedImageBackingStore> imageBackingStore;
        if (update.contentsImage) {
            if (RefPtr image = NativeImage::create(rasterImage(pixels, *update.contentsImage)))
                imageBackingStore = CoordinatedImageBackingStore::create(image.releaseNonNull(), m_threadSafeGrContext);
        }
        layer->setImageBackingStore(imageBackingStore.get());
    }

    if (!update.backingStore)
        return;

    Vector<CoordinatedBackingStoreProxy::Update::TileUpdate> tilesToUpdate;
    for (auto& tileUpdate : update.backingStore->tilesToUpdate) {
        RefPtr recording = m_recordings.get(tileUpdate.recording);
        if (!recording || !m_tileManager)
            continue;

        auto dirtyRect = tileUpdate.dirtyRect;
        dirtyRect.moveBy(tileUpdate.tileRect.location());
        Ref buffer = m_tileManager->scheduleReplay(recording.releaseNonNull(), tileUpdate.tileRect, dirtyRect);
        m_paintingTileBuffers.append(buffer);
        tilesToUpdate.append({ tileUpdate.tileID, tileUpdate.tileRect, tileUpdate.dirtyRect, WTF::move(buffer) });
    }

    CoordinatedBackingStoreProxy::Update backingStoreUpdate;
    backingStoreUpdate.appendUpdate(WTF::move(update.backingStore->tilesToCreate), WTF::move(tilesToUpdate), WTF::move(update.backingStore->tilesToRemove));
    layer->updateBackingStore(WTF::move(backingStoreUpdate), update.backingStore->contentsScale);
}

void RemoteCoordinatedCompositor::commit(CoordinatedLayerTreeTransaction&& transaction, CompletionHandler<void()>&& completionHandler)
{
    m_workQueue->dispatch([protectedThis = Ref { *this }, transaction = WTF::move(transaction), completionHandler = WTF::move(completionHandler)] mutable {
        protectedThis->m_pendingCommitCompletionHandlers.append(WTF::move(completionHandler));
        if (protectedThis->m_isInvalidated) {
            protectedThis->didComposite();
            return;
        }

        for (auto identifier : transaction.removedLayers) {
            if (auto layer = protectedThis->m_layers.take(identifier))
                layer->invalidate();
        }

        auto pixels = transaction.pixels.span();
        for (auto& update : transaction.layers)
            protectedThis->applyLayerUpdate(WTF::move(update), pixels);

        protectedThis->m_rootLayer = &protectedThis->ensureLayer(transaction.rootLayer);

        // Every recording arrives before the commit that uses it, and the web process doesn't record again until it is composited.
        protectedThis->m_recordings.clear();
        for (Ref buffer : std::exchange(protectedThis->m_paintingTileBuffers, { }))
            buffer->waitUntilPaintingFinished();

        for (Ref layer : protectedThis->m_layers.values()) {
            if (layer->hasPendingBackingStoreTileUpdates())
                layer->processPendingTileUpdates();
        }

        protectedThis->scheduleRender();
    });
}

void RemoteCoordinatedCompositor::updateLayers(CoordinatedLayerTreeTransaction&& transaction)
{
    m_workQueue->dispatch([protectedThis = Ref { *this }, transaction = WTF::move(transaction)] mutable {
        if (protectedThis->m_isInvalidated)
            return;

        auto pixels = transaction.pixels.span();
        for (auto& update : transaction.layers) {
            if (protectedThis->m_layers.contains(update.properties.id))
                protectedThis->applyLayerUpdate(WTF::move(update), pixels);
        }
        protectedThis->scheduleRender();
    });
}

void RemoteCoordinatedCompositor::setSize(IntSize size, float deviceScaleFactor)
{
    m_workQueue->dispatch([protectedThis = Ref { *this }, size, deviceScaleFactor] {
        protectedThis->m_viewportSize = size;
        protectedThis->m_deviceScaleFactor = deviceScaleFactor;
    });
}

void RemoteCoordinatedCompositor::setVisible(bool isVisible)
{
    if (m_surface)
        m_surface->visibilityDidChange(isVisible);

    m_workQueue->dispatch([protectedThis = Ref { *this }, isVisible] {
        protectedThis->m_isVisible = isVisible;
        if (isVisible && !protectedThis->m_isInvalidated)
            protectedThis->scheduleRender();
    });
}

void RemoteCoordinatedCompositor::backgroundColorDidChange(std::optional<Color>&& color)
{
    if (m_surface)
        m_surface->backgroundColorDidChange(WTF::move(color));
}

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void RemoteCoordinatedCompositor::preferredBufferFormatsDidChange(Vector<RendererBufferFormat>&& formats)
{
    if (m_surface)
        m_surface->preferredBufferFormatsDidChange(WTF::move(formats));
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void RemoteCoordinatedCompositor::setDamagePropagationSettings(bool propagationEnabled, bool unified, bool usedForCompositing, uint32_t rectangleThreshold)
{
    m_workQueue->dispatch([protectedThis = Ref { *this }, propagationEnabled, unified, usedForCompositing, rectangleThreshold] {
        if (protectedThis->m_isInvalidated)
            return;

        auto& damage = protectedThis->m_damage;
        damage.propagationEnabled = propagationEnabled;
        damage.unified = propagationEnabled && unified;
        damage.usedForCompositing = propagationEnabled && usedForCompositing;
        for (Ref layer : protectedThis->m_layers.values())
            layer->setDamagePropagationEnabled(propagationEnabled);

        protectedThis->m_surface->setFrameDamageRectangleThreshold(Damage::clampRectangleThreshold(rectangleThreshold));
        protectedThis->m_surface->setDamageUsedForCompositing(damage.usedForCompositing);
    });
}

void RemoteCoordinatedCompositor::enableFrameDamageNotificationForTesting()
{
    m_workQueue->dispatch([protectedThis = Ref { *this }] {
        protectedThis->m_damage.shouldNotifyFrameDamageForTesting = true;
    });
}
#endif

void RemoteCoordinatedCompositor::scheduleRender()
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    if (m_isWaitingForFrameDone) {
        m_needsRender = true;
        return;
    }
    render();
}

void RemoteCoordinatedCompositor::render()
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    m_needsRender = false;

    if (m_isInvalidated || !m_threadSafeGrContext || !m_isVisible || !m_rootLayer || m_viewportSize.isEmpty()) {
        didComposite();
        return;
    }

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    if (!grContext) {
        didComposite();
        return;
    }

    auto scaledSize = m_viewportSize;
    scaledSize.scale(m_deviceScaleFactor);
    m_surface->willRenderFrame(scaledSize);

    auto* canvas = m_surface->canvas();
    if (!canvas) {
        m_surface->didRenderFrame(TargetContents::Invalid);
        didComposite();
        return;
    }

    TransformationMatrix viewportTransform;
    viewportTransform.scale(m_deviceScaleFactor);
    m_rootLayer->setTransform(viewportTransform);

    std::optional<Damage> frameDamage;
    std::optional<Damage> priorTargetDamage;
#if ENABLE(DAMAGE_TRACKING)
    if (m_damage.propagationEnabled)
        frameDamage = Damage(scaledSize, m_damage.unified ? Damage::Mode::BoundingBox : Damage::Mode::Rectangles);
    if (m_damage.usedForCompositing)
        priorTargetDamage = m_surface->renderTargetDamage();
#endif

    canvas->save();
    if (m_rootLayer->paint(*canvas, frameDamage, priorTargetDamage, m_surface->skiaClearColor(CompositionReason::RenderingUpdate)))
        m_needsRender = true;
    canvas->restore();

#if ENABLE(DAMAGE_TRACKING)
    if (frameDamage) {
        // Sent before the commit is completed, so the web process has it before its next rendering update.
        if (m_damage.shouldNotifyFrameDamageForTesting)
            m_webProcessConnection->send(Messages::RemoteCoordinatedCompositorProxy::DidRenderFrameForTesting(frameDamage->regionForTesting()), m_identifier);
        if (!frameDamage->isEmpty())
            m_surface->setFrameDamage(WTF::move(*frameDamage));
    }
#endif

    if (auto* surface = canvas->getSurface())
        grContext->flushAndSubmit(surface, GrSyncCpu::kNo);
    PlatformDisplay::sharedDisplay().skiaGLContext()->swapBuffers();

    m_surface->didRenderFrame();
    m_surface->sendFrame();
    m_isWaitingForFrameDone = true;

    didComposite();
}

void RemoteCoordinatedCompositor::didComposite()
{
    auto completionHandlers = std::exchange(m_pendingCommitCompletionHandlers, { });
    if (completionHandlers.isEmpty())
        return;

    RunLoop::mainSingleton().dispatch([completionHandlers = WTF::move(completionHandlers)] mutable {
        for (auto& completionHandler : completionHandlers)
            completionHandler();
    });
}

void RemoteCoordinatedCompositor::frameComplete()
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    m_isWaitingForFrameDone = false;
    if (m_needsRender && !m_isInvalidated)
        render();
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
