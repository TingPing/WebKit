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
#include "RemoteCoordinatedCompositorProxy.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "AcceleratedSurface.h"
#include "CoordinatedLayerTreeTransaction.h"
#include "CoordinatedSceneState.h"
#include "DrawingArea.h"
#include "GPUConnectionToWebProcessMessages.h"
#include "LayerTreeHost.h"
#include "RemoteCoordinatedCompositorMessages.h"
#include "RemoteCoordinatedCompositorProxyMessages.h"
#include "RemoteGraphicsContextProxy.h"
#include "RemoteRenderingBackendProxy.h"
#include "RenderProcessInfo.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/Document.h>
#include <WebCore/NativeImage.h>
#include <WebCore/Page.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/VideoFrame.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#if ENABLE(VIDEO) && USE(GSTREAMER)
#include <WebCore/VideoFrameGStreamer.h>
#endif
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteCoordinatedCompositorProxy);

struct PendingLayerTreeTransaction {
    PlatformLayerIdentifier rootLayer;
    Vector<PlatformLayerIdentifier> removedLayers;
    Vector<CoordinatedPlatformLayer::PendingChanges> layers;
};

static CoordinatedPixels appendPixels(SharedBufferBuilder& pixels, Vector<uint8_t>&& data, const IntSize& size)
{
    CoordinatedPixels result { .offset = pixels.size(), .size = size };
    pixels.append(WTF::move(data));
    return result;
}

static CoordinatedBackingStoreUpdate backingStoreUpdate(const CoordinatedBackingStoreProxy::Update& update, float contentsScale)
{
    CoordinatedBackingStoreUpdate backingStore {
        .contentsScale = contentsScale,
        .tilesToCreate = update.tilesToCreate(),
        .tilesToUpdate = { },
        .tilesToRemove = update.tilesToRemove(),
    };

    for (const auto& tileUpdate : update.tilesToUpdate()) {
        RefPtr buffer = dynamicDowncast<CoordinatedRemoteTileBuffer>(tileUpdate.buffer);
        if (!buffer)
            continue;

        backingStore.tilesToUpdate.append({
            .tileID = tileUpdate.tileID,
            .tileRect = tileUpdate.tileRect,
            .dirtyRect = tileUpdate.dirtyRect,
            .recording = buffer->recordingIdentifier(),
        });
    }

    return backingStore;
}

