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

#include "ImageProcessConnection.h"
#include "MessageReceiver.h"
#include <WebCore/ImageDecoder.h>
#include <WebCore/ImageDecoderIdentifier.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/ThreadSafeWeakPtr.h>

namespace WebKit {

class RemoteImageDecoder;

// Process-lifetime manager for the image decoding that happens in the Image
// process. Owned by the WebProcess. Installs a WebCore::ImageDecoderFactory
// provider so image decoding for supported formats is routed to the Image
// process, and keeps a registry of the live RemoteImageDecoders so results
// pushed from the Image process can be routed to them.
class RemoteImageDecoderManager final
    : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<RemoteImageDecoderManager>
    , public ImageProcessConnection::Client
    , private IPC::MessageReceiver {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RemoteImageDecoderManager);
public:
    static Ref<RemoteImageDecoderManager> create();
    virtual ~RemoteImageDecoderManager();

    WTF_ABSTRACT_THREAD_SAFE_REF_COUNTED_AND_CAN_MAKE_WEAK_PTR_IMPL;

    // Installs (or uninstalls) the WebCore image decoder factory provider that
    // routes image decoding to the Image process, following the
    // ImageProcessDecodingEnabled preference. Only decoders created afterwards
    // are affected; images already decoding keep their decoder.
    void setImageProcessEnabled(bool);

    // WebCore::ImageDecoderFactory::DecoderInterface implementation. Called on
    // the main run loop.
    static bool supportsMediaType(WebCore::ImageDecoder::MediaType);
    RefPtr<WebCore::ImageDecoder> createImageDecoder(WebCore::FragmentedSharedBuffer&, const String& mimeType, WebCore::AlphaOption, WebCore::GammaAndColorProfileOption);

    void registerDecoder(RemoteImageDecoder&);
    void unregisterDecoder(RemoteImageDecoder&);

    // Returns the live connection to the Image process, or nullptr if there is
    // none (it has not been established yet, or the Image process crashed).
    RefPtr<ImageProcessConnection> imageProcessConnectionIfExists() const { return m_imageProcessConnection.get(); }

    // Ensures the connection to the Image process exists. Must be called on the
    // main run loop.
    Ref<ImageProcessConnection> ensureImageProcessConnection();

private:
    RemoteImageDecoderManager();

    // Defined by the generated message receiver code
    // (RemoteImageDecoderManager.messages.in).
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;

    // ImageProcessConnection::Client
    void imageProcessConnectionDidClose(ImageProcessConnection&) final;

    void encodedDataStatusChanged(WebCore::ImageDecoderIdentifier, bool isFailed, bool isAllDataReceived, const WebCore::IntSize&, int32_t repetitionCount, Vector<WebCore::ImageDecoderFrameInfo>&&);
    void frameDecoded(WebCore::ImageDecoderIdentifier, uint64_t index, std::optional<WebCore::ShareableBitmapHandle>&&, bool succeeded, bool isComplete, const WebCore::ImageDecoderFrameInfo&);

    // Connection to the Image process, owned by the WebProcess.
    ThreadSafeWeakPtr<ImageProcessConnection> m_imageProcessConnection;

    bool m_isImageProcessEnabled { false };

    // Registry of live decoders. Registered on the main run loop (decoder
    // creation); unregistered on any thread (decoder destruction). Used to
    // route pushes from the Image process; entries are weak so they never
    // extend decoder lifetimes.
    mutable Lock m_decodersLock;
    HashMap<WebCore::ImageDecoderIdentifier, ThreadSafeWeakPtr<RemoteImageDecoder>> m_decoders WTF_GUARDED_BY_LOCK(m_decodersLock);
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
