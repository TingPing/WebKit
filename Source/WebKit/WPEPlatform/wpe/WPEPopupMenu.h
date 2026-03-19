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

#ifndef WPEPopupMenu_h
#define WPEPopupMenu_h

#if !defined(__WPE_PLATFORM_H_INSIDE__) && !defined(BUILDING_WEBKIT)
#error "Only <wpe/wpe-platform.h> can be included directly."
#endif

#include <wpe/WPEDefines.h>
#include <wpe/WPERectangle.h>

G_BEGIN_DECLS


/**
 * WPEPopupMenuItemType:
 * @WPE_POPUP_MENU_ITEM_TYPE_OPTION: A regular selectable option (HTML `<option>`).
 * @WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP: A non-selectable group header (HTML `<optgroup>`).
 * @WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP_CHILD: A selectable option that is a child of a group.
 * @WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR: A visual separator with no label or action (HTML `<hr>`).
 *
 * The type of a popup menu item.
 *
 * Since: 2.54
 */
typedef enum {
    WPE_POPUP_MENU_ITEM_TYPE_OPTION,
    WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP,
    WPE_POPUP_MENU_ITEM_TYPE_OPTION_GROUP_CHILD,
    WPE_POPUP_MENU_ITEM_TYPE_SEPARATOR,
} WPEPopupMenuItemType;

/**
 * WPETextDirection:
 * @WPE_TEXT_DIRECTION_DEFAULT: No explicit direction override; inherit from platform.
 * @WPE_TEXT_DIRECTION_LTR: Left-to-right text direction.
 * @WPE_TEXT_DIRECTION_RTL: Right-to-left text direction.
 *
 * Text direction of a popup menu item.
 *
 * Since: 2.54
 */
typedef enum {
    WPE_TEXT_DIRECTION_DEFAULT,
    WPE_TEXT_DIRECTION_LTR,
    WPE_TEXT_DIRECTION_RTL
} WPETextDirection;

typedef struct _WPEPopupMenuItem WPEPopupMenuItem;

#define WPE_TYPE_POPUP_MENU_ITEM (wpe_popup_menu_item_get_type())

WPE_API GType             wpe_popup_menu_item_get_type          (void);
WPE_API WPEPopupMenuItem    *wpe_popup_menu_item_new              (const char            *label,
                                                                  const char            *tooltip,
                                                                  const char            *accessibilityText,
                                                                  gboolean               isEnabled,
                                                                  WPEPopupMenuItemType   itemType,
                                                                  WPETextDirection       textDirection);
WPE_API WPEPopupMenuItem *wpe_popup_menu_item_copy           (WPEPopupMenuItem *item);
WPE_API void              wpe_popup_menu_item_free           (WPEPopupMenuItem *item);
WPE_API const char       *wpe_popup_menu_item_get_label      (WPEPopupMenuItem *item);
WPE_API const char       *wpe_popup_menu_item_get_tooltip             (WPEPopupMenuItem *item);
WPE_API const char       *wpe_popup_menu_item_get_accessibility_text  (WPEPopupMenuItem *item);
WPE_API gboolean              wpe_popup_menu_item_is_enabled        (WPEPopupMenuItem *item);
WPE_API WPEPopupMenuItemType  wpe_popup_menu_item_get_item_type     (WPEPopupMenuItem *item);
WPE_API WPETextDirection      wpe_popup_menu_item_get_text_direction (WPEPopupMenuItem *item);

#define WPE_TYPE_POPUP_MENU (wpe_popup_menu_get_type())
WPE_API G_DECLARE_FINAL_TYPE(WPEPopupMenu, wpe_popup_menu, WPE, POPUP_MENU, GObject)

WPE_API WPEPopupMenu     *wpe_popup_menu_new                 (GSList             *items,
                                                              const WPERectangle *rect,
                                                              gint                selectedIndex);
WPE_API guint             wpe_popup_menu_get_n_items         (WPEPopupMenu     *menu);
WPE_API WPEPopupMenuItem *wpe_popup_menu_get_item            (WPEPopupMenu     *menu,
                                                              guint             index);
WPE_API void              wpe_popup_menu_get_rect            (WPEPopupMenu     *menu,
                                                              WPERectangle     *rect);
WPE_API gint              wpe_popup_menu_get_selected_index  (WPEPopupMenu     *menu);
WPE_API void              wpe_popup_menu_select_item         (WPEPopupMenu     *menu,
                                                              guint             index);
WPE_API void              wpe_popup_menu_activate_item       (WPEPopupMenu     *menu,
                                                              gint              index);
WPE_API void              wpe_popup_menu_close               (WPEPopupMenu     *menu);

G_END_DECLS

#endif /* WPEPopupMenu_h */
