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
#include "RemoteImageDecoder.h"

#if ENABLE(IMAGE_PROCESS)

#include "ImageConnectionToWebProcessMessages.h"
#include "Logging.h"
#include "RemoteImageDecoderManager.h"
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/NativeImage.h>
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>

namespace WebKit {

using namespace WebCore;

RefPtr<RemoteImageDecoder> RemoteImageDecoder::create(RemoteImageDecoderManager& manager, ImageDecoderIdentifier identifier, const String& mimeType)
{
    return adoptRef(*new RemoteImageDecoder(manager, identifier, mimeType));
}

RemoteImageDecoder::RemoteImageDecoder(RemoteImageDecoderManager& manager, ImageDecoderIdentifier identifier, const String& mimeType)
    : m_manager(manager)
    , m_identifier(identifier)
    , m_mimeType(mimeType)
{
}

RemoteImageDecoder::~RemoteImageDecoder()
{
    if (auto connection = m_manager->imageProcessConnectionIfExists())
        connection->connection().send(Messages::ImageConnectionToWebProcess::DeleteDecoder(m_identifier), 0);
    m_manager->unregisterDecoder(*this);
}

EncodedDataStatus RemoteImageDecoder::encodedDataStatus() const
{
    Locker locker { m_lock };

    if (m_isFailed)
        return EncodedDataStatus::Error;
    if (m_size.isEmpty())
        return EncodedDataStatus::Unknown;
    return m_isAllDataReceived ? EncodedDataStatus::Complete : EncodedDataStatus::SizeAvailable;
}

void RemoteImageDecoder::setEncodedDataStatusChangeCallback(WTF::Function<void(EncodedDataStatus)>&& callback)
{
    ASSERT(RunLoop::isMain());
    m_encodedDataStatusChangedCallback = WTF::move(callback);
}

IntSize RemoteImageDecoder::size() const
{
    Locker locker { m_lock };
    return m_size;
}

size_t RemoteImageDecoder::frameCount() const
{
    Locker locker { m_lock };
    return m_frameInfos.size();
}

RepetitionCount RemoteImageDecoder::repetitionCount() const
{
    Locker locker { m_lock };
    return m_repetitionCount;
}

String RemoteImageDecoder::filenameExtension() const
{
    return MIMETypeRegistry::preferredExtensionForMIMEType(m_mimeType);
}

IntSize RemoteImageDecoder::frameSizeAtIndex(size_t, SubsamplingLevel) const
{
    return size();
}

bool RemoteImageDecoder::frameIsCompleteAtIndex(size_t index) const
{
    Locker locker { m_lock };
    return m_completeFrames.contains(index);
}

Seconds RemoteImageDecoder::frameDurationAtIndex(size_t index) const
{
    Locker locker { m_lock };
    if (index >= m_frameInfos.size())
        return 0_s;
    return m_frameInfos[index].duration;
}

bool RemoteImageDecoder::frameHasAlphaAtIndex(size_t index) const
{
    Locker locker { m_lock };
    if (index >= m_frameInfos.size())
        return false;
    return m_frameInfos[index].hasAlpha;
}

void RemoteImageDecoder::setExpectedContentSize(long long expectedContentSize)
{
    if (expectedContentSize > 0)
        m_expectedContentSize = static_cast<size_t>(expectedContentSize);
}

void RemoteImageDecoder::setData(const FragmentedSharedBuffer& data, bool allDataReceived)
{
    // Called on the main run loop. The data is appended to the shared buffer;
    // the resulting encoded data status is pushed back asynchronously and
    // reported through the encoded data status change callback.
    RefPtr connection = m_manager->imageProcessConnectionIfExists();
    if (!connection) {
        markFailed();
        return;
    }

    // The data only ever grows for a given decoder.
    size_t size = data.size();
    if (size < m_encodedDataSize) {
        markFailed();
        return;
    }
    if (size == m_encodedDataSize && allDataReceived == m_isAllDataSent)
        return;

    if (!m_encodedDataBuffer || size > m_encodedDataBuffer->size()) {
        static constexpr size_t minimumCapacity = 64 * KB;
        size_t capacity = size;
        if (m_encodedDataBuffer)
            capacity = std::max(capacity, m_encodedDataBuffer->size() * 2);
        else if (!allDataReceived)
            capacity = std::max({ capacity, m_expectedContentSize, minimumCapacity });

        RefPtr buffer = SharedMemory::allocate(capacity);
        if (!buffer) {
            markFailed();
            return;
        }
        auto handle = buffer->createHandle(SharedMemory::Protection::ReadOnly);
        if (!handle) {
            markFailed();
            return;
        }
        data.copyTo(buffer->mutableSpan().first(size));
        connection->connection().send(Messages::ImageConnectionToWebProcess::SetEncodedDataBuffer(m_identifier, WTF::move(*handle)), 0);
        m_encodedDataBuffer = WTF::move(buffer);
    } else
        data.copyTo(m_encodedDataBuffer->mutableSpan().subspan(m_encodedDataSize, size - m_encodedDataSize), m_encodedDataSize);

    m_encodedDataSize = size;
    m_isAllDataSent = allDataReceived;
    connection->connection().send(Messages::ImageConnectionToWebProcess::EncodedDataAppended(m_identifier, size, allDataReceived), 0);
}

bool RemoteImageDecoder::isAllDataReceived() const
{
    Locker locker { m_lock };
    return m_isAllDataReceived;
}

void RemoteImageDecoder::clearFrameBufferCache(size_t index)
{
    // The Image process drops its buffers for the same frames, so a later
    // decode of one of them comes with a new buffer.
    {
        Locker locker { m_lock };
        m_frameBuffers.removeIf([index](auto& entry) {
            return entry.key != index;
        });
        m_completeFrames.removeIf([index](size_t frameIndex) {
            return frameIndex != index;
        });
    }

    if (auto connection = m_manager->imageProcessConnectionIfExists())
        connection->connection().send(Messages::ImageConnectionToWebProcess::ClearFrameBufferCache(m_identifier, index), 0);
}

RefPtr<NativeImage> RemoteImageDecoder::createFrameNativeImage(size_t index) const
{
    RefPtr<ShareableBitmap> buffer;
    {
        Locker locker { m_lock };
        buffer = m_frameBuffers.get(index);
    }
    if (!buffer)
        return nullptr;

    // The image references the shared memory without copying it, and keeps
    // the buffer alive.
#if USE(CAIRO)
    PlatformImagePtr platformImage = buffer->createCairoSurface();
#elif USE(SKIA)
    PlatformImagePtr platformImage = buffer->createPlatformImage(BackingStoreCopy::DontCopyBackingStore, ShouldInterpolate::Yes);
#else
    PlatformImagePtr platformImage;
#endif
    if (!platformImage)
        return nullptr;
    return NativeImage::create(WTF::move(platformImage));
}

PlatformImagePtr RemoteImageDecoder::createFrameImageAtIndex(size_t index, SubsamplingLevel, const DecodingOptions&)
{
    // Never decodes; frames come from decodeFrameAsynchronously(). A frame
    // that is already complete can be handed out directly.
    if (!frameIsCompleteAtIndex(index))
        return nullptr;
    RefPtr nativeImage = createFrameNativeImage(index);
    return nativeImage ? nativeImage->platformImage() : nullptr;
}

void RemoteImageDecoder::decodeFrameAsynchronously(size_t index, SubsamplingLevel, const DecodingOptions&, CompletionHandler<void(RefPtr<NativeImage>&&)>&& completionHandler)
{
    ASSERT(RunLoop::isMain());

    RefPtr connection = m_manager->imageProcessConnectionIfExists();
    if (!connection) {
        completionHandler(nullptr);
        return;
    }

    auto& pendingDecodes = m_pendingFrameDecodes.ensure(index, [] {
        return Vector<CompletionHandler<void(RefPtr<NativeImage>&&)>> { };
    }).iterator->value;
    pendingDecodes.append(WTF::move(completionHandler));
    if (pendingDecodes.size() == 1)
        connection->connection().send(Messages::ImageConnectionToWebProcess::DecodeFrame(m_identifier, index), 0);
}

void RemoteImageDecoder::frameDecoded(size_t index, std::optional<ShareableBitmapHandle>&& bufferHandle, bool succeeded, bool isComplete, const FrameInfo& frameInfo)
{
    ASSERT(RunLoop::isMain());

    RefPtr buffer = bufferHandle ? ShareableBitmap::create(WTF::move(*bufferHandle), SharedMemory::Protection::ReadOnly) : nullptr;
    {
        Locker locker { m_lock };
        if (succeeded) {
            if (index >= m_frameInfos.size())
                m_frameInfos.resize(index + 1);
            m_frameInfos[index] = frameInfo;
        }
        if (buffer)
            m_frameBuffers.set(index, buffer.releaseNonNull());
        if (succeeded && isComplete)
            m_completeFrames.add(index);
        else
            m_completeFrames.remove(index);
    }

    RefPtr nativeImage = succeeded ? createFrameNativeImage(index) : nullptr;
    for (auto& completionHandler : m_pendingFrameDecodes.take(index))
        completionHandler(RefPtr { nativeImage });
}

void RemoteImageDecoder::encodedDataStatusChanged(bool isFailed, bool isAllDataReceived, const IntSize& size, int32_t repetitionCount, Vector<ImageDecoder::FrameInfo>&& frameInfos)
{
    ASSERT(RunLoop::isMain());

    {
        Locker locker { m_lock };
        m_isFailed = isFailed;
        m_isAllDataReceived = isAllDataReceived;
        if (!size.isEmpty())
            m_size = size;
        m_repetitionCount = repetitionCount;
        // Durations are only known for frames that have been decoded; keep
        // the ones frameDecoded() already reported.
        for (size_t i = 0; i < std::min(frameInfos.size(), m_frameInfos.size()); ++i) {
            if (!frameInfos[i].duration)
                frameInfos[i].duration = m_frameInfos[i].duration;
        }
        m_frameInfos = WTF::move(frameInfos);
    }

    if (m_encodedDataStatusChangedCallback)
        m_encodedDataStatusChangedCallback(encodedDataStatus());
}

void RemoteImageDecoder::markFailed()
{
    ASSERT(RunLoop::isMain());

    {
        Locker locker { m_lock };
        m_isFailed = true;
    }

    for (auto& pendingDecodes : std::exchange(m_pendingFrameDecodes, { }).values()) {
        for (auto& completionHandler : pendingDecodes)
            completionHandler(nullptr);
    }

    if (m_encodedDataStatusChangedCallback)
        m_encodedDataStatusChangedCallback(encodedDataStatus());
}

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
