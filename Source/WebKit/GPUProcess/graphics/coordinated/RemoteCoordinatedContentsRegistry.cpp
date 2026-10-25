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
#include "RemoteCoordinatedContentsRegistry.h"

#if ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER) && USE(GBM)
#include <wtf/NeverDestroyed.h>

namespace WebKit {
using namespace WebCore;

RemoteCoordinatedContentsRegistry& RemoteCoordinatedContentsRegistry::singleton()
{
    static NeverDestroyed<RemoteCoordinatedContentsRegistry> registry;
    return registry;
}

void RemoteCoordinatedContentsRegistry::setFrame(ProcessIdentifier processIdentifier, uint64_t identifier, Ref<DMABufBuffer>&& buffer, UnixFileDescriptor&& fence)
{
    Locker locker { m_lock };
    m_frames.set({ processIdentifier.toUInt64(), identifier }, Entry { WTF::move(buffer), WTF::move(fence) });
}

void RemoteCoordinatedContentsRegistry::remove(ProcessIdentifier processIdentifier, uint64_t identifier)
{
    Locker locker { m_lock };
    m_frames.remove({ processIdentifier.toUInt64(), identifier });
}

void RemoteCoordinatedContentsRegistry::removeAll(ProcessIdentifier processIdentifier)
{
    Locker locker { m_lock };
    m_frames.removeIf([processIdentifier = processIdentifier.toUInt64()](auto& entry) {
        return entry.key.first == processIdentifier;
    });
}

auto RemoteCoordinatedContentsRegistry::takeFrame(ProcessIdentifier processIdentifier, uint64_t identifier) -> std::optional<Frame>
{
    Locker locker { m_lock };
    auto it = m_frames.find({ processIdentifier.toUInt64(), identifier });
    if (it == m_frames.end() || !it->value.buffer)
        return std::nullopt;

    return Frame { *it->value.buffer, std::exchange(it->value.fence, { }) };
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER) && USE(GBM)
