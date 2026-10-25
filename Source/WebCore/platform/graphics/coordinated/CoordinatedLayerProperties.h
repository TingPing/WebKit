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

#if USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "AcceleratedAnimations.h"
#include "Color.h"
#include "FilterOperations.h"
#include "FloatPoint.h"
#include "FloatPoint3D.h"
#include "FloatRect.h"
#include "FloatRoundedRect.h"
#include "FloatSize.h"
#include "GraphicsTypes.h"
#include "Path.h"
#include "PlatformLayerIdentifier.h"
#include "TransformationMatrix.h"
#include "WindRule.h"
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace WebCore {

// The bits follow the order of the members they guard, which serialization requires.
enum class CoordinatedLayerChange : uint64_t {
    Position                     = 1LLU << 0,
    BoundsOrigin                 = 1LLU << 1,
    AnchorPoint                  = 1LLU << 2,
    Size                         = 1LLU << 3,
    Transform                    = 1LLU << 4,
    ChildrenTransform            = 1LLU << 5,
    Preserves3D                  = 1LLU << 6,
    MasksToBounds                = 1LLU << 7,
    BackfaceVisibility           = 1LLU << 8,
    BackgroundColor              = 1LLU << 9,
    Opacity                      = 1LLU << 10,
    BlendMode                    = 1LLU << 11,
    BackingStore                 = 1LLU << 12,
    ContentsVisible              = 1LLU << 13,
    ContentsOpaque               = 1LLU << 14,
    ContentsRect                 = 1LLU << 15,
    ContentsRectClipsDescendants = 1LLU << 16,
    ContentsClippingRect         = 1LLU << 17,
    ContentsClipShapePath        = 1LLU << 18,
    ContentsTileSize             = 1LLU << 19,
    ContentsTilePhase            = 1LLU << 20,
    ContentsColor                = 1LLU << 21,
    ClipPath                     = 1LLU << 22,
    ClipPathWindRule             = 1LLU << 23,
    Filters                      = 1LLU << 24,
    Mask                         = 1LLU << 25,
    Replica                      = 1LLU << 26,
    BackdropFilters              = 1LLU << 27,
    BackdropRect                 = 1LLU << 28,
    BackdropShapePath            = 1LLU << 29,
    BackdropRoot                 = 1LLU << 30,
    DebugBorderColor             = 1LLU << 31,
    DebugBorderWidth             = 1LLU << 32,
    RepaintCount                 = 1LLU << 33,
    Children                     = 1LLU << 34,
    Animations                   = 1LLU << 35,
    ContentsImage                = 1LLU << 36,
};

struct CoordinatedLayerProperties {
    PlatformLayerIdentifier id;
    OptionSet<CoordinatedLayerChange> changes { };
    FloatPoint position { };
    FloatPoint boundsOrigin { };
    FloatPoint3D anchorPoint { };
    FloatSize size { };
    TransformationMatrix transform { };
    TransformationMatrix childrenTransform { };
    bool preserves3D { false };
    bool masksToBounds { false };
    bool backfaceVisibility { false };
    Color backgroundColor { };
    float opacity { 0 };
    BlendMode blendMode { BlendMode::Normal };
    bool hasBackingStore { false };
    bool contentsVisible { false };
    bool contentsOpaque { false };
    FloatRect contentsRect { };
    bool contentsRectClipsDescendants { false };
    FloatRoundedRect contentsClippingRect { };
    Path contentsClipShapePath { };
    FloatSize contentsTileSize { };
    FloatSize contentsTilePhase { };
    Color contentsColor { };
    Path clipPath { };
    WindRule clipPathWindRule { WindRule::NonZero };
    FilterOperations filters { };
    std::optional<PlatformLayerIdentifier> mask { };
    std::optional<PlatformLayerIdentifier> replica { };
    FilterOperations backdropFilters { };
    FloatRoundedRect backdropRect { };
    Path backdropShapePath { };
    bool isBackdropRoot { false };
    Color debugBorderColor { };
    std::optional<float> debugBorderWidth { };
    std::optional<unsigned> repaintCount { };
    Vector<PlatformLayerIdentifier> children { };
    AcceleratedAnimations animations { };
};

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
