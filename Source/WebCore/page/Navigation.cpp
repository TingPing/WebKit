/*
 * Copyright (C) 2023 Igalia S.L. All rights reserved.
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
#include "Navigation.h"

#include "Exception.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "HistoryItem.h"
#include "JSNavigationHistoryEntry.h"
#include "MessagePort.h"
#include "NavigationCurrentEntryChangeEvent.h"
#include "ScriptExecutionContext.h"
#include "SerializedScriptValue.h"
#include <optional>
#include <wtf/Assertions.h>
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(Navigation);

Navigation::Navigation(ScriptExecutionContext* context, LocalDOMWindow& window)
    : ContextDestructionObserver(context)
    , LocalDOMWindowProperty(&window)
{
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#initialize-the-navigation-api-entries-for-a-new-document
void Navigation::initializeEntries(const Ref<HistoryItem>& currentItem, Vector<Ref<HistoryItem>>& items)
{
    for (Ref item : items)
        m_entries.append(NavigationHistoryEntry::create(protectedScriptExecutionContext().get(), item));
    m_currentEntryIndex = items.find(currentItem);
}

const Vector<Ref<NavigationHistoryEntry>>& Navigation::entries() const
{
    static NeverDestroyed<Vector<Ref<NavigationHistoryEntry>>> emptyEntries;
    if (hasEntriesAndEventsDisabled())
        return emptyEntries;
    return m_entries;
}

NavigationHistoryEntry* Navigation::currentEntry() const
{
    if (!hasEntriesAndEventsDisabled() && m_currentEntryIndex)
        return m_entries.at(m_currentEntryIndex.value()).ptr();
    return nullptr;
}

Navigation::~Navigation() = default;

ScriptExecutionContext* Navigation::scriptExecutionContext() const
{
    return ContextDestructionObserver::scriptExecutionContext();
}

EventTargetInterface Navigation::eventTargetInterface() const
{
    return NavigationEventTargetInterfaceType;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-api-early-error-result
static void rejectWithError(Ref<DeferredPromise> committed, Ref<DeferredPromise> finished, Exception&& exception)
{
    committed->reject(exception);
    finished->reject(exception);
}

static void rejectWithError(Ref<DeferredPromise> committed, Ref<DeferredPromise> finished, ExceptionCode exceptionCode, const String& errorMessage)
{
    rejectWithError(committed, finished, Exception { exceptionCode, errorMessage });
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-reload
void Navigation::reload(ReloadOptions&&, Ref<DeferredPromise>&& committed, Ref<DeferredPromise>&& finished)
{
    // FIXME: Only a stub to reload for testing.
    window()->frame()->loader().reload();

    // FIXME: keep track of promises to resolve later.
    Ref entry = NavigationHistoryEntry::create(scriptExecutionContext(), { });
    committed->resolve<IDLInterface<NavigationHistoryEntry>>(entry.get());
    finished->resolve<IDLInterface<NavigationHistoryEntry>>(entry.get());
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-navigate
void Navigation::navigate(ScriptExecutionContext& scriptExecutionContext, const String& url, NavigateOptions&& options, Ref<DeferredPromise>&& committed, Ref<DeferredPromise>&& finished)
{
    auto currentURL = scriptExecutionContext.url();
    auto newURL = URL { currentURL, url };

    if (!newURL.isValid()) {
        rejectWithError(committed, finished, ExceptionCode::SyntaxError, "Invalid URL"_s);
        return;
    }

    if (options.history == HistoryBehavior::Auto) {
        if (newURL.protocolIsJavaScript() || currentURL.isAboutBlank())
            options.history = HistoryBehavior::Replace;
        else
            options.history = HistoryBehavior::Push;
    }

    if (options.history == HistoryBehavior::Push && newURL.protocolIsJavaScript()) {
        rejectWithError(committed, finished, ExceptionCode::NotSupportedError, "A \"push\" navigation was explicitly requested, but only a \"replace\" navigation is possible when navigating to a javascript: URL."_s);
        return;
    }

    if (options.state) {
        Vector<RefPtr<MessagePort>> dummyPorts;
        auto serializeResult = SerializedScriptValue::create(*scriptExecutionContext.globalObject(), options.state, { }, dummyPorts, SerializationForStorage::Yes);
        if (serializeResult.hasException()) {
            rejectWithError(committed, finished, serializeResult.releaseException());
            return;
        }
    }

    if (!window()->frame() || !window()->frame()->document()) {
        rejectWithError(committed, finished, ExceptionCode::InvalidStateError, "Invalid state"_s);
        return;
    }

    // FIXME: This is not a proper Navigation API initiated traversal, just a simple load for now.
    window()->frame()->loader().load(FrameLoadRequest(*window()->frame(), newURL));

    // FIXME: keep track of promises to resolve later.
    Ref entry = NavigationHistoryEntry::create(&scriptExecutionContext, newURL);
    committed->resolve<IDLInterface<NavigationHistoryEntry>>(entry.get());
    finished->resolve<IDLInterface<NavigationHistoryEntry>>(entry.get());
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#performing-a-navigation-api-traversal
void Navigation::performTraversal(NavigationHistoryEntry& entry, Ref<DeferredPromise> committed, Ref<DeferredPromise> finished)
{
    // FIXME: This is just a stub that loads a URL for now.
    window()->frame()->loader().load(FrameLoadRequest(*window()->frame(), URL(entry.url())));

    // FIXME: keep track of promises to resolve later.
    committed->resolve<IDLInterface<NavigationHistoryEntry>>(entry);
    finished->resolve<IDLInterface<NavigationHistoryEntry>>(entry);
}

std::optional<Ref<NavigationHistoryEntry>> Navigation::findEntryByKey(const String& key)
{
    auto entryIndex = m_entries.findIf([&key](const Ref<NavigationHistoryEntry> entry) {
        return entry->key() == key;
    });

    if (entryIndex == notFound)
        return std::nullopt;

    return m_entries[entryIndex];
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-traverseto
void Navigation::traverseTo(const String& key, Options&&, Ref<DeferredPromise>&& committed, Ref<DeferredPromise>&& finished)
{
    if (!window()->frame()) {
        rejectWithError(committed, finished, ExceptionCode::InvalidStateError, "Frame is detached"_s);
        return;
    }

    auto current = currentEntry();
    if (current && current->key() == key) {
        committed->resolve<IDLInterface<NavigationHistoryEntry>>(*current);
        finished->resolve<IDLInterface<NavigationHistoryEntry>>(*current);
        return;
    }

    auto entry = findEntryByKey(key);
    if (!entry) {
        rejectWithError(committed, finished, ExceptionCode::InvalidStateError, "Invalid key"_s);
        return;
    }

    performTraversal(*entry, committed, finished);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-back
void Navigation::back(Options&&, Ref<DeferredPromise>&& committed, Ref<DeferredPromise>&& finished)
{
    if (!m_currentEntryIndex.value_or(0)) {
        rejectWithError(committed, finished, ExceptionCode::InvalidStateError, "Cannot go back"_s);
        return;
    }

    performTraversal(m_entries[m_currentEntryIndex.value() - 1], committed, finished);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-forward
void Navigation::forward(Options&&, Ref<DeferredPromise>&& committed, Ref<DeferredPromise>&& finished)
{
    if (!m_currentEntryIndex || m_currentEntryIndex.value() == m_entries.size()) {
        rejectWithError(committed, finished, ExceptionCode::InvalidStateError, "Cannot go forward"_s);
        return;
    }

    performTraversal(m_entries[m_currentEntryIndex.value() + 1], committed, finished);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-updatecurrententry
ExceptionOr<void> Navigation::updateCurrentEntry(JSDOMGlobalObject& globalObject, UpdateCurrentEntryOptions&& options)
{
    if (!window()->frame() || !window()->frame()->document())
        return Exception { ExceptionCode::InvalidStateError };

    auto current = currentEntry();
    if (!current)
        return Exception { ExceptionCode::InvalidStateError };

    auto serializedState = SerializedScriptValue::create(globalObject, options.state, SerializationForStorage::Yes, SerializationErrorMode::Throwing);
    if (!serializedState)
        return { };

    current->setState(WTFMove(serializedState));

    auto currentEntryChangeEvent = NavigationCurrentEntryChangeEvent::create({ "currententrychange"_s }, {
        { false, false, false },
        std::nullopt,
        current
    });
    dispatchEvent(currentEntryChangeEvent);

    return { };
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#has-entries-and-events-disabled
bool Navigation::hasEntriesAndEventsDisabled() const
{
    if (window()->securityOrigin() && window()->securityOrigin()->isOpaque())
        return true;
    return false;
}


} // namespace WebCore
