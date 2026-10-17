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
#include "RemoteImageDecoderManager.h"

#if ENABLE(IMAGE_PROCESS)

#include "ImageConnectionToWebProcessMessages.h"
#include "Logging.h"
#include "RemoteImageDecoder.h"
#include "RemoteImageDecoderManagerMessages.h"
#include "WebProcess.h"
#include <WebCore/ImageDecoderFactory.h>
#include <WebCore/ScalableImageDecoder.h>
#include <wtf/MainThread.h>

namespace WebKit {

using namespace WebCore;

Ref<RemoteImageDecoderManager> RemoteImageDecoderManager::create()
{
    return adoptRef(*new RemoteImageDecoderManager);
}

RemoteImageDecoderManager::RemoteImageDecoderManager() = default;

RemoteImageDecoderManager::~RemoteImageDecoderManager()
{
    if (RefPtr imageProcessConnection = m_imageProcessConnection.get())
        imageProcessConnection->messageReceiverMap().removeMessageReceiver(Messages::RemoteImageDecoderManager::messageReceiverName());
}

void RemoteImageDecoderManager::setImageProcessEnabled(bool enabled)
{
    if (enabled == m_isImageProcessEnabled)
        return;
    m_isImageProcessEnabled = enabled;

    if (!enabled) {
        ImageDecoderFactory::singleton().reset();
        return;
    }

    ImageDecoderFactory::singleton().set({
        supportsMediaType,
        [weakThis = ThreadSafeWeakPtr { *this }](FragmentedSharedBuffer& data, const String& mimeType, AlphaOption alphaOption, GammaAndColorProfileOption gammaAndColorProfileOption) {
            RefPtr protectedThis = weakThis.get();
            return protectedThis ? protectedThis->createImageDecoder(data, mimeType, alphaOption, gammaAndColorProfileOption) : nullptr;
        },
    });
}

bool RemoteImageDecoderManager::supportsMediaType(WebCore::ImageDecoder::MediaType type)
{
    return type == ImageDecoder::MediaType::Image;
}

RefPtr<WebCore::ImageDecoder> RemoteImageDecoderManager::createImageDecoder(FragmentedSharedBuffer& data, const String& mimeType, AlphaOption alphaOption, GammaAndColorProfileOption gammaAndColorProfileOption)
{
    ASSERT(RunLoop::isMain());

    // Only the JPEG XL format is decoded in the Image process for now.
    if (!ScalableImageDecoder::hasJPEGXLSignature(data))
        return nullptr;

    Ref imageProcessConnection = ensureImageProcessConnection();

    auto decoderIdentifier = ImageDecoderIdentifier::generate();
    imageProcessConnection->connection().send(Messages::ImageConnectionToWebProcess::CreateDecoder(decoderIdentifier, static_cast<uint8_t>(alphaOption), static_cast<uint8_t>(gammaAndColorProfileOption)), 0);

    RefPtr remoteImageDecoder = RemoteImageDecoder::create(*this, decoderIdentifier, mimeType);
    registerDecoder(*remoteImageDecoder);
    return remoteImageDecoder;
}

void RemoteImageDecoderManager::registerDecoder(RemoteImageDecoder& decoder)
{
    Locker locker { m_decodersLock };
    m_decoders.add(decoder.identifier(), ThreadSafeWeakPtr<RemoteImageDecoder> { decoder });
}

void RemoteImageDecoderManager::unregisterDecoder(RemoteImageDecoder& decoder)
{
    Locker locker { m_decodersLock };
    m_decoders.remove(decoder.identifier());
}

Ref<ImageProcessConnection> RemoteImageDecoderManager::ensureImageProcessConnection()
{
    ASSERT(RunLoop::isMain());

    if (RefPtr imageProcessConnection = m_imageProcessConnection.get())
        return imageProcessConnection.releaseNonNull();

    Ref imageProcessConnection = WebProcess::singleton().ensureImageProcessConnection();
    imageProcessConnection->addClient(*this);
    imageProcessConnection->messageReceiverMap().addMessageReceiver(Messages::RemoteImageDecoderManager::messageReceiverName(), *this);
    m_imageProcessConnection = imageProcessConnection.get();
    return imageProcessConnection;
}

void RemoteImageDecoderManager::imageProcessConnectionDidClose(ImageProcessConnection&)
{
    // The connection's message receiver map is destroyed with the connection,
    // so there is nothing to unregister. A new connection will be established
    // (and re-registered) on the next call to ensureImageProcessConnection().
    m_imageProcessConnection = nullptr;

    // The decoders' state in the Image process is gone; fail them so pending
    // frame decodes don't wait forever.
    Vector<Ref<RemoteImageDecoder>> decoders;
    {
        Locker locker { m_decodersLock };
        for (auto& weakDecoder : m_decoders.values()) {
            if (RefPtr decoder = weakDecoder.get())
                decoders.append(decoder.releaseNonNull());
        }
    }
    for (auto& decoder : decoders)
        decoder->markFailed();
}

void RemoteImageDecoderManager::frameDecoded(WebCore::ImageDecoderIdentifier identifier, uint64_t index, std::optional<WebCore::ShareableBitmapHandle>&& buffer, bool succeeded, bool isComplete, const WebCore::ImageDecoderFrameInfo& frameInfo)
{
    RefPtr<RemoteImageDecoder> remoteImageDecoder;
    {
        Locker locker { m_decodersLock };
        remoteImageDecoder = m_decoders.get(identifier).get();
    }
    if (!remoteImageDecoder)
        return;

    remoteImageDecoder->frameDecoded(static_cast<size_t>(index), WTF::move(buffer), succeeded, isComplete, frameInfo);
}

void RemoteImageDecoderManager::encodedDataStatusChanged(WebCore::ImageDecoderIdentifier identifier, bool isFailed, bool isAllDataReceived, const WebCore::IntSize& size, int32_t repetitionCount, Vector<WebCore::ImageDecoderFrameInfo>&& frameInfos)
{
    RefPtr<RemoteImageDecoder> remoteImageDecoder;
    {
        Locker locker { m_decodersLock };
        remoteImageDecoder = m_decoders.get(identifier).get();
    }
    if (!remoteImageDecoder)
        return;

    remoteImageDecoder->encodedDataStatusChanged(isFailed, isAllDataReceived, size, repetitionCount, WTF::move(frameInfos));
}

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
