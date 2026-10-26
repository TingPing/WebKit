/*
 * Copyright (C) 2022 Metrological Group B.V.
 * Copyright (C) 2022 Igalia S.L.
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
#include "RemoteGraphicsContextGL.h"

#if ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)

#include "GPUConnectionToWebProcess.h"
#include "RemoteCoordinatedContentsRegistry.h"
#include <wtf/HashSet.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/TZoneMallocInlines.h>

#if USE(SKIA) && !USE(TEXTURE_MAPPER)
#include <WebCore/GLFence.h>
#include <WebCore/GraphicsContextGLCoordinated.h>
#endif

namespace WebKit {

class RemoteGraphicsContextGLGBM final : public RemoteGraphicsContextGL {
    WTF_MAKE_TZONE_ALLOCATED(RemoteGraphicsContextGLGBM);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(RemoteGraphicsContextGLGBM);

public:
    RemoteGraphicsContextGLGBM(GPUConnectionToWebProcess&, RemoteGraphicsContextGLIdentifier, RemoteRenderingBackend&, Ref<IPC::StreamServerConnection>&&);
    ~RemoteGraphicsContextGLGBM();

private:
    void platformWorkQueueInitialize(WebCore::GraphicsContextGLAttributes&&) final;
    void didRunOutOfMessages() final;
    void prepareForDisplay(Vector<uint64_t>&&, std::optional<WebCore::CoordinatedContentsIdentifier>&&, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&, Vector<uint64_t>&&)>&&) final;

    WebCore::GraphicsContextGLGBM& gbmContext() WTF_REQUIRES_CAPABILITY(workQueue()) { return static_cast<WebCore::GraphicsContextGLGBM&>(*m_context); }

    const WebCore::ProcessIdentifier m_webProcessIdentifier;
    std::optional<WebCore::CoordinatedContentsIdentifier> m_contentsIdentifier;
    HashSet<Ref<WebCore::DMABufBuffer>> m_inUseBuffers WTF_GUARDED_BY_CAPABILITY(workQueue());
    HashSet<uint64_t> m_sentBuffers WTF_GUARDED_BY_CAPABILITY(workQueue());
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteGraphicsContextGLGBM);

RemoteGraphicsContextGLGBM::RemoteGraphicsContextGLGBM(GPUConnectionToWebProcess& connection, RemoteGraphicsContextGLIdentifier identifier, RemoteRenderingBackend& renderingBackend, Ref<IPC::StreamServerConnection>&& streamConnection)
    : RemoteGraphicsContextGL(connection, identifier, renderingBackend, WTF::move(streamConnection))
    , m_webProcessIdentifier(connection.webProcessIdentifier())
{ }

RemoteGraphicsContextGLGBM::~RemoteGraphicsContextGLGBM()
{
#if USE(SKIA) && !USE(TEXTURE_MAPPER)
    if (m_contentsIdentifier)
        RemoteCoordinatedContentsRegistry::singleton().remove(m_webProcessIdentifier, *m_contentsIdentifier);
#endif
}

static WebCore::DMABufBuffer::Attributes duplicateAttributes(const WebCore::DMABufBuffer::Attributes& attributes)
{
    return { attributes.size, attributes.fourcc, WTF::map(attributes.fds, [](const auto& fd) {
        return fd.duplicate();
    }), attributes.offsets, attributes.strides, attributes.modifier };
}

void RemoteGraphicsContextGLGBM::didRunOutOfMessages()
{
    assertIsCurrent(workQueue());
    if (RefPtr context = m_context)
        context->flush();
}

void RemoteGraphicsContextGLGBM::platformWorkQueueInitialize(WebCore::GraphicsContextGLAttributes&& attributes)
{
    assertIsCurrent(workQueue());
    m_context = WebCore::GraphicsContextGLGBM::create(WTF::move(attributes));
}

void RemoteGraphicsContextGLGBM::prepareForDisplay(Vector<uint64_t>&& inUseBuffers, std::optional<WebCore::CoordinatedContentsIdentifier>&& contentsIdentifier, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&, Vector<uint64_t>&&)>&& completionHandler)
{
    assertIsCurrent(workQueue());

    m_inUseBuffers.removeIf([&](const auto& buffer) {
        return !inUseBuffers.contains(buffer->id());
    });

    Ref context = gbmContext();
    auto drawingBufferIDs = context->drawingBufferIDs();
    m_sentBuffers.removeIf([&](auto id) {
        return !drawingBufferIDs.contains(id);
    });

    UnixFileDescriptor fenceFD;
    context->prepareForDisplayWithFinishedSignal([&context, &fenceFD] {
        fenceFD = context->createExportedFence();
        if (!fenceFD)
            context->flush();
    });

    RefPtr dmabuf = context->displayBufferDMABuf();
    if (!dmabuf) {
        completionHandler(0, std::nullopt, { }, context->drawingBufferIDs());
        return;
    }

    m_inUseBuffers.add(Ref { *dmabuf });

#if USE(SKIA) && !USE(TEXTURE_MAPPER)
    if (contentsIdentifier) {
        m_contentsIdentifier = *contentsIdentifier;
        RemoteCoordinatedContentsRegistry::singleton().setFrame(m_webProcessIdentifier, *contentsIdentifier, RemoteCoordinatedContentsRegistry::DMABufFrame { *dmabuf, fenceFD.duplicate() });
    }
#else
    UNUSED_PARAM(contentsIdentifier);
#endif

    std::optional<WebCore::DMABufBuffer::Attributes> attributes;
    if (m_sentBuffers.add(dmabuf->id()).isNewEntry)
        attributes = duplicateAttributes(dmabuf->attributes());

    completionHandler(dmabuf->id(), WTF::move(attributes), WTF::move(fenceFD), context->drawingBufferIDs());
}

#if USE(SKIA) && !USE(TEXTURE_MAPPER)
// Used when the GPU can't export DMA-BUFs, such as with software rendering. The compositor in the
// GPU process shares the GL textures directly, so this only works when compositing there.
class RemoteGraphicsContextGLCoordinated final : public RemoteGraphicsContextGL {
    WTF_MAKE_TZONE_ALLOCATED(RemoteGraphicsContextGLCoordinated);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(RemoteGraphicsContextGLCoordinated);

public:
    RemoteGraphicsContextGLCoordinated(GPUConnectionToWebProcess&, RemoteGraphicsContextGLIdentifier, RemoteRenderingBackend&, Ref<IPC::StreamServerConnection>&&);
    ~RemoteGraphicsContextGLCoordinated();

private:
    void platformWorkQueueInitialize(WebCore::GraphicsContextGLAttributes&&) final;
    void didRunOutOfMessages() final;
    void prepareForDisplay(Vector<uint64_t>&&, std::optional<WebCore::CoordinatedContentsIdentifier>&&, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&, Vector<uint64_t>&&)>&&) final;

    WebCore::GraphicsContextGLCoordinated& coordinatedContext() WTF_REQUIRES_CAPABILITY(workQueue()) { return static_cast<WebCore::GraphicsContextGLCoordinated&>(*m_context); }

    const WebCore::ProcessIdentifier m_webProcessIdentifier;
    std::optional<WebCore::CoordinatedContentsIdentifier> m_contentsIdentifier;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteGraphicsContextGLCoordinated);

RemoteGraphicsContextGLCoordinated::RemoteGraphicsContextGLCoordinated(GPUConnectionToWebProcess& connection, RemoteGraphicsContextGLIdentifier identifier, RemoteRenderingBackend& renderingBackend, Ref<IPC::StreamServerConnection>&& streamConnection)
    : RemoteGraphicsContextGL(connection, identifier, renderingBackend, WTF::move(streamConnection))
    , m_webProcessIdentifier(connection.webProcessIdentifier())
{ }

RemoteGraphicsContextGLCoordinated::~RemoteGraphicsContextGLCoordinated()
{
    if (m_contentsIdentifier)
        RemoteCoordinatedContentsRegistry::singleton().remove(m_webProcessIdentifier, *m_contentsIdentifier);
}

void RemoteGraphicsContextGLCoordinated::didRunOutOfMessages()
{
    assertIsCurrent(workQueue());
    if (RefPtr context = m_context)
        context->flush();
}

void RemoteGraphicsContextGLCoordinated::platformWorkQueueInitialize(WebCore::GraphicsContextGLAttributes&& attributes)
{
    assertIsCurrent(workQueue());
    m_context = WebCore::GraphicsContextGLCoordinated::create(WTF::move(attributes));
}

void RemoteGraphicsContextGLCoordinated::prepareForDisplay(Vector<uint64_t>&&, std::optional<WebCore::CoordinatedContentsIdentifier>&& contentsIdentifier, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&, Vector<uint64_t>&&)>&& completionHandler)
{
    assertIsCurrent(workQueue());

    auto displayTexture = coordinatedContext().prepareDisplayTexture();
    if (!displayTexture || !contentsIdentifier) {
        completionHandler(0, std::nullopt, { }, { });
        return;
    }

    uint64_t textureID = displayTexture->texture->id();
    m_contentsIdentifier = *contentsIdentifier;
    RemoteCoordinatedContentsRegistry::singleton().setFrame(m_webProcessIdentifier, *contentsIdentifier, WTF::move(*displayTexture));
    completionHandler(textureID, std::nullopt, { }, { });
}
#endif

Ref<RemoteGraphicsContextGL> RemoteGraphicsContextGL::create(GPUConnectionToWebProcess& connection, WebCore::GraphicsContextGLAttributes&& attributes, RemoteGraphicsContextGLIdentifier identifier, RemoteRenderingBackend& renderingBackend, Ref<IPC::StreamServerConnection>&& streamConnection)
{
#if USE(SKIA) && !USE(TEXTURE_MAPPER)
    if (!WebCore::GraphicsContextGLGBM::checkRequirements()) {
        Ref<RemoteGraphicsContextGL> instance = adoptRef(*new RemoteGraphicsContextGLCoordinated(connection, identifier, renderingBackend, WTF::move(streamConnection)));
        instance->initialize(WTF::move(attributes));
        return instance;
    }
#endif
    Ref<RemoteGraphicsContextGL> instance = adoptRef(*new RemoteGraphicsContextGLGBM(connection, identifier, renderingBackend, WTF::move(streamConnection)));
    instance->initialize(WTF::move(attributes));
    return instance;
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)
