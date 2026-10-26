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
#include "RemoteCoordinatedCompositorMessages.h"
#include "RemoteCoordinatedContentsRegistry.h"
#include <WebCore/CoordinatedBackingStoreProxy.h>
#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/CoordinatedPlatformLayerBufferDMABuf.h>
#include <WebCore/CoordinatedPlatformLayerBufferSkiaImage.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/GLContext.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/SkiaCompositingLayer.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteCoordinatedCompositor);

Ref<RemoteCoordinatedCompositor> RemoteCoordinatedCompositor::create(GPUConnectionToWebProcess& connectionToWebProcess, uint64_t identifier, Parameters&& parameters)
{
    return adoptRef(*new RemoteCoordinatedCompositor(connectionToWebProcess, identifier, WTF::move(parameters)));
}

RemoteCoordinatedCompositor::RemoteCoordinatedCompositor(GPUConnectionToWebProcess& connectionToWebProcess, uint64_t identifier, Parameters&& parameters)
    : m_connectionToWebProcess(connectionToWebProcess)
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
    m_surface = AcceleratedSurface::create(connectionToWebProcess.connection(), m_identifier, WTF::move(surfaceParameters), [this] {
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
    });
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

SkiaCompositingLayer& RemoteCoordinatedCompositor::ensureLayer(PlatformLayerIdentifier identifier)
{
    ASSERT(m_workQueue->runLoop().isCurrent());
    return m_layers.ensure(identifier, [] {
        return SkiaCompositingLayer::create();
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

void RemoteCoordinatedCompositor::applyLayerUpdate(CoordinatedLayerUpdate&& update, std::span<const uint8_t> pixels)
{
    Ref layer = ensureLayer(update.properties.id);
    applyLayerProperties(layer.get(), update.properties);

#if ENABLE(WEBGL) && USE(GBM)
    if (update.properties.changes.contains(CoordinatedLayerChange::ContentsBuffer)) {
        std::unique_ptr<CoordinatedPlatformLayerBuffer> buffer;
        if (const auto& contents = update.properties.contentsBuffer) {
            if (auto frame = RemoteCoordinatedContentsRegistry::singleton().takeFrame(m_webProcessIdentifier, contents->identifier)) {
                buffer = WTF::switchOn(WTF::move(*frame),
                    [&](RemoteCoordinatedContentsRegistry::DMABufFrame&& dmabufFrame) -> std::unique_ptr<CoordinatedPlatformLayerBuffer> {
                        return CoordinatedPlatformLayerBufferDMABuf::create(WTF::move(dmabufFrame.buffer), contents->alphaMode, contents->origin, WTF::move(dmabufFrame.fence), m_threadSafeGrContext);
                    },
                    [&](RemoteCoordinatedContentsRegistry::TextureFrame&& textureFrame) -> std::unique_ptr<CoordinatedPlatformLayerBuffer> {
                        return CoordinatedPlatformLayerBufferSkiaImage::create(WTF::move(textureFrame.texture), textureFrame.size, contents->alphaMode, WTF::move(textureFrame.fence), m_threadSafeGrContext);
                    });
            }
        }
        layer->setContentsBuffer(WTF::move(buffer));
    }
#endif

    if (update.properties.changes.contains(CoordinatedLayerChange::ContentsImage)) {
        RefPtr<CoordinatedImageBackingStore> imageBackingStore;
        if (update.contentsImage) {
            if (auto data = pixelsSpan(pixels, *update.contentsImage); !data.empty()) {
                auto& size = update.contentsImage->size;
                auto info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
                if (RefPtr image = NativeImage::create(SkImages::RasterFromData(info, SkData::MakeWithCopy(data.data(), data.size()), info.minRowBytes())))
                    imageBackingStore = CoordinatedImageBackingStore::create(image.releaseNonNull(), m_threadSafeGrContext);
            }
        }
        layer->setImageBackingStore(imageBackingStore.get());
    }

    if (!update.backingStore)
        return;

    Vector<CoordinatedBackingStoreProxy::Update::TileUpdate> tilesToUpdate;
    for (auto& tileUpdate : update.backingStore->tilesToUpdate) {
        auto data = pixelsSpan(pixels, tileUpdate.pixels);
        if (data.empty())
            continue;

        auto flags = tileUpdate.supportsAlpha ? CoordinatedTileBuffer::SupportsAlpha : CoordinatedTileBuffer::NoFlags;
        auto buffer = CoordinatedUnacceleratedTileBuffer::createPainted(tileUpdate.pixels.size, flags, Vector<uint8_t> { data });
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

        for (Ref layer : protectedThis->m_layers.values()) {
            if (layer->hasPendingBackingStoreTileUpdates())
                layer->processPendingTileUpdates();
        }

        protectedThis->scheduleRender();
    });
}

void RemoteCoordinatedCompositor::updatePositions(Vector<CoordinatedLayerProperties>&& layers)
{
    m_workQueue->dispatch([protectedThis = Ref { *this }, layers = WTF::move(layers)] {
        if (protectedThis->m_isInvalidated)
            return;

        for (const auto& properties : layers) {
            if (auto layer = protectedThis->m_layers.get(properties.id))
                protectedThis->applyLayerProperties(*layer, properties);
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

void RemoteCoordinatedCompositor::preferredBufferFormatsDidChange(Vector<RendererBufferFormat>&& formats)
{
    if (m_surface)
        m_surface->preferredBufferFormatsDidChange(WTF::move(formats));
}

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
    canvas->save();
    if (m_rootLayer->paint(*canvas, frameDamage, std::nullopt, m_surface->skiaClearColor(CompositionReason::RenderingUpdate)))
        m_needsRender = true;
    canvas->restore();

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
