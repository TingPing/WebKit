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
#include "ImageProcessConnection.h"

#if ENABLE(IMAGE_PROCESS)

#include "Logging.h"
#include "WebProcess.h"
#include "WebProcessProxyMessages.h"

namespace WebKit {

RefPtr<ImageProcessConnection> ImageProcessConnection::create(IPC::Connection& parentConnection)
{
    auto connectionIdentifiers = IPC::Connection::createConnectionIdentifierPair();
    if (!connectionIdentifiers)
        return nullptr;

    parentConnection.send(Messages::WebProcessProxy::CreateImageProcessConnection(WTF::move(connectionIdentifiers->client)), 0, IPC::SendOption::DispatchMessageEvenWhenWaitingForSyncReply);

    return adoptRef(*new ImageProcessConnection(WTF::move(connectionIdentifiers->server)));
}

ImageProcessConnection::ImageProcessConnection(IPC::Connection::Identifier&& connectionIdentifier)
    : m_connection(IPC::Connection::createServerConnection(WTF::move(connectionIdentifier)))
{
    m_connection->open(*this);
}

ImageProcessConnection::~ImageProcessConnection()
{
    m_connection->invalidate();
}

void ImageProcessConnection::didClose(IPC::Connection&)
{
    RELEASE_LOG(Process, "%p - ImageProcessConnection::didClose", this);
    Ref protector { *this };
    WebProcess::singleton().imageProcessConnectionClosed(*this);

    m_clients.forEach([this](auto& client) {
        client.imageProcessConnectionDidClose(*this);
    });
    m_clients.clear();
}

void ImageProcessConnection::didReceiveInvalidMessage(IPC::Connection&, IPC::MessageName, const Vector<uint32_t>&)
{
}

void ImageProcessConnection::didReceiveMessage(IPC::Connection& connection, IPC::Decoder& decoder)
{
    m_messageReceiverMap.dispatchMessage(connection, decoder);
}

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
