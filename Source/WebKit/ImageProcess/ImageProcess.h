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

#include "AuxiliaryProcess.h"
#include "ImageDecodingQueue.h"
#include <WebCore/ProcessIdentifier.h>
#include <wtf/MemoryPressureHandler.h>
#include <wtf/MonotonicTime.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Threading.h>

namespace WebKit {

class ImageConnectionToWebProcess;

// The Image process. Decodes images for Web processes out-of-process, for
// security: image decoders parse attacker-controlled bytes, so keeping them
// out of the Web process limits the blast radius of a decoder exploit.
//
// One Image process is shared by all Web processes of the UI process, with
// one IPC connection per Web process. All decoder work is executed on the
// process-wide ImageDecodingQueue (a pool of decoding threads), so decoding
// never runs on, or blocks, the main run loop. All communication is
// asynchronous.
class ImageProcess final
    : public AuxiliaryProcess
    , public ThreadSafeRefCounted<ImageProcess> {
    WTF_MAKE_NONCOPYABLE(ImageProcess);
    WTF_MAKE_TZONE_ALLOCATED(ImageProcess);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ImageProcess);
public:
    ~ImageProcess();

    static ImageProcess& singleton();

    static constexpr WTF::AuxiliaryProcessType processType = WTF::AuxiliaryProcessType::Image;

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    void removeImageConnectionToWebProcess(ImageConnectionToWebProcess&);

    ImageDecodingQueue& decodingQueue() { return m_decodingQueue.get(); }

    void prepareToSuspend(bool isSuspensionImminent, MonotonicTime estimatedSuspendTime, CompletionHandler<void()>&&);
    void processDidResume();

private:
    ImageProcess();

    void lowMemoryHandler(Critical, Synchronous);

    // AuxiliaryProcess
    void initializeConnection(IPC::Connection*) override;
    void initializeProcess(const AuxiliaryProcessInitializationParameters&) override;
    Thread::QOS connectionReceiveQueueQOS() const override { return Thread::QOS::UserInitiated; }
    bool shouldTerminate() override;

    // IPC::Connection::Client
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    // Message Handlers
    void createImageConnectionToWebProcess(WebCore::ProcessIdentifier, IPC::Connection::Handle&&, CompletionHandler<void()>&&);

    // Connections to Web processes.
    HashMap<WebCore::ProcessIdentifier, Ref<ImageConnectionToWebProcess>> m_webProcessConnections;

    Ref<ImageDecodingQueue> m_decodingQueue;
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
