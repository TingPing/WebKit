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

#if ENABLE(IMAGE_PROCESS)

#include <WebCore/ImageDecoder.h>
#include <WebCore/ImageDecoderIdentifier.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/CompletionHandler.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Seconds.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

class RemoteImageDecoderManager;

// A WebCore::ImageDecoder that decodes in the Image process. Metadata (size,
// frame count, frame durations, ...) is mirrored from the Image process; it
// arrives asynchronously through RemoteImageDecoderManager pushes.
//
// No image data is sent over IPC, only shared buffers and notifications, and
// everything happens on the main run loop:
//   - setData() appends the new bytes to a shared memory buffer the Image
//     process decodes from in place; the resulting status is reported through
//     setEncodedDataStatusChangeCallback() once the Image process pushes it.
//   - decodeFrameAsynchronously() asks the Image process to decode a frame.
//     Each frame has a shared buffer, owned by the Image process, that the
//     frame is decoded into; frameDecoded() reports that the buffer is ready
//     and the frame is presented straight from that memory.
class RemoteImageDecoder final
    : public WebCore::ImageDecoder {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RemoteImageDecoder);
public:
    static RefPtr<RemoteImageDecoder> create(RemoteImageDecoderManager&, WebCore::ImageDecoderIdentifier, const String& mimeType);
    virtual ~RemoteImageDecoder();

    WebCore::ImageDecoderIdentifier identifier() const { return m_identifier; }

    // WebCore::ImageDecoder
    bool isDecodingAsynchronous() const final { return true; }

    size_t bytesDecodedToDetermineProperties() const override { return 0; }
    WebCore::EncodedDataStatus encodedDataStatus() const final;
    void setEncodedDataStatusChangeCallback(WTF::Function<void(WebCore::EncodedDataStatus)>&&) final;
    WebCore::IntSize size() const final;
    size_t frameCount() const final;
    WebCore::RepetitionCount repetitionCount() const final;
    String uti() const final { return String(); }
    String filenameExtension() const final;
    std::optional<WebCore::IntPoint> hotSpot() const final { return std::nullopt; }

    WebCore::IntSize frameSizeAtIndex(size_t, WebCore::SubsamplingLevel = WebCore::SubsamplingLevel::Default) const final;
    bool frameIsCompleteAtIndex(size_t) const final;

    Seconds frameDurationAtIndex(size_t) const final;
    bool frameHasAlphaAtIndex(size_t) const final;

    WebCore::PlatformImagePtr createFrameImageAtIndex(size_t, WebCore::SubsamplingLevel = WebCore::SubsamplingLevel::Default, const WebCore::DecodingOptions& = WebCore::DecodingOptions(WebCore::DecodingMode::Synchronous)) final;
    void decodeFrameAsynchronously(size_t, WebCore::SubsamplingLevel, const WebCore::DecodingOptions&, CompletionHandler<void(RefPtr<WebCore::NativeImage>&&)>&&) final;

    void setExpectedContentSize(long long) final;
    void setData(const WebCore::FragmentedSharedBuffer&, bool allDataReceived) final;
    bool isAllDataReceived() const final;
    void clearFrameBufferCache(size_t) final;

    // Called by RemoteImageDecoderManager when the Image process pushes the
    // decoder state after more encoded data arrived. Main run loop.
    void encodedDataStatusChanged(bool isFailed, bool isAllDataReceived, const WebCore::IntSize&, int32_t repetitionCount, Vector<WebCore::ImageDecoder::FrameInfo>&&);

    // Called by RemoteImageDecoderManager when the Image process reports that
    // a frame's shared buffer is ready to be presented. Main run loop.
    void frameDecoded(size_t index, std::optional<WebCore::ShareableBitmapHandle>&&, bool succeeded, bool isComplete, const WebCore::ImageDecoder::FrameInfo&);

    // Marks the decoder as failed, fails pending frame decodes, and reports
    // EncodedDataStatus::Error. Used when the connection to the Image process
    // is lost. Main run loop.
    void markFailed();

private:
    RemoteImageDecoder(RemoteImageDecoderManager&, WebCore::ImageDecoderIdentifier, const String& mimeType);

    RefPtr<WebCore::NativeImage> createFrameNativeImage(size_t index) const;

    // Guards the metadata mirrored from the Image process. Written on the
    // main run loop (pushes); ImageDecoder getters may be called from other
    // threads.
    mutable Lock m_lock;

    WebCore::IntSize m_size WTF_GUARDED_BY_LOCK(m_lock);
    int32_t m_repetitionCount WTF_GUARDED_BY_LOCK(m_lock) { WebCore::RepetitionCountNone };
    Vector<WebCore::ImageDecoder::FrameInfo> m_frameInfos WTF_GUARDED_BY_LOCK(m_lock);
    bool m_isAllDataReceived WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_isFailed WTF_GUARDED_BY_LOCK(m_lock) { false };

    // The frame buffers shared by the Image process, mapped read-only, and
    // which frames are completely decoded in them.
    HashMap<size_t, Ref<WebCore::ShareableBitmap>, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>> m_frameBuffers WTF_GUARDED_BY_LOCK(m_lock);
    HashSet<size_t, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>> m_completeFrames WTF_GUARDED_BY_LOCK(m_lock);

    // Main run loop only (installed and fired there).
    WTF::Function<void(WebCore::EncodedDataStatus)> m_encodedDataStatusChangedCallback;

    // Callers waiting for a frame to be decoded. Main run loop only.
    HashMap<size_t, Vector<CompletionHandler<void(RefPtr<WebCore::NativeImage>&&)>>, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>> m_pendingFrameDecodes;

    // The encoded data, shared read-only with the Image process. Only the
    // bytes the Image process hasn't seen yet are written on each setData().
    // Main run loop only.
    RefPtr<WebCore::SharedMemory> m_encodedDataBuffer;
    size_t m_encodedDataSize { 0 };
    bool m_isAllDataSent { false };
    size_t m_expectedContentSize { 0 };

    Ref<RemoteImageDecoderManager> m_manager;
    WebCore::ImageDecoderIdentifier m_identifier;
    String m_mimeType;
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
