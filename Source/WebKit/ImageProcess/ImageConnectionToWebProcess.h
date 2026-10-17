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

#include "Connection.h"
#include <WebCore/ImageDecoderIdentifier.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/ScalableImageDecoder.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebKit {

class ImageProcess;

// The connection from the Image process to a Web process. Owns the decoders
// created for that Web process. All decoder work is executed on the Image
// process decoding queue (never the main run loop); all communication with
// the Web process is asynchronous.
class ImageConnectionToWebProcess
    : public ThreadSafeRefCounted<ImageConnectionToWebProcess, WTF::DestructionThread::Main>
    , public IPC::Connection::Client {
    WTF_MAKE_NONCOPYABLE(ImageConnectionToWebProcess);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ImageConnectionToWebProcess);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ImageConnectionToWebProcess);
public:
    static Ref<ImageConnectionToWebProcess> create(ImageProcess&, WebCore::ProcessIdentifier, IPC::Connection::Handle&&);
    virtual ~ImageConnectionToWebProcess();

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    IPC::Connection& connection() { return m_connection.get(); }
    ImageProcess& imageProcess() { return m_imageProcess.get(); }
    WebCore::ProcessIdentifier webProcessIdentifier() const { return m_webProcessIdentifier; }

    // Message handlers (ImageConnectionToWebProcess.messages.in)
    void createDecoder(WebCore::ImageDecoderIdentifier, uint8_t alphaOption, uint8_t gammaAndColorProfileOption);
    void setEncodedDataBuffer(WebCore::ImageDecoderIdentifier, WebCore::SharedMemory::Handle&&);
    void encodedDataAppended(WebCore::ImageDecoderIdentifier, uint64_t size, bool allDataReceived);
    void decodeFrame(WebCore::ImageDecoderIdentifier, uint64_t index);
    void cancelDecode(WebCore::ImageDecoderIdentifier, uint64_t index);
    void clearFrameBufferCache(WebCore::ImageDecoderIdentifier, uint64_t index);
    void deleteDecoder(WebCore::ImageDecoderIdentifier);

    // Clears the decoded frame caches of all of this connection's decoders.
    // Frees the decode-side references to the (shared) destination buffers.
    void clearAllFrameBufferCaches();

    // The shared buffers a decoder's frames are decoded into and presented
    // from by the Web process.
    struct FrameBuffers : ThreadSafeRefCounted<FrameBuffers> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FrameBuffers);

        static Ref<FrameBuffers> create() { return adoptRef(*new FrameBuffers); }

        void removeAllExcept(size_t index);
        void clear();

        HashMap<size_t, Ref<WebCore::ShareableBitmap>, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>> buffers;
        // Frames whose pixels the decoder writes into the buffer itself; the
        // others are copied into it after each decode.
        HashSet<size_t, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>> decodedInPlace;
    };

private:
    ImageConnectionToWebProcess(ImageProcess&, WebCore::ProcessIdentifier, IPC::Connection::Handle&&);

    // IPC::Connection::Client. didReceiveMessage() is defined by the generated
    // message receiver code (ImageConnectionToWebProcess.messages.in).
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;
    void didClose(IPC::Connection&) final;
    void didReceiveInvalidMessage(IPC::Connection&, IPC::MessageName, const Vector<uint32_t>& indicesOfObjectsFailingDecoding) final;

    const Ref<IPC::Connection> m_connection;
    const Ref<ImageProcess> m_imageProcess;
    const WebCore::ProcessIdentifier m_webProcessIdentifier;

    // The decoders of this Web process. The map is touched on the main run
    // loop only; decoding queue tasks hold their own references to what they
    // use, and the frame buffers are only touched on the decoder's decoding
    // queue worker.
    struct Decoder {
        RefPtr<WebCore::ScalableImageDecoder> decoder;
        // Written by the Web process.
        RefPtr<WebCore::SharedMemory> encodedData;
        RefPtr<FrameBuffers> frameBuffers;
    };
    HashMap<WebCore::ImageDecoderIdentifier, Decoder> m_decoders;
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