static std::optional<CoordinatedPixels> imagePixels(NativeImage& image, SharedBufferBuilder& pixels)
{
    const auto& platformImage = image.platformImage();
    if (!platformImage)
        return std::nullopt;

    auto info = SkImageInfo::Make(platformImage->width(), platformImage->height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    Vector<uint8_t> data(info.computeMinByteSize());
    if (data.isEmpty() || !platformImage->readPixels(nullptr, info, data.mutableSpan().data(), info.minRowBytes(), 0, 0))
        return std::nullopt;

    return appendPixels(pixels, WTF::move(data), { platformImage->width(), platformImage->height() });
}

CoordinatedLayerTreeTransaction RemoteCoordinatedCompositorProxy::makeTransaction(PendingLayerTreeTransaction&& pendingTransaction)
{
    CoordinatedLayerTreeTransaction transaction {
        .rootLayer = pendingTransaction.rootLayer,
        .layers = { },
        .removedLayers = WTF::move(pendingTransaction.removedLayers),
        .pixels = { },
        .removedVideoFrameSlots = { },
    };

    SharedBufferBuilder pixels;
    for (auto& changes : pendingTransaction.layers) {
        CoordinatedLayerUpdate update { .properties = WTF::move(changes.properties), .backingStore = { }, .contentsImage = { }, .contentsBuffer = { }, .contentsVideoFrame = { }, .contentsHolePunch = changes.contentsBufferHolePunchSize };
        if (changes.backingStoreUpdate)
            update.backingStore = backingStoreUpdate(*changes.backingStoreUpdate, changes.contentsScale);
        if (RefPtr image = changes.contentsImage)
            update.contentsImage = imagePixels(*image, pixels);
        if (RefPtr image = changes.contentsBufferImage)
            update.contentsBuffer = imagePixels(*image, pixels);
        if (RefPtr videoFrame = changes.contentsBufferVideoFrame)
            update.contentsVideoFrame = videoFrameInSlot(*videoFrame, changes.contentsBufferRotation);
        transaction.layers.append(WTF::move(update));
    }

    if (!pixels.isEmpty())
        transaction.pixels = IPC::SharedBufferReference(pixels.takeBuffer());

    Locker locker { m_videoFrameSlotsLock };
    transaction.removedVideoFrameSlots = std::exchange(m_removedVideoFrameSlots, { });

    return transaction;
}

std::optional<CoordinatedVideoFrame> RemoteCoordinatedCompositorProxy::videoFrameInSlot(VideoFrame& videoFrame, CoordinatedPlatformLayerBuffer::Rotation rotation)
{
#if ENABLE(VIDEO) && USE(GSTREAMER)
    RefPtr frame = dynamicDowncast<VideoFrameGStreamer>(videoFrame);
    if (!frame)
        return std::nullopt;

    std::optional<CoordinatedVideoFrameSlotIdentifier> slotIdentifier;
    RefPtr<SharedMemory> memory;
    std::optional<SharedMemory::Handle> handle;
    auto layout = GStreamerVideoFrameLayout::write(*frame, [&](size_t size) -> std::span<uint8_t> {
        Locker locker { m_videoFrameSlotsLock };
        for (auto& entry : m_videoFrameSlots) {
            if (!entry.value.isInUse && entry.value.memory->size() >= size && entry.value.memory->size() <= 2 * size) {
                slotIdentifier = entry.key;
                break;
            }
        }

        if (!slotIdentifier) {
            // The free slots don't fit, usually because the size of the video changed.
            Vector<CoordinatedVideoFrameSlotIdentifier> removedSlots;
            m_videoFrameSlots.removeIf([&](auto& entry) {
                if (entry.value.isInUse)
                    return false;
                removedSlots.append(entry.key);
                return true;
            });
            m_removedVideoFrameSlots.appendVector(WTF::move(removedSlots));

            RefPtr newMemory = SharedMemory::allocate(size);
            if (!newMemory)
                return { };
            slotIdentifier = CoordinatedVideoFrameSlotIdentifier::generate();
            m_videoFrameSlots.add(*slotIdentifier, VideoFrameSlot { .memory = WTF::move(newMemory) });
        }

        auto& slot = m_videoFrameSlots.find(*slotIdentifier)->value;
        if (!slot.wasSent) {
            handle = slot.memory->createHandle(SharedMemory::Protection::ReadOnly);
            if (!handle)
                return { };
            slot.wasSent = true;
        }
        slot.isInUse = true;
        memory = slot.memory;
        return memory->mutableSpan();
    });
    if (!layout)
        return std::nullopt;

    return CoordinatedVideoFrame {
        .slot = *slotIdentifier,
        .slotHandle = WTF::move(handle),
        .layout = WTF::move(*layout),
        .rotation = rotation,
    };
#else
    UNUSED_PARAM(videoFrame);
    UNUSED_PARAM(rotation);
    return std::nullopt;
#endif
}

void RemoteCoordinatedCompositorProxy::didUpdateHolePunchVideoRectangle(PlatformLayerIdentifier identifier, IntRect&& rect)
{
#if ENABLE(VIDEO) && USE(GSTREAMER)
    m_sceneState->forEachLayer([&](CoordinatedPlatformLayer& layer) {
        if (layer.id() == identifier)
            layer.setHolePunchVideoRectangle(rect);
    });
#else
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(rect);
#endif
}

void RemoteCoordinatedCompositorProxy::releaseVideoFrameSlot(CoordinatedVideoFrameSlotIdentifier identifier)
{
    Locker locker { m_videoFrameSlotsLock };
    auto it = m_videoFrameSlots.find(identifier);
    if (it != m_videoFrameSlots.end())
        it->value.isInUse = false;
}

Ref<RemoteCoordinatedCompositorProxy> RemoteCoordinatedCompositorProxy::create(WebPage& webPage, LayerTreeHost& layerTreeHost, CoordinatedSceneState& sceneState)
{
    return adoptRef(*new RemoteCoordinatedCompositorProxy(webPage, layerTreeHost, sceneState));
}

RemoteCoordinatedCompositorProxy::RemoteCoordinatedCompositorProxy(WebPage& webPage, LayerTreeHost& layerTreeHost, CoordinatedSceneState& sceneState)
    : m_webPage(webPage)
    , m_layerTreeHost(&layerTreeHost)
    , m_sceneState(sceneState)
    , m_surfaceID(AcceleratedSurface::generateSurfaceID())
    , m_commitQueue(WorkQueue::create("org.webkit.RemoteCoordinatedCompositorCommit"_s))
{
    ASSERT(RunLoop::isMain());
    connectToGPUProcess(webPage);
}

void RemoteCoordinatedCompositorProxy::connectToGPUProcess(WebPage& webPage)
{
    ASSERT(RunLoop::isMain());
    Ref gpuProcessConnection = WebProcess::singleton().ensureGPUProcessConnection();
    gpuProcessConnection->addClient(*this);
    gpuProcessConnection->messageReceiverMap().addMessageReceiver(Messages::RemoteCoordinatedCompositorProxy::messageReceiverName(), m_surfaceID, *this);
    m_gpuProcessConnection = gpuProcessConnection.get();
    Ref connection = gpuProcessConnection->connection();
    // Synchronous, because tile recordings reach the GPU process through another connection and need the compositor.
    connection->sendSync(Messages::GPUConnectionToWebProcess::CreateCoordinatedCompositor(m_surfaceID, webPage.webPageProxyIdentifier(), webPage.size(), webPage.deviceScaleFactor(), webPage.effectiveBackgroundColor(),
        webPage.activityState().contains(ActivityState::IsVisible), webPage.preferredBufferFormats(), WebProcess::singleton().rendererBufferTransportMode().contains(RendererBufferTransportMode::Hardware)), 0);

    // Only now can the commit queue send to the compositor.
    Locker locker { m_connectionLock };
    m_gpuConnection = connection.ptr();
}

RemoteCoordinatedCompositorProxy::~RemoteCoordinatedCompositorProxy() = default;

RefPtr<IPC::Connection> RemoteCoordinatedCompositorProxy::gpuConnection() const
{
    Locker locker { m_connectionLock };
    return m_gpuConnection;
}

void RemoteCoordinatedCompositorProxy::invalidate()
{
    ASSERT(RunLoop::isMain());
    m_isInvalidated = true;
    m_didCompositeFunction = nullptr;
    m_layerTreeHost = nullptr;
    if (auto gpuProcessConnection = m_gpuProcessConnection.get())
        gpuProcessConnection->messageReceiverMap().removeMessageReceiver(Messages::RemoteCoordinatedCompositorProxy::messageReceiverName(), m_surfaceID);

    RefPtr connection = gpuConnection();
    if (!connection)
        return;

    m_commitQueue->dispatch([connection = connection.releaseNonNull(), surfaceID = m_surfaceID] {
        connection->send(Messages::GPUConnectionToWebProcess::ReleaseCoordinatedCompositor(surfaceID), 0);
    });
}

int RemoteCoordinatedCompositorProxy::maxTextureSize() const
{
    ASSERT(RunLoop::isMain());
    if (!m_maxTextureSize) {
        static constexpr int fallbackMaxTextureSize = 4096;
        auto maxSize = WebProcess::singleton().ensureGPUProcessConnection().maxAcceleratedImageBufferSize();
        m_maxTextureSize = maxSize ? std::min<uint32_t>(maxSize, std::numeric_limits<int>::max()) : fallbackMaxTextureSize;
    }
    return m_maxTextureSize;
}

void RemoteCoordinatedCompositorProxy::backgroundColorDidChange()
{
    RefPtr webPage = m_webPage;
    RefPtr connection = gpuConnection();
    if (!webPage || !connection)
        return;

    connection->send(Messages::RemoteCoordinatedCompositor::BackgroundColorDidChange(webPage->effectiveBackgroundColor()), m_surfaceID);
}

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void RemoteCoordinatedCompositorProxy::preferredBufferFormatsDidChange()
{
    RefPtr webPage = m_webPage;
    RefPtr connection = gpuConnection();
    if (!webPage || !connection)
        return;

    connection->send(Messages::RemoteCoordinatedCompositor::PreferredBufferFormatsDidChange(webPage->preferredBufferFormats()), m_surfaceID);
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void RemoteCoordinatedCompositorProxy::setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>> flags, unsigned rectangleThreshold)
{
    m_damagePropagationSettings = DamagePropagationSettings { flags, rectangleThreshold };
    sendDamageSettings();
}

void RemoteCoordinatedCompositorProxy::enableFrameDamageNotificationForTesting()
{
    m_shouldNotifyFrameDamageForTesting = true;
    if (RefPtr connection = gpuConnection())
        connection->send(Messages::RemoteCoordinatedCompositor::EnableFrameDamageNotificationForTesting(), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::sendDamageSettings()
{
    RefPtr connection = gpuConnection();
    if (!connection)
        return;

    if (m_damagePropagationSettings) {
        auto flags = m_damagePropagationSettings->flags;
        connection->send(Messages::RemoteCoordinatedCompositor::SetDamagePropagationSettings(!!flags, flags && flags->contains(DamagePropagationFlags::Unified),
            flags && flags->contains(DamagePropagationFlags::UseForCompositing), m_damagePropagationSettings->rectangleThreshold), m_surfaceID);
    }

    if (m_shouldNotifyFrameDamageForTesting)
        connection->send(Messages::RemoteCoordinatedCompositor::EnableFrameDamageNotificationForTesting(), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::didRenderFrameForTesting(Region&& frameDamage)
{
    if (CheckedPtr layerTreeHost = m_layerTreeHost)
        layerTreeHost->notifyFrameDamageForTesting(WTF::move(frameDamage));
}
#endif

void RemoteCoordinatedCompositorProxy::setSize(const IntSize& size, float deviceScaleFactor)
{
    if (RefPtr connection = gpuConnection())
        connection->send(Messages::RemoteCoordinatedCompositor::SetSize(size, deviceScaleFactor), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::requestCompositionForRenderingUpdate(Function<void()>&& didCompositeFunction)
{
    ASSERT(RunLoop::isMain());
    ASSERT(!m_didCompositeFunction);
    m_didCompositeFunction = WTF::move(didCompositeFunction);

    RefPtr webPage = m_webPage;
    if (webPage) {
        if (RefPtr drawingArea = webPage->drawingArea())
            drawingArea->willStartRenderingUpdateDisplay();
    }

    // The GPU process must have the tile recordings before the commit using them.
    Function<bool()> waitForRecordings;
    if (std::exchange(m_hasPendingRecordings, false) && webPage)
        waitForRecordings = webPage->ensureRemoteRenderingBackendProxy().flushImageBuffers();

    Locker locker { m_commitLock };
    PendingLayerTreeTransaction pendingTransaction { .rootLayer = m_sceneState->rootLayer().id(), .removedLayers = { }, .layers = { } };
    for (auto& layer : m_sceneState->takePendingLayersToRemove())
        pendingTransaction.removedLayers.append(layer->id());
    m_sceneState->forEachLayer([&](CoordinatedPlatformLayer& layer) {
        if (auto changes = layer.takePendingChanges())
            pendingTransaction.layers.append(WTF::move(*changes));
    });
    m_needsFullCommit = false;

    ++m_pendingCommits;
    m_commitQueue->dispatch([protectedThis = Ref { *this }, pendingTransaction = WTF::move(pendingTransaction), waitForRecordings = WTF::move(waitForRecordings)] mutable {
        if (waitForRecordings)
            waitForRecordings();
        RefPtr connection = protectedThis->gpuConnection();
        if (!connection) {
            callOnMainRunLoop([protectedThis = WTF::move(protectedThis)] {
                protectedThis->didComposite();
            });
            return;
        }

        connection->sendWithAsyncReply(Messages::RemoteCoordinatedCompositor::Commit(protectedThis->makeTransaction(WTF::move(pendingTransaction))), [protectedThis] {
            protectedThis->didComposite();
        }, protectedThis->m_surfaceID);
    });
}

void RemoteCoordinatedCompositorProxy::didComposite()
{
    ASSERT(RunLoop::isMain());
    if (m_pendingCommits)
        --m_pendingCommits;
    didCompositeRenderingUpdate();
}

void RemoteCoordinatedCompositorProxy::didCompositeRenderingUpdate()
{
    auto didCompositeFunction = std::exchange(m_didCompositeFunction, nullptr);
    if (!didCompositeFunction)
        return;

    if (RefPtr webPage = m_webPage) {
        if (RefPtr drawingArea = webPage->drawingArea())
            drawingArea->didCompleteRenderingUpdateDisplay();
    }
    didCompositeFunction();
}

void RemoteCoordinatedCompositorProxy::didUpdateFPS(float fps)
{
    RefPtr webPage = m_webPage;
    if (!webPage)
        return;

    if (RefPtr document = webPage->corePage()->localTopDocument())
        document->addConsoleMessage(MessageSource::Rendering, MessageLevel::Info, makeString("FPS: "_s, fps));
}

CoordinatedTileRecordingIdentifier RemoteCoordinatedCompositorProxy::recordTiles(const IntRect& recordRect, unsigned dirtyTilesCount, bool contentsOpaque, float contentsScale, const Function<void(GraphicsContext&)>& paint)
{
    ASSERT(RunLoop::isMain());
    auto recordingIdentifier = CoordinatedTileRecordingIdentifier::generate();
    RefPtr webPage = m_webPage;
    if (!webPage)
        return recordingIdentifier;

    Ref renderingBackend = webPage->ensureRemoteRenderingBackendProxy();
    auto recorder = renderingBackend->createCoordinatedTileRecorder(m_surfaceID, recordRect, dirtyTilesCount, contentsOpaque, contentsScale);
    paint(recorder.get());
    renderingBackend->sinkCoordinatedTileRecorder(WTF::move(recorder), recordingIdentifier);
    m_hasPendingRecordings = true;
    return recordingIdentifier;
}

void RemoteCoordinatedCompositorProxy::requestComposition(CompositionReason reason)
{
    if (reason != CompositionReason::AsyncScrolling && reason != CompositionReason::VideoFrame)
        return;

    Locker locker { m_commitLock };
    // A new compositor doesn't have the layers yet, so these changes wait for the full commit.
    if (m_needsFullCommit)
        return;

    PendingLayerTreeTransaction pendingTransaction { .rootLayer = m_sceneState->rootLayer().id(), .removedLayers = { }, .layers = { } };
    m_sceneState->forEachLayer([&](CoordinatedPlatformLayer& layer) {
        if (auto changes = layer.takePendingScrollingChanges())
            pendingTransaction.layers.append(WTF::move(*changes));
    });
    if (pendingTransaction.layers.isEmpty())
        return;

    m_commitQueue->dispatch([protectedThis = Ref { *this }, pendingTransaction = WTF::move(pendingTransaction)] mutable {
        if (RefPtr connection = protectedThis->gpuConnection())
            connection->send(Messages::RemoteCoordinatedCompositor::UpdateLayers(protectedThis->makeTransaction(WTF::move(pendingTransaction))), protectedThis->m_surfaceID);
    });
}

void RemoteCoordinatedCompositorProxy::suspend()
{
    if (++m_suspendedCount > 1)
        return;

    if (RefPtr connection = gpuConnection())
        connection->send(Messages::RemoteCoordinatedCompositor::SetVisible(false), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::resume()
{
    ASSERT(m_suspendedCount);
    if (--m_suspendedCount)
        return;

    if (RefPtr connection = gpuConnection())
        connection->send(Messages::RemoteCoordinatedCompositor::SetVisible(true), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::fillGLInformation(RenderProcessInfo&& info, CompletionHandler<void(RenderProcessInfo&&)>&& completionHandler)
{
    completionHandler(WTF::move(info));
}

void RemoteCoordinatedCompositorProxy::gpuProcessConnectionDidClose(GPUProcessConnection& gpuProcessConnection)
{
    ASSERT(RunLoop::isMain());
    gpuProcessConnection.messageReceiverMap().removeMessageReceiver(Messages::RemoteCoordinatedCompositorProxy::messageReceiverName(), m_surfaceID);
    {
        Locker locker { m_connectionLock };
        m_gpuConnection = nullptr;
    }
    {
        Locker locker { m_videoFrameSlotsLock };
        m_videoFrameSlots.clear();
        m_removedVideoFrameSlots.clear();
    }

    m_pendingCommits = 0;
    didCompositeRenderingUpdate();

    RefPtr webPage = m_webPage;
    if (m_isInvalidated || !webPage)
        return;

    {
        Locker locker { m_commitLock };
        m_needsFullCommit = true;
        m_sceneState->forEachLayer([](CoordinatedPlatformLayer& layer) {
            layer.markAllPropertiesChanged();
        });
    }

    connectToGPUProcess(*webPage);
#if ENABLE(DAMAGE_TRACKING)
    sendDamageSettings();
#endif
    if (m_suspendedCount) {
        if (RefPtr connection = gpuConnection())
            connection->send(Messages::RemoteCoordinatedCompositor::SetVisible(false), m_surfaceID);
    }
    if (RefPtr drawingArea = webPage->drawingArea())
        drawingArea->triggerRenderingUpdate();
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
