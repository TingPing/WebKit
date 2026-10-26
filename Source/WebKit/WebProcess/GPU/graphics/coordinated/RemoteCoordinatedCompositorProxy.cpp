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
#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurface.h"
#include "AcceleratedSurfaceMessages.h"
#include "CoordinatedLayerTreeTransaction.h"
#include "CoordinatedSceneState.h"
#include "GPUConnectionToWebProcessMessages.h"
#include "HandleMessage.h"
#include "RemoteCoordinatedCompositorMessages.h"
#include "RenderProcessInfo.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/NativeImage.h>
#include <WebCore/SharedBuffer.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
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

static CoordinatedBackingStoreUpdate backingStoreUpdate(const CoordinatedBackingStoreProxy::Update& update, float contentsScale, SharedBufferBuilder& pixels)
{
    CoordinatedBackingStoreUpdate backingStore {
        .contentsScale = contentsScale,
        .tilesToCreate = update.tilesToCreate(),
        .tilesToUpdate = { },
        .tilesToRemove = update.tilesToRemove(),
    };

    for (const auto& tileUpdate : update.tilesToUpdate()) {
        Ref buffer = tileUpdate.buffer;
        buffer->waitUntilPaintingFinished();
        if (buffer->paintingState() != CoordinatedTileBuffer::PaintingState::Complete || buffer->isBackedByOpenGL())
            continue;

        auto data = static_cast<CoordinatedUnacceleratedTileBuffer&>(buffer.get()).takeData();
        if (data.isEmpty())
            continue;

        backingStore.tilesToUpdate.append({
            .tileID = tileUpdate.tileID,
            .tileRect = tileUpdate.tileRect,
            .dirtyRect = tileUpdate.dirtyRect,
            .supportsAlpha = buffer->supportsAlpha(),
            .pixels = appendPixels(pixels, WTF::move(data), buffer->size()),
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

static CoordinatedLayerTreeTransaction makeTransaction(PendingLayerTreeTransaction&& pendingTransaction)
{
    CoordinatedLayerTreeTransaction transaction {
        .rootLayer = pendingTransaction.rootLayer,
        .layers = { },
        .removedLayers = WTF::move(pendingTransaction.removedLayers),
        .pixels = { },
    };

    SharedBufferBuilder pixels;
    for (auto& changes : pendingTransaction.layers) {
        CoordinatedLayerUpdate update { .properties = WTF::move(changes.properties), .backingStore = { }, .contentsImage = { }, .contentsBuffer = { } };
        if (changes.backingStoreUpdate)
            update.backingStore = backingStoreUpdate(*changes.backingStoreUpdate, changes.contentsScale, pixels);
        if (RefPtr image = changes.contentsImage)
            update.contentsImage = imagePixels(*image, pixels);
        if (RefPtr image = changes.contentsBufferImage)
            update.contentsBuffer = imagePixels(*image, pixels);
        transaction.layers.append(WTF::move(update));
    }

    if (!pixels.isEmpty())
        transaction.pixels = IPC::SharedBufferReference(pixels.takeBuffer());

    return transaction;
}

Ref<RemoteCoordinatedCompositorProxy> RemoteCoordinatedCompositorProxy::create(WebPage& webPage, CoordinatedSceneState& sceneState)
{
    return adoptRef(*new RemoteCoordinatedCompositorProxy(webPage, sceneState));
}

RemoteCoordinatedCompositorProxy::RemoteCoordinatedCompositorProxy(WebPage& webPage, CoordinatedSceneState& sceneState)
    : m_webPage(webPage)
    , m_sceneState(sceneState)
    , m_surfaceID(AcceleratedSurface::generateSurfaceID())
    , m_commitQueue(WorkQueue::create("org.webkit.RemoteCoordinatedCompositorCommit"_s))
    , m_relayQueue(WorkQueue::create("org.webkit.RemoteCoordinatedCompositorRelay"_s))
{
    ASSERT(RunLoop::isMain());

    Ref gpuProcessConnection = WebProcess::singleton().ensureGPUProcessConnection();
    gpuProcessConnection->addClient(*this);
    Ref connection = gpuProcessConnection->connection();
    {
        Locker locker { m_connectionLock };
        m_gpuConnection = connection.ptr();
    }

    connection->addMessageReceiver(m_relayQueue.get(), *this, Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
    WebProcess::singleton().parentProcessConnection()->addMessageReceiver(m_relayQueue.get(), *this, Messages::AcceleratedSurface::messageReceiverName(), m_surfaceID);

    connection->send(Messages::GPUConnectionToWebProcess::CreateCoordinatedCompositor(m_surfaceID, webPage.size(), webPage.deviceScaleFactor(), webPage.effectiveBackgroundColor(),
        webPage.activityState().contains(ActivityState::IsVisible), webPage.preferredBufferFormats(), WebProcess::singleton().rendererBufferTransportMode().contains(RendererBufferTransportMode::Hardware)), 0);
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

    WebProcess::singleton().parentProcessConnection()->removeMessageReceiver(Messages::AcceleratedSurface::messageReceiverName(), m_surfaceID);

    RefPtr connection = gpuConnection();
    if (!connection)
        return;

    connection->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
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

    Locker locker { m_commitLock };
    PendingLayerTreeTransaction pendingTransaction { .rootLayer = m_sceneState->rootLayer().id(), .removedLayers = { }, .layers = { } };
    for (auto& layer : m_sceneState->takePendingLayersToRemove())
        pendingTransaction.removedLayers.append(layer->id());
    m_sceneState->forEachLayer([&](CoordinatedPlatformLayer& layer) {
        if (auto changes = layer.takePendingChanges())
            pendingTransaction.layers.append(WTF::move(*changes));
    });

    ++m_pendingCommits;
    m_commitQueue->dispatch([protectedThis = Ref { *this }, pendingTransaction = WTF::move(pendingTransaction)] mutable {
        auto transaction = makeTransaction(WTF::move(pendingTransaction));
        RefPtr connection = protectedThis->gpuConnection();
        if (!connection) {
            callOnMainRunLoop([protectedThis = WTF::move(protectedThis)] {
                protectedThis->didComposite();
            });
            return;
        }

        connection->sendWithAsyncReply(Messages::RemoteCoordinatedCompositor::Commit(WTF::move(transaction)), [protectedThis] {
            protectedThis->didComposite();
        }, protectedThis->m_surfaceID);
    });
}

void RemoteCoordinatedCompositorProxy::didComposite()
{
    ASSERT(RunLoop::isMain());
    if (m_pendingCommits)
        --m_pendingCommits;
    if (auto didCompositeFunction = std::exchange(m_didCompositeFunction, nullptr))
        didCompositeFunction();
}

void RemoteCoordinatedCompositorProxy::requestComposition(CompositionReason reason)
{
    if (reason != CompositionReason::AsyncScrolling)
        return;

    Locker locker { m_commitLock };
    PendingLayerTreeTransaction pendingTransaction { .rootLayer = m_sceneState->rootLayer().id(), .removedLayers = { }, .layers = { } };
    m_sceneState->forEachLayer([&](CoordinatedPlatformLayer& layer) {
        if (auto changes = layer.takePendingScrollingChanges())
            pendingTransaction.layers.append(WTF::move(*changes));
    });
    if (pendingTransaction.layers.isEmpty())
        return;

    m_commitQueue->dispatch([protectedThis = Ref { *this }, pendingTransaction = WTF::move(pendingTransaction)] mutable {
        if (RefPtr connection = protectedThis->gpuConnection())
            connection->send(Messages::RemoteCoordinatedCompositor::UpdateLayers(makeTransaction(WTF::move(pendingTransaction))), protectedThis->m_surfaceID);
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

void RemoteCoordinatedCompositorProxy::gpuProcessConnectionDidClose(GPUProcessConnection&)
{
    {
        Locker locker { m_connectionLock };
        m_gpuConnection = nullptr;
    }

    // FIXME: recreate the compositor in a new GPU process.
    m_pendingCommits = 0;
    if (auto didCompositeFunction = std::exchange(m_didCompositeFunction, nullptr))
        didCompositeFunction();
}

void RemoteCoordinatedCompositorProxy::didReceiveMessage(IPC::Connection& connection, IPC::Decoder& decoder)
{
    if (decoder.messageName() == Messages::AcceleratedBackingStore::DidCreateDMABufBuffer::name()) {
        IPC::handleMessage<Messages::AcceleratedBackingStore::DidCreateDMABufBuffer>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::didCreateDMABufBuffer);
        return;
    }
    if (decoder.messageName() == Messages::AcceleratedBackingStore::DidCreateSHMBuffer::name()) {
        IPC::handleMessage<Messages::AcceleratedBackingStore::DidCreateSHMBuffer>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::didCreateSHMBuffer);
        return;
    }
    if (decoder.messageName() == Messages::AcceleratedBackingStore::DidDestroyBuffer::name()) {
        IPC::handleMessage<Messages::AcceleratedBackingStore::DidDestroyBuffer>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::didDestroyBuffer);
        return;
    }
    if (decoder.messageName() == Messages::AcceleratedBackingStore::Frame::name()) {
        IPC::handleMessage<Messages::AcceleratedBackingStore::Frame>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::frame);
        return;
    }
#if ENABLE(WPE_PLATFORM)
    if (decoder.messageName() == Messages::AcceleratedBackingStore::DidChangeBufferConfiguration::name()) {
        IPC::handleMessage<Messages::AcceleratedBackingStore::DidChangeBufferConfiguration>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::didChangeBufferConfiguration);
        return;
    }
#endif
    if (decoder.messageName() == Messages::AcceleratedSurface::ReleaseBuffer::name()) {
        IPC::handleMessage<Messages::AcceleratedSurface::ReleaseBuffer>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::releaseBuffer);
        return;
    }
    if (decoder.messageName() == Messages::AcceleratedSurface::FrameDone::name()) {
        IPC::handleMessage<Messages::AcceleratedSurface::FrameDone>(connection, decoder, this, &RemoteCoordinatedCompositorProxy::frameDone);
        return;
    }
    decoder.markInvalid();
}

void RemoteCoordinatedCompositorProxy::didCreateDMABufBuffer(uint64_t id, DMABufBufferAttributes&& attributes, RendererBufferFormat::Usage usage)
{
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateDMABufBuffer(id, WTF::move(attributes), usage), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::didCreateSHMBuffer(uint64_t id, ShareableBitmapHandle&& handle)
{
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateSHMBuffer(id, WTF::move(handle)), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::didDestroyBuffer(uint64_t id)
{
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidDestroyBuffer(id), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::frame(uint64_t id, Vector<IntRect, 1>&& damage, UnixFileDescriptor&& renderingFence)
{
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::Frame(id, WTF::move(damage), WTF::move(renderingFence)), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::didChangeBufferConfiguration(uint32_t bufferCount)
{
#if ENABLE(WPE_PLATFORM)
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidChangeBufferConfiguration(bufferCount), m_surfaceID);
#else
    UNUSED_PARAM(bufferCount);
#endif
}

void RemoteCoordinatedCompositorProxy::releaseBuffer(uint64_t id, UnixFileDescriptor&& releaseFence)
{
    if (RefPtr connection = gpuConnection())
        connection->send(Messages::AcceleratedSurface::ReleaseBuffer(id, WTF::move(releaseFence)), m_surfaceID);
}

void RemoteCoordinatedCompositorProxy::frameDone()
{
    if (RefPtr connection = gpuConnection())
        connection->send(Messages::AcceleratedSurface::FrameDone(), m_surfaceID);
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
