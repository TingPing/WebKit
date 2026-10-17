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

#include "AuxiliaryProcessProxy.h"
#include "ProcessLauncher.h"
#include "ProcessTerminationReason.h"
#include <WebCore/ProcessIdentifier.h>

namespace WebKit {

class WebProcessProxy;

// The UIProcess side of the Image process. A single Image process is shared
// by all Web processes of the UI process. It is launched lazily, when a Web
// process first needs to decode an image.
class ImageProcessProxy final : public AuxiliaryProcessProxy {
    WTF_MAKE_TZONE_ALLOCATED(ImageProcessProxy);
    WTF_MAKE_NONCOPYABLE(ImageProcessProxy);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ImageProcessProxy);
public:
    static Ref<ImageProcessProxy> getOrCreate();
    ~ImageProcessProxy();

    void createImageProcessConnection(WebProcessProxy&, IPC::Connection::Handle&&);

private:
    explicit ImageProcessProxy();

    void terminateWebProcess(WebCore::ProcessIdentifier, IPC::MessageName);

    Type type() const final { return Type::Image; }

    // AuxiliaryProcessProxy
    ASCIILiteral processName() const final { return "Image"_s; }

    void getLaunchOptions(ProcessLauncher::LaunchOptions&) final;
    void processWillShutDown(IPC::Connection&) final;

    void imageProcessExited(ProcessTerminationReason);

    // ProcessThrottlerClient
    void sendPrepareToSuspend(IsSuspensionImminent, double remainingRunTime, CompletionHandler<void()>&&) final;
    void sendProcessDidResume(ResumeReason) final;

    // ProcessLauncher::Client
    void didFinishLaunching(ProcessLauncher*, IPC::Connection::Identifier&&) final;

    // IPC::Connection::Client
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;
    void didClose(IPC::Connection&) override;
    void didReceiveInvalidMessage(IPC::Connection&, IPC::MessageName, const Vector<uint32_t>& indicesOfObjectsFailingDecoding) override;

    // ResponsivenessTimer::Client
    void didBecomeUnresponsive() final;
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
