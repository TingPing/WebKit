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

#include "config.h"
#include "ImageDecoderGlycin.h"

#if USE(GLYCIN)

#include "MIMETypeRegistry.h"
#include "SharedBuffer.h"
#include <glycin.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/GUniquePtr.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkImageInfo.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageDecoderGlycin);

static constexpr uint64_t maximumImagePixelCount = (1 << 29) - 1;
static constexpr size_t maximumFrameCount = 1024;

static bool isOverSize(const IntSize& size)
{
    return size.isEmpty() || static_cast<uint64_t>(size.width()) * size.height() > maximumImagePixelCount;
}

static constexpr GlyMemoryFormatSelection acceptedMemoryFormats(bool premultiplyAlpha)
{
    if constexpr (kN32_SkColorType == kBGRA_8888_SkColorType) {
        return static_cast<GlyMemoryFormatSelection>(GLY_MEMORY_SELECTION_B8G8R8
            | (premultiplyAlpha ? GLY_MEMORY_SELECTION_B8G8R8A8_PREMULTIPLIED : GLY_MEMORY_SELECTION_B8G8R8A8));
    }

    return static_cast<GlyMemoryFormatSelection>(GLY_MEMORY_SELECTION_R8G8B8
        | (premultiplyAlpha ? GLY_MEMORY_SELECTION_R8G8B8A8_PREMULTIPLIED : GLY_MEMORY_SELECTION_R8G8B8A8));
}

static GlySandboxSelector sandboxSelector()
{
    // A 1x1 grayscale PNG, used to find out whether glycin is able to spawn a sandboxed
    // loader at all. It cannot when the web process is itself confined by a sandbox that
    // disallows nesting, and glycin does not fall back to running loaders unsandboxed.
    static constexpr std::array<uint8_t, 67> probeImageData {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x3A, 0x7E, 0x9B, 0x55, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x60, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0xE5, 0x27, 0xDE, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };

    static GlySandboxSelector selector = [] {
        GRefPtr<GBytes> bytes = adoptGRef(g_bytes_new_static(probeImageData.data(), probeImageData.size()));
        GRefPtr<GlyLoader> loader = adoptGRef(gly_loader_new_for_bytes(bytes.get()));
        GUniqueOutPtr<GError> error;
        if (adoptGRef(gly_loader_load(loader.get(), &error.outPtr())))
            return GLY_SANDBOX_SELECTOR_AUTO;

        WTFLogAlways("Glycin cannot spawn a sandboxed image loader (%s), images will be decoded without the glycin sandbox",
            error ? error->message : "unknown error");
        return GLY_SANDBOX_SELECTOR_NOT_SANDBOXED;
    }();

    return selector;
}

