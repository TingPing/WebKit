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
#include "ImageProcess.h"

#if ENABLE(IMAGE_PROCESS)

#include "ImageConnectionToWebProcess.h"
#include "ImageProcessMessages.h"
#include "Logging.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WTFProcess.h>

namespace WebKit {

using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageProcess);

ImageProcess::ImageProcess()
    : m_decodingQueue(ImageDecodingQueue::create())
{
    RELEASE_LOG(Process, "%p - ImageProcess::ImageProcess:", this);
}

ImageProcess::~ImageProcess() = default;

ImageProcess& ImageProcess::singleton()
{
    static NeverDestroyed<Ref<ImageProcess>> imageProcess = adoptRef(*new ImageProcess);
    return imageProcess.get().get();
}

[[noreturn]] static void callExit(IPC::Connection*)
{
    terminateProcess(EXIT_SUCCESS);
}

void ImageProcess::initializeConnection(IPC::Connection* connection)
{
    AuxiliaryProcess::initializeConnection(connection);

    // There are no main thread resources to release, so exit straight from the connection queue
    // instead of waiting for the main thread, which may be blocked behind a decode.
    connection->setDidCloseOnConnectionWorkQueueCallback(callExit);
}

void ImageProcess::initializeProcess(const AuxiliaryProcessInitializationParameters&)
{
    auto& memoryPressureHandler = MemoryPressureHandler::singleton();
    memoryPressureHandler.setLowMemoryHandler([this](Critical critical, Synchronous synchronous) {
        lowMemoryHandler(critical, synchronous);
    });
    memoryPressureHandler.install();
}

void ImageProcess::lowMemoryHandler(Critical critical, Synchronous synchronous)
{
    RELEASE_LOG(Process, "ImageProcess::lowMemoryHandler: critical=%d, synchronous=%d", critical == Critical::Yes, synchronous == Synchronous::Yes);

    // Drop the decoded frame caches. This frees the decode-side references to
    // the destination buffers (whose pages are shared with the Web processes).
    for (auto& connection : m_webProcessConnections.values())
        connection->clearAllFrameBufferCaches();
}

bool ImageProcess::shouldTerminate()
{
    return m_webProcessConnections.isEmpty();
}

void ImageProcess::createImageConnectionToWebProcess(WebCore::ProcessIdentifier processIdentifier, IPC::Connection::Handle&& connectionHandle, CompletionHandler<void()>&& completionHandler)
{
    RELEASE_LOG(Process, "%p - ImageProcess::createImageConnectionToWebProcess: processIdentifier=%" PRIu64, this, processIdentifier.toUInt64());

    auto reply = makeScopeExit(WTF::move(completionHandler));
    // If the sender exited before we received the handle, the handle may not be valid.
    if (!connectionHandle)
        return;

    auto newConnection = ImageConnectionToWebProcess::create(*this, processIdentifier, WTF::move(connectionHandle));

    ASSERT(!m_webProcessConnections.contains(processIdentifier));
    m_webProcessConnections.add(processIdentifier, WTF::move(newConnection));
}

void ImageProcess::removeImageConnectionToWebProcess(ImageConnectionToWebProcess& connection)
{
    RELEASE_LOG(Process, "%p - ImageProcess::removeImageConnectionToWebProcess: processIdentifier=%" PRIu64, this, connection.webProcessIdentifier().toUInt64());
    ASSERT(m_webProcessConnections.contains(connection.webProcessIdentifier()));
    m_webProcessConnections.remove(connection.webProcessIdentifier());
}

void ImageProcess::prepareToSuspend(bool isSuspensionImminent, MonotonicTime, CompletionHandler<void()>&& completionHandler)
{
    RELEASE_LOG(ProcessSuspension, "%p - ImageProcess::prepareToSuspend(), isSuspensionImminent: %d", this, isSuspensionImminent);

    lowMemoryHandler(Critical::Yes, Synchronous::Yes);
    completionHandler();
}

void ImageProcess::processDidResume()
{
    RELEASE_LOG(ProcessSuspension, "%p - ImageProcess::processDidResume()", this);
}

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
