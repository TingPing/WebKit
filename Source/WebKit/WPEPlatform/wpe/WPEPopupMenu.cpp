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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WPEPopupMenu.h"

#include <wtf/FastMalloc.h>
#include <wtf/text/CString.h>
#include <wtf/Vector.h>
#include <wtf/glib/WTFGType.h>

/**
 * WPEPopupMenuItem:
 *
 * A single item in a #WPEPopupMenu.
 *
 * Since: 2.54
 */
 struct _WPEPopupMenuItem {
    CString label;
    CString tooltip;
    CString accessibilityText;
    WPEPopupMenuItemType itemType { WPE_POPUP_MENU_ITEM_TYPE_OPTION };
    WPETextDirection textDirection { WPE_TEXT_DIRECTION_DEFAULT };
    bool isEnabled { false };
};


/**
 * wpe_popup_menu_item_new:
 * @label: (nullable): the item label text, or %NULL for separators
 * @tooltip: (nullable): the item tooltip, or %NULL
 * @accessibilityText: (nullable): the item accessibility text, or %NULL
 * @isEnabled: whether the item is enabled
 * @itemType: the #WPEPopupMenuItemType of the item
 * @textDirection: the text direction of the item
 *
 * Create a new #WPEPopupMenuItem.
 *
 * Returns: (transfer full): a new #WPEPopupMenuItem
 * Since: 2.54
 */
WPEPopupMenuItem* wpe_popup_menu_item_new(const char* label, const char* tooltip, const char* accessibilityText, gboolean isEnabled, WPEPopupMenuItemType itemType, WPETextDirection textDirection)
{
    auto* item = static_cast<WPEPopupMenuItem*>(fastMalloc(sizeof(WPEPopupMenuItem)));
    new (item) WPEPopupMenuItem();
    if (label)
        item->label = CString(label);
    if (tooltip)
        item->tooltip = CString(tooltip);
    if (accessibilityText)
        item->accessibilityText = CString(accessibilityText);
    item->isEnabled = isEnabled;
    item->itemType = itemType;
    item->textDirection = textDirection;
    return item;
}

/**
 * wpe_popup_menu_item_copy:
 * @item: a #WPEPopupMenuItem
 *
 * Make a copy of @item.
 *
 * Returns: (transfer full): a copy of @item
 */
WPEPopupMenuItem* wpe_popup_menu_item_copy(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, nullptr);

    auto* copy = static_cast<WPEPopupMenuItem*>(fastMalloc(sizeof(WPEPopupMenuItem)));
    new (copy) WPEPopupMenuItem(*item);
    return copy;
}

/**
 * wpe_popup_menu_item_free:
 * @item: a #WPEPopupMenuItem
 *
 * Free @item.
 */
void wpe_popup_menu_item_free(WPEPopupMenuItem* item)
{
    g_return_if_fail(item);

    item->~_WPEPopupMenuItem();
    fastFree(item);
}

G_DEFINE_BOXED_TYPE(WPEPopupMenuItem, wpe_popup_menu_item, wpe_popup_menu_item_copy, wpe_popup_menu_item_free)

/**
 * wpe_popup_menu_item_get_label:
 * @item: a #WPEPopupMenuItem
 *
 * Get the label text of @item.
 *
 * Returns: (transfer none) (nullable): the item label
 */
const char* wpe_popup_menu_item_get_label(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, nullptr);

    return item->label.data();
}

/**
 * wpe_popup_menu_item_get_tooltip:
 * @item: a #WPEPopupMenuItem
 *
 * Get the tooltip of @item.
 *
 * Returns: (transfer none) (nullable): the item tooltip, or %NULL
 */
const char* wpe_popup_menu_item_get_tooltip(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, nullptr);

    return item->tooltip.data();
}

/**
 * wpe_popup_menu_item_get_accessibility_text:
 * @item: a #WPEPopupMenuItem
 *
 * Get the accessibility text of @item.
 *
 * Returns: (transfer none) (nullable): the item accessibility text, or %NULL
 */
const char* wpe_popup_menu_item_get_accessibility_text(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, nullptr);

    return item->accessibilityText.data();
}

/**
 * wpe_popup_menu_item_is_enabled:
 * @item: a #WPEPopupMenuItem
 *
 * Get whether @item is enabled and can be selected.
 *
 * Returns: %TRUE if the item is enabled
 */
gboolean wpe_popup_menu_item_is_enabled(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, FALSE);

    return item->isEnabled;
}

