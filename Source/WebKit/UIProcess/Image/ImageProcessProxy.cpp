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
#include "ImageProcessProxy.h"

#if ENABLE(IMAGE_PROCESS)

#include "ImageProcessMessages.h"
#include "ImageProcessProxyMessages.h"
#include "Logging.h"
#include "WebProcessPool.h"
#include "WebProcessProxy.h"

#define MESSAGE_CHECK(assertion) MESSAGE_CHECK_BASE(assertion, connection())

namespace WebKit {

static WeakPtr<ImageProcessProxy>& singleton()
{
    static NeverDestroyed<WeakPtr<ImageProcessProxy>> singleton;
    return singleton;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageProcessProxy);

Ref<ImageProcessProxy> ImageProcessProxy::getOrCreate()
{
    ASSERT(RunLoop::isMain());
    if (auto& existingImageProcess = singleton()) {
        ASSERT(existingImageProcess->state() != State::Terminated);
        return *existingImageProcess;
    }

    Ref imageProcess = adoptRef(*new ImageProcessProxy);
    singleton() = imageProcess;
    return imageProcess;
}

ImageProcessProxy::ImageProcessProxy()
    : AuxiliaryProcessProxy("ImageProcess"_s, ShouldTakeUIBackgroundAssertion::No)
{
    connect();
}

ImageProcessProxy::~ImageProcessProxy() = default;

void ImageProcessProxy::getLaunchOptions(ProcessLauncher::LaunchOptions& launchOptions)
{
    launchOptions.processType = ProcessLauncher::ProcessType::Image;
    AuxiliaryProcessProxy::getLaunchOptions(launchOptions);
}

void ImageProcessProxy::processWillShutDown(IPC::Connection& connection)
{
    ASSERT_UNUSED(connection, &this->connection() == &connection);
}

void ImageProcessProxy::createImageProcessConnection(WebProcessProxy& webProcessProxy, IPC::Connection::Handle&& connectionIdentifier)
{
    MESSAGE_CHECK(webProcessProxy.state() != WebProcessProxy::State::Terminated);

    startResponsivenessTimer(UseLazyStop::No);

    sendWithAsyncReply(
        Messages::ImageProcess::CreateImageConnectionToWebProcess {
            webProcessProxy.coreProcessIdentifier(),
            WTF::move(connectionIdentifier)
        },
        [this, weakThis = WeakPtr { *this }]() mutable {
            if (!weakThis)
                return;
            stopResponsivenessTimer();
        }, 0, IPC::SendOption::DispatchMessageEvenWhenWaitingForSyncReply);
}

void ImageProcessProxy::terminateWebProcess(WebCore::ProcessIdentifier webProcessIdentifier, IPC::MessageName invalidMessageName)
{
    if (auto process = WebProcessProxy::processForIdentifier(webProcessIdentifier)) {
        MESSAGE_CHECK(process->state() != WebProcessProxy::State::Terminated);
        process->requestTermination(ProcessTerminationReason::RequestedByImageProcess, invalidMessageName);
    }
}

void ImageProcessProxy::imageProcessExited(ProcessTerminationReason reason)
{
    Ref protectedThis { *this };

    switch (reason) {
    case ProcessTerminationReason::ExceededMemoryLimit:
    case ProcessTerminationReason::ExceededCPULimit:
    case ProcessTerminationReason::RequestedByClient:
    case ProcessTerminationReason::IdleExit:
    case ProcessTerminationReason::Unresponsive:
    case ProcessTerminationReason::Crash:
        RELEASE_LOG_ERROR(Process, "%p - ImageProcessProxy::imageProcessExited: reason=%" PUBLIC_LOG_STRING, this, processTerminationReasonToString(reason).characters());
        break;
    case ProcessTerminationReason::ExceededProcessCountLimit:
    case ProcessTerminationReason::NavigationSwap:
    case ProcessTerminationReason::RequestedByNetworkProcess:
    case ProcessTerminationReason::RequestedByGPUProcess:
    case ProcessTerminationReason::RequestedByModelProcess:
    case ProcessTerminationReason::RequestedByImageProcess:
    case ProcessTerminationReason::GPUProcessCrashedTooManyTimes:
    case ProcessTerminationReason::ModelProcessCrashedTooManyTimes:
    case ProcessTerminationReason::NonMainFrameWebContentProcessCrash:
        ASSERT_NOT_REACHED();
        break;
    }

    if (singleton() == this)
        singleton() = nullptr;

    for (auto& processPool : WebProcessPool::allProcessPools())
        processPool->imageProcessExited(processID(), reason);
}

void ImageProcessProxy::didClose(IPC::Connection&)
{
    RELEASE_LOG_ERROR(Process, "%p - ImageProcessProxy::didClose:", this);
    imageProcessExited(ProcessTerminationReason::Crash); // May cause |this| to get deleted.
}

void ImageProcessProxy::didReceiveInvalidMessage(IPC::Connection& connection, IPC::MessageName messageName, const Vector<uint32_t>&)
{
    logInvalidMessage(connection, messageName);

    WebProcessPool::didReceiveInvalidMessage(messageName);

    // Terminate the image process.
    terminate(messageName);

    // Since we've invalidated the connection we'll never get a IPC::Connection::Client::didClose
    // callback so we'll explicitly call it here instead.
    didClose(connection);
}

void ImageProcessProxy::didFinishLaunching(ProcessLauncher* launcher, IPC::Connection::Identifier&& connectionIdentifier)
{
    bool didTerminate = !connectionIdentifier;

    AuxiliaryProcessProxy::didFinishLaunching(launcher, WTF::move(connectionIdentifier));

    if (didTerminate) {
        imageProcessExited(ProcessTerminationReason::Crash);
        return;
    }

    beginResponsivenessChecks();
}

void ImageProcessProxy::sendPrepareToSuspend(IsSuspensionImminent isSuspensionImminent, double remainingRunTime, CompletionHandler<void()>&& completionHandler)
{
    sendWithAsyncReply(Messages::ImageProcess::PrepareToSuspend(isSuspensionImminent == IsSuspensionImminent::Yes, MonotonicTime::now() + Seconds(remainingRunTime)), WTF::move(completionHandler), 0, { }, ShouldStartProcessThrottlerActivity::No);
}

void ImageProcessProxy::sendProcessDidResume(ResumeReason)
{
    if (canSendMessage())
        send(Messages::ImageProcess::ProcessDidResume(), 0);
}

void ImageProcessProxy::didBecomeUnresponsive()
{
    RELEASE_LOG_ERROR(Process, "ImageProcessProxy::didBecomeUnresponsive: ImageProcess with PID %d became unresponsive, terminating it", processID());
    terminate();
    imageProcessExited(ProcessTerminationReason::Unresponsive);
}

} // namespace WebKit

#undef MESSAGE_CHECK

#endif // ENABLE(IMAGE_PROCESS)