static uint32_t readBigEndian32(std::span<const uint8_t> data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

static uint32_t readLittleEndian32(std::span<const uint8_t> data)
{
    return data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

static unsigned readLittleEndian16(std::span<const uint8_t> data)
{
    return data[0] | (data[1] << 8);
}

static RepetitionCount gifRepetitionCount(std::span<const uint8_t> data)
{
    // Netscape application extension: 0x21 0xFF 0x0B, an 11 byte identifier, then a
    // sub-block holding the loop count.
    constexpr size_t extensionSize = 18;
    for (size_t offset = 0; offset + extensionSize <= data.size(); ++offset) {
        if (data[offset] != 0x21 || data[offset + 1] != 0xFF || data[offset + 2] != 0x0B)
            continue;

        auto identifier = data.subspan(offset + 3, 11);
        if (!spanHasPrefix(identifier, "NETSCAPE2.0"_span) && !spanHasPrefix(identifier, "ANIMEXTS1.0"_span))
            continue;

        if (data[offset + 14] != 0x03 || data[offset + 15] != 0x01)
            continue;

        auto loopCount = readLittleEndian16(data.subspan(offset + 16, 2));
        if (!loopCount)
            return RepetitionCountInfinite;
        return loopCount + 1;
    }

    // A GIF without a loop count plays its animation once.
    return RepetitionCountOnce;
}

static RepetitionCount apngRepetitionCount(std::span<const uint8_t> data)
{
    constexpr size_t signatureSize = 8;
    for (size_t offset = signatureSize; offset + 12 <= data.size();) {
        auto chunkSize = readBigEndian32(data.subspan(offset, 4));
        auto name = data.subspan(offset + 4, 4);

        if (spanHasPrefix(name, "acTL"_span)) {
            if (chunkSize < 8 || offset + 8 + chunkSize > data.size())
                break;
            auto playCount = readBigEndian32(data.subspan(offset + 12, 4));
            if (!playCount)
                return RepetitionCountInfinite;
            return playCount;
        }

        // The animation control chunk always comes before the image data.
        if (spanHasPrefix(name, "IDAT"_span) || chunkSize > data.size())
            break;

        offset += 12 + chunkSize;
    }

    return RepetitionCountInfinite;
}

static RepetitionCount webPRepetitionCount(std::span<const uint8_t> data)
{
    constexpr size_t headerSize = 12;
    for (size_t offset = headerSize; offset + 8 <= data.size();) {
        auto name = data.subspan(offset, 4);
        auto chunkSize = readLittleEndian32(data.subspan(offset + 4, 4));

        if (spanHasPrefix(name, "ANIM"_span)) {
            if (chunkSize < 6 || offset + 14 > data.size())
                break;
            auto loopCount = readLittleEndian16(data.subspan(offset + 12, 2));
            if (!loopCount)
                return RepetitionCountInfinite;
            return loopCount;
        }

        if (chunkSize > data.size())
            break;

        offset += 8 + chunkSize + (chunkSize & 1);
    }

    return RepetitionCountInfinite;
}

// glycin has no API for the number of times an animation should play, so it is read
// from the container here. Only this metadata is parsed; glycin decodes the pixels.
static RepetitionCount repetitionCountFromEncodedData(const String& mimeType, std::span<const uint8_t> data)
{
    if (mimeType == "image/gif"_s)
        return gifRepetitionCount(data);
    if (mimeType == "image/png"_s || mimeType == "image/apng"_s)
        return apngRepetitionCount(data);
    if (mimeType == "image/webp"_s)
        return webPRepetitionCount(data);
    return RepetitionCountInfinite;
}

static PlatformImagePtr createImageForFrame(GlyFrame* frame, const IntSize& size, bool hasAlpha, bool premultiplyAlpha)
{
    GRefPtr<GBytes> pixels = gly_frame_get_buf_bytes(frame);
    if (!pixels)
        return nullptr;

    size_t pixelsSize = 0;
    const auto* pixelsData = static_cast<const uint8_t*>(g_bytes_get_data(pixels.get(), &pixelsSize));
    if (!pixelsData)
        return nullptr;

    uint64_t sourceStride = gly_frame_get_stride(frame);
    uint64_t sourceBytesPerPixel = hasAlpha ? 4 : 3;
    if (pixelsSize < sourceStride * (size.height() - 1) + size.width() * sourceBytesPerPixel)
        return nullptr;

    auto imageInfo = SkImageInfo::Make(size.width(), size.height(), kN32_SkColorType,
        hasAlpha ? (premultiplyAlpha ? kPremul_SkAlphaType : kUnpremul_SkAlphaType) : kOpaque_SkAlphaType,
        SkColorSpace::MakeSRGB());

    if (hasAlpha) {
        auto data = SkData::MakeWithProc(pixelsData, pixelsSize, [](const void*, void* context) {
            g_bytes_unref(static_cast<GBytes*>(context));
        }, pixels.leakRef());
        return SkImages::RasterFromData(imageInfo, WTF::move(data), sourceStride);
    }

    auto destinationStride = imageInfo.minRowBytes();
    auto data = SkData::MakeUninitialized(imageInfo.computeByteSize(destinationStride));
    auto source = unsafeMakeSpan(pixelsData, pixelsSize);
    auto destination = unsafeMakeSpan(static_cast<uint8_t*>(data->writable_data()), data->size());
    for (int y = 0; y < size.height(); ++y) {
        auto sourceRow = source.subspan(y * sourceStride);
        auto destinationRow = destination.subspan(y * destinationStride);
        for (int x = 0; x < size.width(); ++x) {
            auto sourcePixel = sourceRow.subspan(x * 3, 3);
            auto destinationPixel = destinationRow.subspan(x * 4, 4);
            destinationPixel[0] = sourcePixel[0];
            destinationPixel[1] = sourcePixel[1];
            destinationPixel[2] = sourcePixel[2];
            destinationPixel[3] = 0xFF;
        }
    }

    return SkImages::RasterFromData(imageInfo, WTF::move(data), destinationStride);
}

Ref<ImageDecoderGlycin> ImageDecoderGlycin::create(FragmentedSharedBuffer&, const String&, AlphaOption alphaOption, GammaAndColorProfileOption gammaAndColorProfileOption)
{
    return adoptRef(*new ImageDecoderGlycin(alphaOption, gammaAndColorProfileOption));
}

ImageDecoderGlycin::ImageDecoderGlycin(AlphaOption alphaOption, GammaAndColorProfileOption)
    : m_premultiplyAlpha(alphaOption == AlphaOption::Premultiplied)
{
}

ImageDecoderGlycin::~ImageDecoderGlycin() = default;

bool ImageDecoderGlycin::setFailed()
{
    m_encodedDataStatus = EncodedDataStatus::Error;
    m_image = nullptr;
    m_firstFrameBytes = nullptr;
    m_frames.clear();
    m_nextFrameIndex = 0;
    m_totalFrameCount = std::nullopt;
    return false;
}

void ImageDecoderGlycin::resetImage()
{
    m_image = nullptr;
    m_nextFrameIndex = 0;
}

bool ImageDecoderGlycin::ensureImage()
{
    if (m_image)
        return true;

    if (!m_data || m_data->isEmpty())
        return false;

    auto bytes = m_data->createGBytes();
    GRefPtr<GlyLoader> loader = adoptGRef(gly_loader_new_for_bytes(bytes.get()));
    gly_loader_set_sandbox_selector(loader.get(), sandboxSelector());
    gly_loader_set_apply_transformations(loader.get(), FALSE);
    gly_loader_set_accepted_memory_formats(loader.get(), acceptedMemoryFormats(m_premultiplyAlpha));

    GUniqueOutPtr<GError> error;
    m_image = adoptGRef(gly_loader_load(loader.get(), &error.outPtr()));
    if (!m_image) {
        if (m_isAllDataReceived)
            return setFailed();

        m_metadataAttemptThreshold = m_data->size() * 4;
        return false;
    }

    m_nextFrameIndex = 0;
    return true;
}

bool ImageDecoderGlycin::decodeMetadata()
{
    if (!ensureImage())
        return false;

    // glycin detects the format from the image data and its loaders cover more formats than
    // WebKit supports, so what it found has to be checked against the supported formats. The
    // type the resource was served with is not enough: the data may be a different format.
    m_mimeType = String::fromUTF8(gly_image_get_mime_type(m_image.get()));
    if (!MIMETypeRegistry::isSupportedImageMIMEType(m_mimeType))
        return setFailed();

    IntSize size(gly_image_get_width(m_image.get()), gly_image_get_height(m_image.get()));
    if (isOverSize(size))
        return setFailed();

    m_size = size;
    m_orientation = ImageOrientation::fromEXIFValue(gly_image_get_transformation_orientation(m_image.get()));
    m_encodedDataStatus = EncodedDataStatus::SizeAvailable;
    m_hasCompleteMetadata = m_isAllDataReceived;

    if (!m_isAllDataReceived)
        resetImage();

    return true;
}

void ImageDecoderGlycin::didFindLastFrame(size_t frameCount)
{
    m_totalFrameCount = frameCount;
    m_firstFrameBytes = nullptr;
}

bool ImageDecoderGlycin::decodeNextFrame(bool retainImage)
{
    if (!ensureImage())
        return false;

    GUniqueOutPtr<GError> error;
    GRefPtr<GlyFrame> frame = adoptGRef(gly_image_next_frame(m_image.get(), &error.outPtr()));
    if (!frame) {
        // Only failing to decode the first frame makes the image invalid. Images that declare
        // more frames than they have data for keep the frames that did decode.
        if (!m_nextFrameIndex)
            return setFailed();

        didFindLastFrame(m_nextFrameIndex);
        m_image = nullptr;
        return false;
    }

    IntSize size(gly_frame_get_width(frame.get()), gly_frame_get_height(frame.get()));
    if (isOverSize(size))
        return setFailed();

    GRefPtr<GBytes> pixels = gly_frame_get_buf_bytes(frame.get());
    if (!pixels)
        return setFailed();

    // Frame requests wrap around to the first frame at the end of an animation, which is the
    // only way to find the frame count: glycin has no API for it, and asking for a frame past
    // the last one is not allowed, with some loaders hanging rather than reporting an error.
    if (!m_totalFrameCount && m_nextFrameIndex && g_bytes_equal(pixels.get(), m_firstFrameBytes.get())) {
        didFindLastFrame(m_nextFrameIndex);
        m_nextFrameIndex = 1;
        if (*m_totalFrameCount == 1)
            m_image = nullptr;
        return false;
    }

    auto duration = Seconds::fromMicroseconds(gly_frame_get_delay(frame.get()));
    bool hasAlpha = gly_memory_format_has_alpha(gly_frame_get_memory_format(frame.get()));

    Frame decodedFrame;
    decodedFrame.size = size;
    decodedFrame.duration = duration;
    decodedFrame.hasAlpha = hasAlpha;
    if (retainImage) {
        decodedFrame.image = createImageForFrame(frame.get(), size, hasAlpha, m_premultiplyAlpha);
        if (!decodedFrame.image)
            return setFailed();
    }

    if (m_nextFrameIndex >= m_frames.size())
        m_frames.grow(m_nextFrameIndex + 1);
    m_frames[m_nextFrameIndex] = WTF::move(decodedFrame);

    if (!m_nextFrameIndex && !m_totalFrameCount)
        m_firstFrameBytes = WTF::move(pixels);
    ++m_nextFrameIndex;

    if (!m_totalFrameCount) {
        // A zero delay means the image is not animated, so there is no next frame to ask for.
        if (!duration)
            didFindLastFrame(m_nextFrameIndex);
        else if (m_nextFrameIndex == maximumFrameCount)
            didFindLastFrame(m_nextFrameIndex);
    }

    if (m_totalFrameCount && m_nextFrameIndex == *m_totalFrameCount)
        m_image = nullptr;

    return true;
}

size_t ImageDecoderGlycin::frameCountWithLock()
{
    if (!m_totalFrameCount && m_isAllDataReceived && !failed()) {
        while (!m_totalFrameCount && decodeNextFrame(!m_nextFrameIndex)) { }
    }

    return m_totalFrameCount.value_or(1);
}

const ImageDecoderGlycin::Frame* ImageDecoderGlycin::frameWithLock(size_t index) const
{
    if (index >= m_frames.size() || !m_frames[index].isValid())
        return nullptr;
    return &m_frames[index];
}

const ImageDecoderGlycin::Frame* ImageDecoderGlycin::decodedFrameWithLock(size_t index)
{
    if (const auto* frame = frameWithLock(index); frame && frame->image)
        return frame;

    if (!m_isAllDataReceived || failed())
        return nullptr;

    if (m_totalFrameCount && index >= *m_totalFrameCount)
        return nullptr;

    if (index < m_nextFrameIndex)
        resetImage();

    while (m_nextFrameIndex <= index) {
        if (!decodeNextFrame(true))
            return nullptr;
    }

    return frameWithLock(index);
}

EncodedDataStatus ImageDecoderGlycin::encodedDataStatus() const
{
    Locker locker { m_lock };
    return m_encodedDataStatus;
}

IntSize ImageDecoderGlycin::size() const
{
    Locker locker { m_lock };
    return m_encodedDataStatus >= EncodedDataStatus::SizeAvailable ? m_size : IntSize();
}

size_t ImageDecoderGlycin::frameCount() const
{
    Locker locker { m_lock };
    return const_cast<ImageDecoderGlycin*>(this)->frameCountWithLock();
}

RepetitionCount ImageDecoderGlycin::repetitionCount() const
{
    Locker locker { m_lock };
    auto* decoder = const_cast<ImageDecoderGlycin*>(this);
    if (decoder->frameCountWithLock() <= 1)
        return RepetitionCountNone;

    if (!m_repetitionCount && m_data)
        decoder->m_repetitionCount = repetitionCountFromEncodedData(m_mimeType, m_data->span());

    return m_repetitionCount.value_or(RepetitionCountInfinite);
}

String ImageDecoderGlycin::filenameExtension() const
{
    Locker locker { m_lock };
    return MIMETypeRegistry::preferredExtensionForMIMEType(m_mimeType);
}

IntSize ImageDecoderGlycin::frameSizeAtIndex(size_t index, SubsamplingLevel) const
{
    Locker locker { m_lock };
    if (const auto* frame = frameWithLock(index))
        return frame->size;
    return m_encodedDataStatus >= EncodedDataStatus::SizeAvailable ? m_size : IntSize();
}

bool ImageDecoderGlycin::frameIsCompleteAtIndex(size_t index) const
{
    Locker locker { m_lock };
    return frameWithLock(index);
}

ImageOrientation ImageDecoderGlycin::frameOrientationAtIndex(size_t) const
{
    Locker locker { m_lock };
    return m_orientation;
}

Seconds ImageDecoderGlycin::frameDurationAtIndex(size_t index) const
{
    Locker locker { m_lock };
    const auto* frame = frameWithLock(index);

    // Many annoying ads specify a 0 duration to make an image flash as quickly as possible.
    // We follow Firefox's behavior and use a duration of 100 ms for any frames that specify
    // a duration of <= 10 ms. See <rdar://problem/7689300> and <http://webkit.org/b/36082>
    // for more information.
    if (!frame || frame->duration < 11_ms)
        return 100_ms;

    return frame->duration;
}

bool ImageDecoderGlycin::frameHasAlphaAtIndex(size_t index) const
{
    Locker locker { m_lock };
    const auto* frame = frameWithLock(index);
    return frame ? frame->hasAlpha : true;
}

PlatformImagePtr ImageDecoderGlycin::createFrameImageAtIndex(size_t index, SubsamplingLevel, const DecodingOptions&)
{
    Locker locker { m_lock };
    if (const auto* frame = decodedFrameWithLock(index))
        return frame->image;
    return nullptr;
}

void ImageDecoderGlycin::setData(const FragmentedSharedBuffer& data, bool allDataReceived)
{
    Locker locker { m_lock };
    if (failed())
        return;

    m_data = data.makeContiguous();
    m_isAllDataReceived = allDataReceived;
    m_repetitionCount = std::nullopt;
    m_hasCompleteMetadata = false;
    resetImage();
    m_frames.clear();
    m_firstFrameBytes = nullptr;
    m_totalFrameCount = std::nullopt;

    // Metadata read from an incomplete buffer can be wrong: glycin reports what it can find
    // so far, which for JPEG means no orientation until the whole file is there.
    bool needsMetadata = m_encodedDataStatus < EncodedDataStatus::SizeAvailable || (allDataReceived && !m_hasCompleteMetadata);
    if (needsMetadata && (allDataReceived || m_data->size() >= m_metadataAttemptThreshold))
        decodeMetadata();

    if (allDataReceived && m_encodedDataStatus == EncodedDataStatus::SizeAvailable)
        m_encodedDataStatus = EncodedDataStatus::Complete;
}

bool ImageDecoderGlycin::isAllDataReceived() const
{
    Locker locker { m_lock };
    return m_encodedDataStatus == EncodedDataStatus::Complete;
}

void ImageDecoderGlycin::clearFrameBufferCache(size_t clearBeforeFrame)
{
    Locker locker { m_lock };
    for (size_t index = 0; index < std::min(clearBeforeFrame, m_frames.size()); ++index)
        m_frames[index].image = nullptr;
}

} // namespace WebCore

#endif // USE(GLYCIN)
