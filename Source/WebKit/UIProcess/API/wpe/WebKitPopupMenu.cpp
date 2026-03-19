/*
 * Copyright (C) 2020 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "WebKitPopupMenu.h"

#include "APIViewClient.h"
#include "WPEWebView.h"
#include "WebPopupItem.h"
#include "WebKitOptionMenuPrivate.h"
#include "WebKitWebViewClient.h"
#include <WebCore/IntRect.h>
#include <wpe/WPEPopupMenu.h>
#include <wpe/WPEView.h>
#include <wtf/text/WTFString.h>

namespace WebKit {
using namespace WebCore;

Ref<WebKitPopupMenu> WebKitPopupMenu::create(WKWPE::View& view, WebPopupMenuProxy::Client& client)
{
    ASSERT(view.client().isGLibBasedAPI());
    return adoptRef(*new WebKitPopupMenu(view, client));
}

WebKitPopupMenu::WebKitPopupMenu(WKWPE::View& view, WebPopupMenuProxy::Client& client)
    : WebPopupMenuProxy(client)
    , m_view(view)
{
}

static void menuCloseCallback(WebKitPopupMenu* popupMenu)
{
    popupMenu->activateItem(std::nullopt);
}

static void wpePopupMenuItemSelectedCallback(WebKitPopupMenu* self, guint index)
{
    self->selectItem(index);
}

static void wpePopupMenuItemActivatedCallback(WebKitPopupMenu* self, gint index)
{
    self->activateItem(index >= 0 ? std::optional<unsigned>(index) : std::nullopt);
}

void WebKitPopupMenu::showPopupMenu(const IntRect& rect, TextDirection direction, double pageScaleFactor, const Vector<WebPopupItem>& items, const PlatformPopupMenuData& platformData, int32_t selectedIndex)
{
    if (auto* wpeView = m_view.wpeView()) {
        GSList* menuItems = nullptr;
        for (size_t i = 0; i < items.size(); i++) {
            const auto& item = items[i];
            WPETextDirection textDirection = WPE_TEXT_DIRECTION_DEFAULT;
            if (item.m_hasTextDirectionOverride)
                textDirection = item.m_textDirection == TextDirection::RTL ? WPE_TEXT_DIRECTION_RTL : WPE_TEXT_DIRECTION_LTR;
            WPEPopupMenuItemType itemType;
            if (item.m_type == WebPopupItem::Type::Separator)
                itemType = WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR;
            else if (item.m_isLabel)
                itemType = WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP;
            else if (item.m_text.startsWith("    "_s)) // Group children are identified by 4-space indent, matching WebKitOptionMenu.
                itemType = WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP_CHILD;
            else
                itemType = WPE_POPUP_MENU_ITEM_TYPE_OPTION;
            menuItems = g_slist_append(menuItems, wpe_popup_menu_item_new(
                itemType == WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR ? nullptr : item.m_text.trim(deprecatedIsSpaceOrNewline).utf8().data(),
                item.m_toolTip.isEmpty() ? nullptr : item.m_toolTip.utf8().data(),
                item.m_accessibilityText.isEmpty() ? nullptr : item.m_accessibilityText.utf8().data(),
                item.m_isEnabled, itemType, textDirection));
        }

        WPERectangle wpeRect { rect.x(), rect.y(), rect.width(), rect.height() };
        auto wpeMenu = adoptGRef(wpe_popup_menu_new(menuItems, &wpeRect, selectedIndex));
        g_slist_free_full(menuItems, reinterpret_cast<GDestroyNotify>(wpe_popup_menu_item_free));
        g_signal_connect_swapped(wpeMenu.get(), "item-selected", G_CALLBACK(wpePopupMenuItemSelectedCallback), this);
        g_signal_connect_swapped(wpeMenu.get(), "item-activated", G_CALLBACK(wpePopupMenuItemActivatedCallback), this);
        if (wpe_view_show_popup_menu(wpeView, wpeMenu.get())) {
            m_wpePlatformMenu = wpeMenu;
            g_signal_connect_swapped(wpeMenu.get(), "close", G_CALLBACK(+[](WebKitPopupMenu* self) {
                if (!self->m_wpePlatformMenu)
                    return;
                auto menu = WTF::move(self->m_wpePlatformMenu);
                g_signal_handlers_disconnect_matched(menu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, self);
                // Platform dismissed without selection.
                self->cancelTracking();
            }), this);
            return;
        }
        g_signal_handlers_disconnect_matched(wpeMenu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    }

    GRefPtr<WebKitOptionMenu> menu = static_cast<WebKitWebViewClient&>(m_view.client()).showOptionMenu(*this, rect, items, selectedIndex);
    if (menu) {
        m_menu = WTF::move(menu);
        g_signal_connect_swapped(m_menu.get(), "close", G_CALLBACK(menuCloseCallback), this);
    }
}

void WebKitPopupMenu::hidePopupMenu()
{
    if (m_wpePlatformMenu) {
        g_signal_handlers_disconnect_matched(m_wpePlatformMenu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        wpe_popup_menu_close(m_wpePlatformMenu.get());
        m_wpePlatformMenu = nullptr;
        return;
    }
    if (m_menu) {
        g_signal_handlers_disconnect_matched(m_menu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        webkit_option_menu_close(m_menu.get());
    }
}

void WebKitPopupMenu::cancelTracking()
{
    hidePopupMenu();
    m_menu = nullptr;
}

void WebKitPopupMenu::selectItem(unsigned itemIndex)
{
    if (CheckedPtr client = this->client())
        client->setTextFromItemForPopupMenu(this, itemIndex);
    m_selectedItem = itemIndex;
}

void WebKitPopupMenu::activateItem(std::optional<unsigned> itemIndex)
{
    if (CheckedPtr client = this->client())
        client->valueChangedForPopupMenu(this, itemIndex.value_or(m_selectedItem.value_or(-1)));
    if (m_wpePlatformMenu) {
        g_signal_handlers_disconnect_matched(m_wpePlatformMenu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        m_wpePlatformMenu = nullptr;
    }
    if (m_menu) {
        g_signal_handlers_disconnect_matched(m_menu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        m_menu = nullptr;
    }
}

} // namespace WebKit
