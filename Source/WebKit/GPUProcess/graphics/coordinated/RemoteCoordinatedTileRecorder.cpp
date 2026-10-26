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
#include "RemoteCoordinatedTileRecorder.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "RemoteCoordinatedCompositor.h"
#include "RemoteGraphicsContextMessages.h"
#include <WebCore/GraphicsContextSkia.h>
#include <WebCore/SkiaRecordingResult.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkBBHFactory.h>
#include <skia/core/SkPictureRecorder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

struct RemoteCoordinatedTileRecorder::Recording {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Recording);

    SkPictureRecorder pictureRecorder;
    std::unique_ptr<GraphicsContextSkia> context;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(RemoteCoordinatedTileRecorder::Recording);

Ref<RemoteCoordinatedTileRecorder> RemoteCoordinatedTileRecorder::create(RemoteGraphicsContextIdentifier identifier, RemoteRenderingBackend& renderingBackend, RefPtr<RemoteCoordinatedCompositor>&& compositor, const IntRect& recordRect, unsigned dirtyTilesCount, bool contentsOpaque, float contentsScale)
{
    // Images on the GPU are drawn through promise images of the compositor's context.
    auto threadSafeGrContext = compositor ? compositor->threadSafeGrContextForRecording() : nullptr;
    auto renderingMode = threadSafeGrContext ? RenderingMode::Accelerated : RenderingMode::Unaccelerated;

    auto recording = makeUnique<Recording>();
    // Use a bounding box hierarchy when the picture is replayed into more than one tile,
    // so that every playback uses only the operations intersecting the tile.
    SkRTreeFactory rtreeFactory;
    auto* canvas = recording->pictureRecorder.beginRecording(recordRect.width(), recordRect.height(), dirtyTilesCount > 1 ? &rtreeFactory : nullptr);
    recording->context = makeUnique<GraphicsContextSkia>(*canvas, renderingMode, RenderingPurpose::LayerBacking);
    recording->context->beginRecording(GraphicsContextSkia::RecordingMode::Tile, threadSafeGrContext);

    Ref instance = adoptRef(*new RemoteCoordinatedTileRecorder(WTF::move(recording), identifier, renderingBackend, WTF::move(compositor), recordRect, renderingMode, contentsOpaque, contentsScale));
    instance->startListeningForIPC();
    return instance;
}

RemoteCoordinatedTileRecorder::RemoteCoordinatedTileRecorder(std::unique_ptr<Recording>&& recording, RemoteGraphicsContextIdentifier identifier, RemoteRenderingBackend& renderingBackend, RefPtr<RemoteCoordinatedCompositor>&& compositor, const IntRect& recordRect, RenderingMode renderingMode, bool contentsOpaque, float contentsScale)
    : RemoteGraphicsContext(*recording->context, renderingBackend)
    , m_recording(WTF::move(recording))
    , m_identifier(identifier)
    , m_compositor(WTF::move(compositor))
    , m_recordRect(recordRect)
    , m_renderingMode(renderingMode)
    , m_contentsOpaque(contentsOpaque)
    , m_contentsScale(contentsScale)
{
}

RemoteCoordinatedTileRecorder::~RemoteCoordinatedTileRecorder() = default;

void RemoteCoordinatedTileRecorder::startListeningForIPC()
{
    m_renderingBackend->streamConnection().startReceivingMessages(*this, Messages::RemoteGraphicsContext::messageReceiverName(), m_identifier.toUInt64());
}

void RemoteCoordinatedTileRecorder::stopListeningForIPC()
{
    m_renderingBackend->streamConnection().stopReceivingMessages(Messages::RemoteGraphicsContext::messageReceiverName(), m_identifier.toUInt64());
}

Ref<SkiaRecordingResult> RemoteCoordinatedTileRecorder::finishRecording()
{
    auto recordingData = m_recording->context->endRecording();
    auto picture = m_recording->pictureRecorder.finishRecordingAsPicture();
    return SkiaRecordingResult::create(WTF::move(picture), WTF::move(recordingData), m_recordRect, m_renderingMode, m_contentsOpaque, m_contentsScale);
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
