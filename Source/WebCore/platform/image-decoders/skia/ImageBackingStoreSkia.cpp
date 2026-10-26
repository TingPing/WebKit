/*
 * Copyright (C) 2024 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImageBackingStore.h"

#include "ShareableBitmap.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPixmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

namespace WebCore {

static std::atomic<bool> s_allocatesSharedMemory { false };
static Lock s_sharedPixelsLock;

static HashMap<const void*, ShareableBitmap*>& sharedPixels() WTF_REQUIRES_LOCK(s_sharedPixelsLock)
{
    static NeverDestroyed<HashMap<const void*, ShareableBitmap*>> sharedPixels;
    return sharedPixels;
}

namespace {
class SharedPixelsOwner {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(SharedPixelsOwner);
public:
    explicit SharedPixelsOwner(Ref<ShareableBitmap>&& bitmap)
        : m_bitmap(WTF::move(bitmap))
    {
        Locker locker { s_sharedPixelsLock };
        sharedPixels().add(m_bitmap->span().data(), m_bitmap.ptr());
    }

    ~SharedPixelsOwner()
    {
        Locker locker { s_sharedPixelsLock };
        sharedPixels().remove(m_bitmap->span().data());
    }

    std::span<const uint8_t> span() const { return m_bitmap->span(); }

private:
    const Ref<ShareableBitmap> m_bitmap;
};
}

void ImageBackingStore::setAllocatesSharedMemory(bool allocatesSharedMemory)
{
    s_allocatesSharedMemory = allocatesSharedMemory;
}

bool ImageBackingStore::allocateSharedPixels(const IntSize& size)
{
    if (!s_allocatesSharedMemory)
        return false;

    RefPtr bitmap = ShareableBitmap::create({ size });
    if (!bitmap || bitmap->bytesPerRow() != size.width() * sizeof(uint32_t))
        return false;

    auto pixels = bitmap->mutableSpan();
    auto owner = makeUnique<SharedPixelsOwner>(bitmap.releaseNonNull());
    m_pixels = FragmentedSharedBuffer::DataSegment::create(FragmentedSharedBuffer::DataSegment::Provider { [owner = WTF::move(owner)] {
        return owner->span();
    } });
    m_pixelsSpan = spanReinterpretCast<uint32_t>(pixels);
    return true;
}

RefPtr<ShareableBitmap> ImageBackingStore::shareableBitmap(const NativeImage& image)
{
    SkPixmap pixmap;
    const auto& platformImage = image.platformImage();
    if (!platformImage || !platformImage->peekPixels(&pixmap) || pixmap.colorType() != kN32_SkColorType || pixmap.alphaType() != kPremul_SkAlphaType)
        return nullptr;

    Locker locker { s_sharedPixelsLock };
    RefPtr bitmap = sharedPixels().get(pixmap.addr());
    if (!bitmap || bitmap->size() != IntSize(pixmap.width(), pixmap.height()))
        return nullptr;
    return bitmap;
}

PlatformImagePtr ImageBackingStore::image() const
{
    m_pixels->ref();
    auto info = SkImageInfo::MakeN32(size().width(), size().height(), m_premultiplyAlpha ? kPremul_SkAlphaType : kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, m_pixelsSpan.data(), info.minRowBytes64());
    return SkImages::RasterFromPixmap(pixmap, [](const void*, void* context) {
        static_cast<DataSegment*>(context)->deref();
    }, m_pixels.get());
}

} // namespace WebCore