/**
 * wpe_popup_menu_item_get_item_type:
 * @item: a #WPEPopupMenuItem
 *
 * Get the type of @item.
 *
 * Returns: the #WPEPopupMenuItemType of the item
 */
WPEPopupMenuItemType wpe_popup_menu_item_get_item_type(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, WPE_POPUP_MENU_ITEM_TYPE_OPTION);

    return item->itemType;
}

/**
 * wpe_popup_menu_item_get_text_direction:
 * @item: a #WPEPopupMenuItem
 *
 * Get the text direction of @item.
 *
 * Returns: the #WPETextDirection for the item
 */
WPETextDirection wpe_popup_menu_item_get_text_direction(WPEPopupMenuItem* item)
{
    g_return_val_if_fail(item, WPE_TEXT_DIRECTION_DEFAULT);

    return item->textDirection;
}

/**
 * WPEPopupMenu:
 *
 * Represents a popup menu for a `select` HTML element.
 *
 * When a select element in a #WPEView needs to display a popup menu, the
 * signal #WPEView::show-popup-menu is emitted with a #WPEPopupMenu describing
 * the available choices.
 *
 * A platform implementation should connect to this signal, render the given
 * items, and call wpe_popup_menu_select_item() as the user highlights items,
 * then wpe_popup_menu_activate_item() when the user confirms a choice or
 * wpe_popup_menu_close() if the menu is dismissed without selection.
 *
 * Since: 2.50
 */
struct _WPEPopupMenuPrivate {
    Vector<WPEPopupMenuItem> items;
    WPERectangle rect;
    gint selectedIndex;
    bool closed;
};

enum {
    ITEM_SELECTED,
    ITEM_ACTIVATED,
    CLOSE,

    LAST_SIGNAL
};

static std::array<unsigned, LAST_SIGNAL> signals;

WEBKIT_DEFINE_FINAL_TYPE(WPEPopupMenu, wpe_popup_menu, G_TYPE_OBJECT, GObject)

static void wpePopupMenuDispose(GObject* object)
{
    auto* menu = WPE_POPUP_MENU(object);
    menu->priv->items.clear();

    G_OBJECT_CLASS(wpe_popup_menu_parent_class)->dispose(object);
}

static void wpe_popup_menu_class_init(WPEPopupMenuClass* menuClass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(menuClass);
    objectClass->dispose = wpePopupMenuDispose;

    /**
     * WPEPopupMenu::item-selected:
     * @menu: a #WPEPopupMenu
     * @index: the index of the highlighted item
     *
     * Emitted when the user highlights an item without yet confirming the
     * selection, allowing the browser to preview the change.
     *
     * Since: 2.50
     */
    signals[ITEM_SELECTED] = g_signal_new(
        "item-selected",
        G_TYPE_FROM_CLASS(menuClass),
        G_SIGNAL_RUN_LAST,
        0, nullptr, nullptr,
        g_cclosure_marshal_generic,
        G_TYPE_NONE, 1,
        G_TYPE_UINT);

    /**
     * WPEPopupMenu::item-activated:
     * @menu: a #WPEPopupMenu
     * @index: the index of the activated item, or -1 if cancelled
     *
     * Emitted when the user confirms a choice, or -1 if the popup was
     * dismissed without making a selection.
     *
     * Since: 2.50
     */
    signals[ITEM_ACTIVATED] = g_signal_new(
        "item-activated",
        G_TYPE_FROM_CLASS(menuClass),
        G_SIGNAL_RUN_LAST,
        0, nullptr, nullptr,
        g_cclosure_marshal_generic,
        G_TYPE_NONE, 1,
        G_TYPE_INT);

    /**
     * WPEPopupMenu::close:
     * @menu: a #WPEPopupMenu
     *
     * Emitted when the popup menu should be closed. This signal is
     * emitted by the WebKit layer when it wants to dismiss the popup,
     * for example when the page navigates away.
     *
     * Since: 2.50
     */
    signals[CLOSE] = g_signal_new(
        "close",
        G_TYPE_FROM_CLASS(menuClass),
        G_SIGNAL_RUN_LAST,
        0, nullptr, nullptr,
        g_cclosure_marshal_generic,
        G_TYPE_NONE, 0);
}

/**
 * wpe_popup_menu_new:
 * @items: (element-type WPEPopupMenuItem): a #GSList of #WPEPopupMenuItem
 * @rect: the anchor rectangle in view logical coordinates
 * @selectedIndex: the initially selected item index, or -1
 *
 * Create a new #WPEPopupMenu.
 *
 * Returns: (transfer full): a new #WPEPopupMenu
 * Since: 2.50
 */
