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
#include "MessageReceiverMap.h"
#include <wtf/AbstractThreadSafeRefCountedAndCanMakeWeakPtr.h>
#include <wtf/ThreadSafeWeakHashSet.h>

namespace WebKit {

class RemoteImageDecoderManager;

// The connection from the WebProcess to the Image process. All communication
// is asynchronous; there are no synchronous messages.
class ImageProcessConnection
    : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<ImageProcessConnection>
    , public IPC::Connection::Client {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ImageProcessConnection);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ImageProcessConnection);
public:
    static RefPtr<ImageProcessConnection> create(IPC::Connection& parentConnection);
    ~ImageProcessConnection();

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    IPC::Connection& connection() { return m_connection.get(); }
    IPC::MessageReceiverMap& messageReceiverMap() LIFETIME_BOUND { return m_messageReceiverMap; }

    class Client : public AbstractThreadSafeRefCountedAndCanMakeWeakPtr {
    public:
        virtual ~Client() = default;

        virtual void imageProcessConnectionDidClose(ImageProcessConnection&) { }
    };
    void addClient(const Client& client) { m_clients.add(client); }

private:
    explicit ImageProcessConnection(IPC::Connection::Identifier&&);

    // IPC::Connection::Client
    void didClose(IPC::Connection&) override;
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;
    void didReceiveInvalidMessage(IPC::Connection&, IPC::MessageName, const Vector<uint32_t>& indicesOfObjectsFailingDecoding) override;

    // The connection from the web process to the image process.
    const Ref<IPC::Connection> m_connection;
    IPC::MessageReceiverMap m_messageReceiverMap;

    ThreadSafeWeakHashSet<Client> m_clients;
};

} // namespace WebKit

#endif // ENABLE(IMAGE_PROCESS)
