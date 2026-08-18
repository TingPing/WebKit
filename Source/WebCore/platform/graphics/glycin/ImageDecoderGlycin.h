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

#pragma once

#if USE(GLYCIN)

#include "ImageDecoder.h"
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/glib/GRefPtr.h>

typedef struct _GBytes GBytes;
typedef struct _GlyImage GlyImage;

namespace WebCore {

class SharedBuffer;

class ImageDecoderGlycin final : public ImageDecoder {
    WTF_MAKE_TZONE_ALLOCATED(ImageDecoderGlycin);
    WTF_MAKE_NONCOPYABLE(ImageDecoderGlycin);
public:
    static Ref<ImageDecoderGlycin> create(FragmentedSharedBuffer&, const String& mimeType, AlphaOption, GammaAndColorProfileOption);
    ImageDecoderGlycin(AlphaOption, GammaAndColorProfileOption);
    ~ImageDecoderGlycin();

    static bool supportsMediaType(MediaType type) { return type == MediaType::Image; }

    size_t bytesDecodedToDetermineProperties() const final { return 0; }

    EncodedDataStatus encodedDataStatus() const final;
    IntSize size() const final;
    size_t frameCount() const final;
    RepetitionCount repetitionCount() const final;
    String filenameExtension() const final;
    std::optional<IntPoint> hotSpot() const final { return std::nullopt; }

    IntSize frameSizeAtIndex(size_t, SubsamplingLevel = SubsamplingLevel::Default) const final;
    bool frameIsCompleteAtIndex(size_t) const final;
    ImageOrientation frameOrientationAtIndex(size_t) const final;
    Seconds frameDurationAtIndex(size_t) const final;
    bool frameHasAlphaAtIndex(size_t) const final;

    PlatformImagePtr createFrameImageAtIndex(size_t, SubsamplingLevel = SubsamplingLevel::Default, const DecodingOptions& = DecodingOptions(DecodingMode::Synchronous)) final;

    void setData(const FragmentedSharedBuffer&, bool allDataReceived) final;
    bool isAllDataReceived() const final;
    void clearFrameBufferCache(size_t) final;

private:
    struct Frame {
        bool isValid() const { return !size.isEmpty(); }

        IntSize size;
        Seconds duration;
        bool hasAlpha { true };
        PlatformImagePtr image;
    };

    bool failed() const WTF_REQUIRES_LOCK(m_lock) { return m_encodedDataStatus == EncodedDataStatus::Error; }
    bool setFailed() WTF_REQUIRES_LOCK(m_lock);
    void resetImage() WTF_REQUIRES_LOCK(m_lock);
    bool ensureImage() WTF_REQUIRES_LOCK(m_lock);
    bool decodeMetadata() WTF_REQUIRES_LOCK(m_lock);
    void didFindLastFrame(size_t frameCount) WTF_REQUIRES_LOCK(m_lock);
    bool decodeNextFrame(bool retainImage) WTF_REQUIRES_LOCK(m_lock);
    size_t frameCountWithLock() WTF_REQUIRES_LOCK(m_lock);
    const Frame* frameWithLock(size_t) const WTF_REQUIRES_LOCK(m_lock);
    const Frame* decodedFrameWithLock(size_t) WTF_REQUIRES_LOCK(m_lock);

    mutable Lock m_lock;
    RefPtr<const SharedBuffer> m_data WTF_GUARDED_BY_LOCK(m_lock);
    GRefPtr<GlyImage> m_image WTF_GUARDED_BY_LOCK(m_lock);
    GRefPtr<GBytes> m_firstFrameBytes WTF_GUARDED_BY_LOCK(m_lock);
    Vector<Frame, 1> m_frames WTF_GUARDED_BY_LOCK(m_lock);
    std::optional<size_t> m_totalFrameCount WTF_GUARDED_BY_LOCK(m_lock);
    std::optional<RepetitionCount> m_repetitionCount WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_nextFrameIndex WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    size_t m_metadataAttemptThreshold WTF_GUARDED_BY_LOCK(m_lock) { 1024 };
    EncodedDataStatus m_encodedDataStatus WTF_GUARDED_BY_LOCK(m_lock) { EncodedDataStatus::TypeAvailable };
    IntSize m_size WTF_GUARDED_BY_LOCK(m_lock);
    ImageOrientation m_orientation WTF_GUARDED_BY_LOCK(m_lock);
    String m_mimeType WTF_GUARDED_BY_LOCK(m_lock);
    bool m_isAllDataReceived WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_hasCompleteMetadata WTF_GUARDED_BY_LOCK(m_lock) { false };
    const bool m_premultiplyAlpha;
};

} // namespace WebCore

#endif // USE(GLYCIN)
