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
#include "SystemdScopeManager.h"

#if USE(GLIB)

#include <gio/gio.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace WebKit {

static constexpr const char* kSystemdBusName = "org.freedesktop.systemd1";
static constexpr const char* kSystemdObjectPath = "/org/freedesktop/systemd1";
static constexpr const char* kManagerInterface = "org.freedesktop.systemd1.Manager";

SystemdScopeManager& SystemdScopeManager::singleton()
{
    static NeverDestroyed<SystemdScopeManager> manager;
    return manager;
}

SystemdScopeManager::SystemdScopeManager()
{
    GUniqueOutPtr<GError> error;
    m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error.outPtr());
    if (!m_connection) {
        WTFLogAlways("SystemdScopeManager: Failed to connect to session bus: %s. Process suspension will not use cgroup freezing.", error->message);
        return;
    }
    m_available = true;
}

SystemdScopeManager::~SystemdScopeManager()
{
    if (m_connection)
        g_object_unref(m_connection);
}

String SystemdScopeManager::scopeNameForPID(ProcessID pid)
{
    return makeString("webkit-webprocess-"_s, pid, ".scope"_s);
}

bool SystemdScopeManager::callManagerMethod(const char* methodName, GVariant* parameters)
{
    ASSERT(m_available);
    GUniqueOutPtr<GError> error;
    GRefPtr<GVariant> result = adoptGRef(g_dbus_connection_call_sync(
        m_connection,
        kSystemdBusName,
        kSystemdObjectPath,
        kManagerInterface,
        methodName,
        parameters,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error.outPtr()));
    if (!result) {
        WTFLogAlways("SystemdScopeManager: %s failed: %s", methodName, error->message);
        return false;
    }
    return true;
}

void SystemdScopeManager::ensureScope(ProcessID pid)
{
    if (!m_available)
        return;

    if (m_scopedProcesses.contains(pid))
        return;

    String name = scopeNameForPID(pid);
    CString nameCStr = name.utf8();

    // Build the properties array: PIDs=[pid], Delegate=true
    // PIDs uses the "au" (array of uint32) variant type.
    GVariantBuilder propsBuilder;
    g_variant_builder_init(&propsBuilder, G_VARIANT_TYPE("a(sv)"));

    GVariantBuilder pidsBuilder;
    g_variant_builder_init(&pidsBuilder, G_VARIANT_TYPE("au"));
    g_variant_builder_add(&pidsBuilder, "u", static_cast<guint32>(pid));
    g_variant_builder_add(&propsBuilder, "(sv)", "PIDs", g_variant_builder_end(&pidsBuilder));
    g_variant_builder_add(&propsBuilder, "(sv)", "Delegate", g_variant_new_boolean(TRUE));

    // Empty auxiliary units array.
    GVariantBuilder auxBuilder;
    g_variant_builder_init(&auxBuilder, G_VARIANT_TYPE("a(sa(sv))"));

    GVariant* parameters = g_variant_new("(ssa(sv)a(sa(sv)))",
        nameCStr.data(),
        "fail",
        &propsBuilder,
        &auxBuilder);

    if (callManagerMethod("StartTransientUnit", parameters)) {
        WTFLogAlways("SystemdScopeManager: created scope %s for pid %d", nameCStr.data(), pid);
        m_scopedProcesses.add(pid);
    }
}

void SystemdScopeManager::freeze(ProcessID pid)
{
    if (!m_available || !m_scopedProcesses.contains(pid))
        return;

    if (m_frozenProcesses.contains(pid)) {
        WTFLogAlways("SystemdScopeManager: freeze(%d) skipped — already frozen", pid);
        return;
    }

    String name = scopeNameForPID(pid);
    if (callManagerMethod("FreezeUnit", g_variant_new("(s)", name.utf8().data()))) {
        m_frozenProcesses.add(pid);
        WTFLogAlways("SystemdScopeManager: froze %s", name.utf8().data());
    }
}

void SystemdScopeManager::thaw(ProcessID pid)
{
    if (!m_available || !m_frozenProcesses.contains(pid))
        return;

    m_frozenProcesses.remove(pid);
    String name = scopeNameForPID(pid);
    if (callManagerMethod("ThawUnit", g_variant_new("(s)", name.utf8().data())))
        WTFLogAlways("SystemdScopeManager: thawed %s", name.utf8().data());
}

} // namespace WebKit

#endif // USE(GLIB)
