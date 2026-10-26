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
#include "SharedBufferReference.h"
#include <WebCore/CoordinatedLayerProperties.h>
#include <WebCore/CoordinatedPlatformLayerBuffer.h>
#include <WebCore/IntRect.h>
#include <WebCore/SharedMemory.h>
#include <optional>
#include <wtf/Vector.h>

namespace WebKit {

// Premultiplied BGRA pixels with a stride of four times the width, stored in the transaction pixels buffer.
struct CoordinatedPixels {
    uint64_t offset { 0 };
    WebCore::IntSize size;
};

// The GPU process paints the tile from the recording the web process made with this identifier.
struct CoordinatedTileUpdate {
    uint32_t tileID { 0 };
    WebCore::IntRect tileRect;
    WebCore::IntRect dirtyRect;
    uint64_t recording { 0 };
};

struct CoordinatedBackingStoreUpdate {
    float contentsScale { 1 };
    Vector<uint32_t> tilesToCreate;
    Vector<CoordinatedTileUpdate> tilesToUpdate;
    Vector<uint32_t> tilesToRemove;
};

// A video frame in system memory, copied into a shared memory slot that the GPU process maps once.
// The format is a GstVideoFormat, and the offsets and strides are those of each plane in the slot.
struct CoordinatedVideoFrame {
    CoordinatedVideoFrameSlotIdentifier slot;
    std::optional<WebCore::SharedMemory::Handle> slotHandle;
    WebCore::IntSize presentationSize;
    uint32_t format { 0 };
    WebCore::IntSize size;
    Vector<uint64_t> offsets;
    Vector<int32_t> strides;
    uint8_t colorRange { 0 };
    uint8_t colorMatrix { 0 };
    uint8_t colorTransfer { 0 };
    uint8_t colorPrimaries { 0 };
    WebCore::CoordinatedPlatformLayerBuffer::Rotation rotation { WebCore::CoordinatedPlatformLayerBuffer::Rotation::None };
};

struct CoordinatedLayerUpdate {
    WebCore::CoordinatedLayerProperties properties;
    std::optional<CoordinatedBackingStoreUpdate> backingStore;
    std::optional<CoordinatedPixels> contentsImage;
    std::optional<CoordinatedPixels> contentsBuffer;
    std::optional<CoordinatedVideoFrame> contentsVideoFrame;
};

struct CoordinatedLayerTreeTransaction {
    WebCore::PlatformLayerIdentifier rootLayer;
    Vector<CoordinatedLayerUpdate> layers;
    Vector<WebCore::PlatformLayerIdentifier> removedLayers;
    IPC::SharedBufferReference pixels;
    Vector<CoordinatedVideoFrameSlotIdentifier> removedVideoFrameSlots;
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
