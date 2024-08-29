/*
 * Copyright (C) 2024 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "SettingsStateGLib.h"

#include <optional>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class SystemSettingsGLib {
    WTF_MAKE_NONCOPYABLE(SystemSettingsGLib);
    friend NeverDestroyed<SystemSettingsGLib>;
public:
    static SystemSettingsGLib& singleton();

    void applySettings(SettingsStateGLib&&);
    const SettingsStateGLib& settingsState() { return m_settings; }

    void addObserver(Function<void(const SettingsStateGLib&)>&&, void* context);
    void removeObserver(void* context);

    std::optional<String> themeName() const { return m_settings.themeName; };
    std::optional<String> fontName() const { return m_settings.fontName; };
    std::optional<bool> cursorBlink() const { return m_settings.cursorBlink; };
    std::optional<int> cursorBlinkTime() const { return m_settings.cursorBlinkTime; };
    std::optional<int> xftDPI() const { return m_settings.xftDPI; }
    std::optional<bool> overlayScrolling() const { return m_settings.overlayScrolling; };
    std::optional<bool> primaryButtonWarpsSlider() const { return m_settings.primaryButtonWarpsSlider; };
    std::optional<bool> enableAnimations() const { return m_settings.enableAnimations; };

private:
    SystemSettingsGLib();

    void applyHintingSettings();
    void applyAntialiasSettings();

    SettingsStateGLib m_settings;
    HashMap<void*, Function<void(const WebCore::SettingsStateGLib&)>> m_observers;
};

} // namespace WebCore
