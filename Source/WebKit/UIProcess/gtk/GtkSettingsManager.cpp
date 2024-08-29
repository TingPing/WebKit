/*
 * Copyright (C) 2021 Purism SPC
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

#include "config.h"
#include "GtkSettingsManager.h"

#include "SystemSettingsProxyGLibMessages.h"
#include "WebProcessPool.h"

namespace WebKit {
using namespace WebCore;

void GtkSettingsManager::initialize()
{
    static NeverDestroyed<GtkSettingsManager> manager;
}

String GtkSettingsManager::themeName() const
{
    if (auto* themeNameEnv = g_getenv("GTK_THEME")) {
        String name = String::fromUTF8(themeNameEnv);
        if (name.endsWith("-dark"_s) || name.endsWith("-Dark"_s) || name.endsWith(":dark"_s))
            return name.left(name.length() - 5);
        return name;
    }

    GUniqueOutPtr<char> themeNameSetting;
    g_object_get(m_settings, "gtk-theme-name", &themeNameSetting.outPtr(), nullptr);
    String name = String::fromUTF8(themeNameSetting.get());
    if (name.endsWith("-dark"_s) || name.endsWith("-Dark"_s))
        return name.left(name.length() - 5);
    return name;
}

String GtkSettingsManager::fontName() const
{
    GUniqueOutPtr<char> fontNameSetting;
    g_object_get(m_settings, "gtk-font-name", &fontNameSetting.outPtr(), nullptr);
    return String::fromUTF8(fontNameSetting.get());
}

int GtkSettingsManager::xftAntialias() const
{
    int antialiasSetting;
    g_object_get(m_settings, "gtk-xft-antialias", &antialiasSetting, nullptr);
    return antialiasSetting;
}

int GtkSettingsManager::xftHinting() const
{
    int hintingSetting;
    g_object_get(m_settings, "gtk-xft-hinting", &hintingSetting, nullptr);
    return hintingSetting;
}

String GtkSettingsManager::xftHintStyle() const
{
    GUniqueOutPtr<char> hintStyleSetting;
    g_object_get(m_settings, "gtk-xft-hintstyle", &hintStyleSetting.outPtr(), nullptr);
    return String::fromUTF8(hintStyleSetting.get());
}

String GtkSettingsManager::xftRGBA() const
{
    GUniqueOutPtr<char> rgbaSetting;
    g_object_get(m_settings, "gtk-xft-rgba", &rgbaSetting.outPtr(), nullptr);
    return String::fromUTF8(rgbaSetting.get());
}

int GtkSettingsManager::xftDPI() const
{
    int dpiSetting;
    g_object_get(m_settings, "gtk-xft-dpi", &dpiSetting, nullptr);
    return dpiSetting;
}

bool GtkSettingsManager::cursorBlink() const
{
    gboolean cursorBlinkSetting;
    g_object_get(m_settings, "gtk-cursor-blink", &cursorBlinkSetting, nullptr);
    return cursorBlinkSetting ? true : false;
}

int GtkSettingsManager::cursorBlinkTime() const
{
    int cursorBlinkTimeSetting;
    g_object_get(m_settings, "gtk-cursor-blink-time", &cursorBlinkTimeSetting, nullptr);
    return cursorBlinkTimeSetting;
}

bool GtkSettingsManager::primaryButtonWarpsSlider() const
{
    gboolean buttonSetting;
    g_object_get(m_settings, "gtk-primary-button-warps-slider", &buttonSetting, nullptr);
    return buttonSetting ? true : false;
}

bool GtkSettingsManager::overlayScrolling() const
{
    gboolean overlayScrollingSetting;
    g_object_get(m_settings, "gtk-overlay-scrolling", &overlayScrollingSetting, nullptr);
    return overlayScrollingSetting ? true : false;
}

bool GtkSettingsManager::enableAnimations() const
{
    gboolean enableAnimationsSetting;
    g_object_get(m_settings, "gtk-enable-animations", &enableAnimationsSetting, nullptr);
    return enableAnimationsSetting ? true : false;
}

void GtkSettingsManager::settingsDidChange()
{
    auto& oldState = SystemSettingsGLib::singleton().settingsState();
    SettingsStateGLib changedState;

    auto themeName = this->themeName();
    if (oldState.themeName != themeName)
        changedState.themeName = themeName;

    auto fontName = this->fontName();
    if (oldState.fontName != fontName)
        changedState.fontName = fontName;

    auto xftAntialias = this->xftAntialias();
    if (xftAntialias != -1 && oldState.xftAntialias != xftAntialias)
        changedState.xftAntialias = xftAntialias;

    auto xftHinting = this->xftHinting();
    if (xftHinting != -1 && oldState.xftHinting != xftHinting)
        changedState.xftHinting = xftHinting;

    auto xftDPI = this->xftDPI();
    if (xftDPI != -1 && oldState.xftDPI != xftDPI)
        changedState.xftDPI = xftDPI;

    auto xftHintStyle = this->xftHintStyle();
    if (oldState.xftHintStyle != xftHintStyle)
        changedState.xftHintStyle = xftHintStyle;

    auto xftRGBA = this->xftRGBA();
    if (oldState.xftRGBA != xftRGBA)
        changedState.xftRGBA = xftRGBA;


    auto cursorBlink = this->cursorBlink();
    if (oldState.cursorBlink != cursorBlink)
        changedState.cursorBlink = cursorBlink;

    auto cursorBlinkTime = this->cursorBlinkTime();
    if (oldState.cursorBlinkTime != cursorBlinkTime)
        changedState.cursorBlinkTime = cursorBlinkTime;

    auto primaryButtonWarpsSlider = this->primaryButtonWarpsSlider();
    if (oldState.primaryButtonWarpsSlider != primaryButtonWarpsSlider)
        changedState.primaryButtonWarpsSlider = primaryButtonWarpsSlider;

    auto overlayScrolling = this->overlayScrolling();
    if (oldState.overlayScrolling != overlayScrolling)
        changedState.overlayScrolling = overlayScrolling;

    auto enableAnimations = this->enableAnimations();
    if (oldState.enableAnimations != enableAnimations)
        changedState.enableAnimations = enableAnimations;

    for (auto& processPool : WebProcessPool::allProcessPools())
        processPool->sendToAllProcesses(Messages::SystemSettingsProxyGLib::SettingsDidChange(changedState));

    SystemSettingsGLib::singleton().applySettings(WTFMove(changedState));
}

GtkSettingsManager::GtkSettingsManager()
    : m_settings(gtk_settings_get_default())
{
    auto settingsChangedCallback = +[](GtkSettingsManager* settingsManager) {
        settingsManager->settingsDidChange();
    };

    g_signal_connect_swapped(m_settings, "notify::gtk-theme-name", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-font-name", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-xft-antialias", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-xft-dpi", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-xft-hinting", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-xft-hintstyle", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-xft-rgba", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-cursor-blink", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-cursor-blink-time", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-primary-button-warps-slider", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-overlay-scrolling", G_CALLBACK(settingsChangedCallback), this);
    g_signal_connect_swapped(m_settings, "notify::gtk-enable-animations", G_CALLBACK(settingsChangedCallback), this);

    settingsDidChange();
}

} // namespace WebKit