WPEPopupMenu* wpe_popup_menu_new(GSList* items, const WPERectangle* rect, gint selectedIndex)
{
    g_return_val_if_fail(rect, nullptr);

    auto* menu = WPE_POPUP_MENU(g_object_new(WPE_TYPE_POPUP_MENU, nullptr));
    menu->priv->items.reserveInitialCapacity(g_slist_length(items));
    for (GSList* l = items; l; l = l->next)
        menu->priv->items.append(*static_cast<WPEPopupMenuItem*>(l->data));
    menu->priv->rect = *rect;
    menu->priv->selectedIndex = selectedIndex;
    return menu;
}

/**
 * wpe_popup_menu_get_n_items:
 * @menu: a #WPEPopupMenu
 *
 * Get the number of items in @menu.
 *
 * Returns: the number of items
 */
guint wpe_popup_menu_get_n_items(WPEPopupMenu* menu)
{
    g_return_val_if_fail(WPE_IS_POPUP_MENU(menu), 0);

    return menu->priv->items.size();
}

/**
 * wpe_popup_menu_get_item:
 * @menu: a #WPEPopupMenu
 * @index: the item index
 *
 * Get item at @index in @menu.
 *
 * Returns: (transfer none) (nullable): a #WPEPopupMenuItem, or %NULL if
 *   @index is out of range
 */
WPEPopupMenuItem* wpe_popup_menu_get_item(WPEPopupMenu* menu, guint index)
{
    g_return_val_if_fail(WPE_IS_POPUP_MENU(menu), nullptr);

    if (index >= menu->priv->items.size())
        return nullptr;
    return &menu->priv->items[index];
}

/**
 * wpe_popup_menu_get_rect:
 * @menu: a #WPEPopupMenu
 * @rect: (out): return location for the rectangle
 *
 * Get the rectangle of the element that triggered @menu, in view
 * logical coordinates.
 */
void wpe_popup_menu_get_rect(WPEPopupMenu* menu, WPERectangle* rect)
{
    g_return_if_fail(WPE_IS_POPUP_MENU(menu));
    g_return_if_fail(rect);

    *rect = menu->priv->rect;
}

/**
 * wpe_popup_menu_get_selected_index:
 * @menu: a #WPEPopupMenu
 *
 * Get the index of the currently selected item in @menu.
 *
 * Returns: the selected item index, or -1 if none is selected
 */
gint wpe_popup_menu_get_selected_index(WPEPopupMenu* menu)
{
    g_return_val_if_fail(WPE_IS_POPUP_MENU(menu), -1);

    return menu->priv->selectedIndex;
}

/**
 * wpe_popup_menu_select_item:
 * @menu: a #WPEPopupMenu
 * @index: the item index to highlight
 *
 * Notify the browser that the user is hovering over the item at @index.
 * This allows an instant preview of the selection before it is confirmed.
 * Emits #WPEPopupMenu::item-selected.
 */
void wpe_popup_menu_select_item(WPEPopupMenu* menu, guint index)
{
    g_return_if_fail(WPE_IS_POPUP_MENU(menu));

    g_signal_emit(menu, signals[ITEM_SELECTED], 0, index);
}

/**
 * wpe_popup_menu_activate_item:
 * @menu: a #WPEPopupMenu
 * @index: the index of the chosen item, or -1 to cancel
 *
 * Confirm the selection of the item at @index, or pass -1 to indicate the
 * popup was dismissed without a selection.
 * Emits #WPEPopupMenu::item-activated.
 */
void wpe_popup_menu_activate_item(WPEPopupMenu* menu, gint index)
{
    g_return_if_fail(WPE_IS_POPUP_MENU(menu));

    g_signal_emit(menu, signals[ITEM_ACTIVATED], 0, index);
}

/**
 * wpe_popup_menu_close:
 * @menu: a #WPEPopupMenu
 *
 * Request the platform implementation to close the popup menu. This is
 * called by the WebKit layer when it needs to dismiss the popup, for
 * example because the page has navigated. Emits #WPEPopupMenu::close.
 * Calling this more than once has no effect.
 *
 * Since: 2.54
 */
void wpe_popup_menu_close(WPEPopupMenu* menu)
{
    g_return_if_fail(WPE_IS_POPUP_MENU(menu));

    if (menu->priv->closed)
        return;

    menu->priv->closed = true;
    g_signal_emit(menu, signals[CLOSE], 0);
}
