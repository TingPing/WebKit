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
#include "ImageConnectionToWebProcess.h"

#if ENABLE(IMAGE_PROCESS)

#include "ImageDecodingQueue.h"
#include "ImageProcess.h"
#include "ImageProcessProxyMessages.h"
#include "Logging.h"
#include "RemoteImageDecoderManagerMessages.h"
#include <WebCore/JPEGXLImageDecoder.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SharedMemory.h>
#include <wtf/RunLoop.h>
#include <limits>

#define MESSAGE_CHECK(assertion) MESSAGE_CHECK_BASE(assertion, connection())

namespace WebKit {

using namespace WebCore;

Ref<ImageConnectionToWebProcess> ImageConnectionToWebProcess::create(
    ImageProcess& imageProcess,
    WebCore::ProcessIdentifier webProcessIdentifier,
    IPC::Connection::Handle&& connectionHandle)
{
    return adoptRef(*new ImageConnectionToWebProcess(imageProcess, webProcessIdentifier, WTF::move(connectionHandle)));
}

ImageConnectionToWebProcess::ImageConnectionToWebProcess(
    ImageProcess& imageProcess,
    WebCore::ProcessIdentifier webProcessIdentifier,
    IPC::Connection::Handle&& connectionHandle)
    : m_connection(IPC::Connection::createClientConnection(IPC::Connection::Identifier { WTF::move(connectionHandle) }))
    , m_imageProcess(imageProcess)
    , m_webProcessIdentifier(webProcessIdentifier)
{
    RELEASE_ASSERT(RunLoop::isMain());

    m_connection->open(*this);
}

ImageConnectionToWebProcess::~ImageConnectionToWebProcess()
{
    RELEASE_ASSERT(RunLoop::isMain());

    m_connection->invalidate();
}

void ImageConnectionToWebProcess::didClose(IPC::Connection&)
{
    imageProcess().removeImageConnectionToWebProcess(*this); // May destroy |this|.
}

void ImageConnectionToWebProcess::didReceiveInvalidMessage(IPC::Connection&, IPC::MessageName messageName, const Vector<uint32_t>&)
{
    RELEASE_LOG_FAULT_WITH_PAYLOAD(IPC, "Received an invalid message %s from WebContent process %" PRIu64 ", requesting for it to be terminated.", description(messageName), m_webProcessIdentifier.toUInt64());
    m_imageProcess->parentProcessConnection()->send(Messages::ImageProcessProxy::TerminateWebProcess(m_webProcessIdentifier, messageName), 0);
}

void ImageConnectionToWebProcess::createDecoder(WebCore::ImageDecoderIdentifier identifier, uint8_t alphaOption, uint8_t gammaAndColorProfileOption)
{
    MESSAGE_CHECK(!m_decoders.contains(identifier));

    // Only the JPEG XL format is decoded in the Image process for now. The
    // Web process is not trusted, so it could request a decoder for data it
    // never sniffed; that is harmless, the decoder will simply fail and the
    // failure will be reported with EncodedDataStatusChanged().
    auto decoder = JPEGXLImageDecoder::create(static_cast<AlphaOption>(alphaOption), static_cast<GammaAndColorProfileOption>(gammaAndColorProfileOption));
    if (!decoder) {
        // The Web process did nothing wrong here; report the decoder as failed.
        connection().send(Messages::RemoteImageDecoderManager::EncodedDataStatusChanged(identifier, true, false, IntSize(), RepetitionCountNone, { }), 0);
        return;
    }

    m_decoders.add(identifier, Decoder { WTF::move(decoder), nullptr, FrameBuffers::create() });
}

void ImageConnectionToWebProcess::setEncodedDataBuffer(WebCore::ImageDecoderIdentifier identifier, WebCore::SharedMemory::Handle&& handle)
{
    auto it = m_decoders.find(identifier);
    MESSAGE_CHECK(it != m_decoders.end());

    RefPtr memory = WebCore::SharedMemory::map(WTF::move(handle), WebCore::SharedMemory::Protection::ReadOnly);
    MESSAGE_CHECK(memory);
    it->value.encodedData = WTF::move(memory);
}

void ImageConnectionToWebProcess::encodedDataAppended(WebCore::ImageDecoderIdentifier identifier, uint64_t size, bool allDataReceived)
{
    auto it = m_decoders.find(identifier);
    MESSAGE_CHECK(it != m_decoders.end());
    RefPtr memory = it->value.encodedData;
    MESSAGE_CHECK(memory && size <= memory->size());

    // The decoder reads the shared memory in place: a single segment keeps
    // ScalableImageDecoder::setData() from copying it into a contiguous buffer.
    Ref buffer = WebCore::SharedBuffer::create(WebCore::FragmentedSharedBuffer::DataSegment::Provider {
        [memory = memory.releaseNonNull(), size = static_cast<size_t>(size)]() -> std::span<const uint8_t> { return memory->span().first(size); }
    });

    imageProcess().decodingQueue().enqueueTask(identifier, [connection = m_connection, identifier, decoder = Ref { *it->value.decoder }, buffer = WTF::move(buffer), allDataReceived] {
        decoder->setData(buffer, allDataReceived);

        auto frameCount = decoder->frameCount();
        Vector<WebCore::ImageDecoder::FrameInfo> frameInfos;
        frameInfos.reserveInitialCapacity(frameCount);
        for (size_t i = 0; i < frameCount; ++i)
            frameInfos.append({ decoder->frameHasAlphaAtIndex(i), decoder->frameDurationAtIndex(i) });

        connection->send(Messages::RemoteImageDecoderManager::EncodedDataStatusChanged(identifier, decoder->failed(), decoder->isAllDataReceived(), decoder->size(), decoder->repetitionCount(), WTF::move(frameInfos)), 0);
    });
}

namespace {

struct FrameDecodeResult {
    std::optional<WebCore::ShareableBitmapHandle> newBuffer;
    bool succeeded { false };
    bool isComplete { false };
    WebCore::ImageDecoderFrameInfo frameInfo { true, 0_s };
};

}

static FrameDecodeResult decodeFrameIntoSharedBuffer(WebCore::ScalableImageDecoder& decoder, ImageConnectionToWebProcess::FrameBuffers& frameBuffers, size_t index)
{
    FrameDecodeResult result;
    auto frameSize = decoder.frameSizeAtIndex(index, WebCore::SubsamplingLevel::Default);
    if (frameSize.isEmpty())
        return result;

    RefPtr buffer = frameBuffers.buffers.get(index);
    if (!buffer || buffer->size() != frameSize) {
        buffer = WebCore::ShareableBitmap::create({ frameSize });
        if (!buffer)
            return result;
        auto handle = buffer->createHandle(WebCore::SharedMemory::Protection::ReadOnly);
        if (!handle)
            return result;
        result.newBuffer = WTF::move(*handle);
        frameBuffers.buffers.set(index, *buffer);
        frameBuffers.decodedInPlace.remove(index);
    }

    // Have the decoder write the frame's pixels straight into the shared
    // buffer. A ShareableBitmap's rows are tightly packed, which is what the
    // decoders expect. Only a frame that hasn't started decoding yet can take
    // a destination.
    bool offeredDestination = false;
    if (!decoder.frameIsInitializedAtIndex(index) && buffer->bytesPerRow() == static_cast<unsigned>(frameSize.width()) * 4) {
        decoder.setFrameDestination(index, WebCore::FragmentedSharedBuffer::DataSegment::create(WebCore::FragmentedSharedBuffer::DataSegment::Provider {
            [buffer = Ref { *buffer }]() -> std::span<const uint8_t> { return buffer->span(); }
        }));
        offeredDestination = true;
    }

    auto platformImage = decoder.createFrameImageAtIndex(index, WebCore::SubsamplingLevel::Default, { });

    if (offeredDestination) {
        if (decoder.didUseFrameDestination(index))
            frameBuffers.decodedInPlace.add(index);
        else
            frameBuffers.decodedInPlace.remove(index);
    }

    if (!platformImage)
        return result;

    if (!frameBuffers.decodedInPlace.contains(index)) {
        // The decoder keeps this frame in its own memory; copy it into the
        // shared buffer.
        auto imageInfo = SkImageInfo::MakeN32Premul(frameSize.width(), frameSize.height(), buffer->colorSpace().platformColorSpace());
        if (!platformImage->readPixels(imageInfo, buffer->mutableSpan().data(), buffer->bytesPerRow(), 0, 0))
            return result;
    }

    result.succeeded = true;
    result.isComplete = decoder.frameIsCompleteAtIndex(index);
    result.frameInfo = { decoder.frameHasAlphaAtIndex(index), decoder.frameDurationAtIndex(index) };
    return result;
}

void ImageConnectionToWebProcess::decodeFrame(WebCore::ImageDecoderIdentifier identifier, uint64_t index)
{
    auto it = m_decoders.find(identifier);
    MESSAGE_CHECK(it != m_decoders.end());

    imageProcess().decodingQueue().enqueueTask(identifier, [connection = m_connection, identifier, index, decoder = Ref { *it->value.decoder }, frameBuffers = Ref { *it->value.frameBuffers }] {
        auto result = decodeFrameIntoSharedBuffer(decoder, frameBuffers, static_cast<size_t>(index));
        connection->send(Messages::RemoteImageDecoderManager::FrameDecoded(identifier, index, WTF::move(result.newBuffer), result.succeeded, result.isComplete, result.frameInfo), 0);
    }, index);
}

void ImageConnectionToWebProcess::cancelDecode(WebCore::ImageDecoderIdentifier identifier, uint64_t index)
{
    MESSAGE_CHECK(m_decoders.contains(identifier));
    imageProcess().decodingQueue().cancelTasks(identifier, index);
}

void ImageConnectionToWebProcess::clearFrameBufferCache(WebCore::ImageDecoderIdentifier identifier, uint64_t index)
{
    auto it = m_decoders.find(identifier);
    MESSAGE_CHECK(it != m_decoders.end());

    // The Web process drops its buffers for the same frames, so a later
    // decode of one of them sends a new buffer.
    imageProcess().decodingQueue().enqueueTask(identifier, [decoder = Ref { *it->value.decoder }, frameBuffers = Ref { *it->value.frameBuffers }, index] {
        decoder->clearFrameBufferCache(index);
        frameBuffers->removeAllExcept(index);
    });
}

void ImageConnectionToWebProcess::deleteDecoder(WebCore::ImageDecoderIdentifier identifier)
{
    MESSAGE_CHECK(m_decoders.contains(identifier));

    // Drop queued work for the decoder; in-flight tasks hold their own
    // references to the decoder and finish harmlessly (their results are
    // ignored by the Web process).
    imageProcess().decodingQueue().cancelTasks(identifier);
    m_decoders.remove(identifier);
}

void ImageConnectionToWebProcess::clearAllFrameBufferCaches()
{
    // The Web process keeps presenting the buffers it has mapped; frames
    // decoded again get new ones.
    for (auto& entry : m_decoders) {
        imageProcess().decodingQueue().enqueueTask(entry.key, [decoder = Ref { *entry.value.decoder }, frameBuffers = Ref { *entry.value.frameBuffers }] {
            decoder->clearFrameBufferCache(std::numeric_limits<size_t>::max());
            frameBuffers->clear();
        });
    }
}

void ImageConnectionToWebProcess::FrameBuffers::removeAllExcept(size_t index)
{
    buffers.removeIf([index](auto& entry) {
        return entry.key != index;
    });
    decodedInPlace.removeIf([index](size_t frameIndex) {
        return frameIndex != index;
    });
}

void ImageConnectionToWebProcess::FrameBuffers::clear()
{
    buffers.clear();
    decodedInPlace.clear();
}

} // namespace WebKit

#undef MESSAGE_CHECK

#endif // ENABLE(IMAGE_PROCESS)
