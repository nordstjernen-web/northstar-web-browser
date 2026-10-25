/* Northstar Web Browser — page-engine interface for the GTK shell and headless driver.
 * ns_browser handle: open, render, damage, input, scrolling, selection and text/DOM/layout dumps.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_BROWSER_CORE_H
#define NS_BROWSER_CORE_H

#include <stddef.h>

#include <glib.h>

#include "print.h"

typedef struct ns_browser ns_browser;

int ns_browser_init(void);

ns_browser *ns_browser_open_viewport(const char *url, int viewport_width,
                                     double viewport_height, int settle_ms);
ns_browser *ns_browser_open_post_viewport(const char *url, int viewport_width,
                                          double viewport_height,
                                          int settle_ms, const void *body,
                                          size_t body_len,
                                          const char *content_type);
char *ns_browser_take_post(ns_browser *browser, size_t *out_len,
                           char **out_content_type);

char *ns_browser_render_text(ns_browser *browser);
char *ns_browser_dump_dom(ns_browser *browser);
char *ns_browser_dump_layout(ns_browser *browser);
char *ns_browser_dump_performance(ns_browser *browser);
int ns_browser_render_image(ns_browser *browser, const char *path);

GPtrArray *ns_browser_print_pages(ns_browser *browser,
                                  ns_print_setup *out_setup);
int ns_browser_page_size(ns_browser *browser, int *out_width, int *out_height);

int ns_browser_snap_document(ns_browser *browser, double viewport_w,
                             double viewport_h, int prev_x, int prev_y,
                             int *scroll_x, int *scroll_y);
int ns_browser_set_viewport(ns_browser *browser, int css_width,
                            double css_height);
int ns_browser_render_argb32_rects(ns_browser *browser, int scroll_x,
                                   int scroll_y, int width, int height,
                                   int pad, double scale, unsigned char *out,
                                   int stride, const int *rects, int n_rects);
int ns_browser_take_damage(ns_browser *browser, int scroll_x, int scroll_y,
                           GArray *out_rects);
int ns_browser_scroll_blit_bands(ns_browser *browser, GArray *out_bands);

char *ns_browser_link_at(ns_browser *browser, int x, int y);
char *ns_browser_cursor_at(ns_browser *browser, int x, int y);
char *ns_browser_press(ns_browser *browser, int x, int y, int mods);
char *ns_browser_release_click(ns_browser *browser, int *out_changed);
char *ns_browser_key_full(ns_browser *browser, int kind, const char *key,
                          const char *code, int keycode, int mods,
                          int *out_prevented);
int ns_browser_hover(ns_browser *browser, int x, int y);
int ns_browser_scroll_at(ns_browser *browser, int x, int y, int dx, int dy);
int ns_browser_scrollbar_press(ns_browser *browser, int x, int y);
int ns_browser_scrollbar_drag(ns_browser *browser, int x, int y);
void ns_browser_scrollbar_release(ns_browser *browser);
int ns_browser_drop_files(ns_browser *browser, int x, int y,
                          const char *const *paths, int n_paths);
char *ns_browser_select(ns_browser *browser, int kind, int x, int y);
int ns_browser_contextmenu(ns_browser *browser, int x, int y);

int ns_browser_find(ns_browser *browser, const char *query,
                    int case_sensitive, int direction, int from_y,
                    int *out_total, int *out_current, int *out_y);
char *ns_browser_console_drain(ns_browser *browser);
char *ns_browser_eval(ns_browser *browser, const char *src);
char *ns_browser_media_at(ns_browser *browser, int x, int y,
                          int *out_is_video, int *out_stream);
char *ns_browser_take_pending_nav(ns_browser *browser);
/* The document that initiated the pending navigation when it is not the
 * page itself (a form in a cross-origin frame), else NULL.  malloc'd. */
char *ns_browser_take_pending_initiator(ns_browser *browser);
int ns_browser_take_pending_scroll(ns_browser *browser, int *out_scroll_x,
                                   int *out_scroll_y);
char *ns_browser_take_pending_camera(ns_browser *browser);
void ns_browser_resolve_camera(ns_browser *browser, const char *origin,
                               int allow);
char *ns_browser_take_pending_download(ns_browser *browser);
int   ns_browser_has_pending_clipboard(ns_browser *browser);
char *ns_browser_take_pending_clipboard(ns_browser *browser);

int ns_browser_tick(ns_browser *browser, int budget_ms);
int ns_browser_continuous(ns_browser *browser);
int ns_browser_needs_frame(ns_browser *browser);
gint64 ns_browser_next_wake_us(ns_browser *browser);
void ns_browser_set_frame_time(ns_browser *browser, gint64 frame_time_us);
int ns_browser_set_caret_blink_active(ns_browser *browser, int active);
int ns_browser_caret_blinking(ns_browser *browser);
char *ns_browser_title(ns_browser *browser);
char *ns_browser_url(ns_browser *browser);
int   ns_browser_take_url_pushed(ns_browser *browser);
void ns_browser_set_next_navigation(const char *referrer,
                                    int user_activated);
int ns_browser_security(ns_browser *browser, const char **out_ip);
int ns_browser_bfcache_eligible(ns_browser *browser);
void ns_browser_bfcache_park(ns_browser *browser);
void ns_browser_bfcache_restore(ns_browser *browser, int viewport_width,
                                double viewport_height);
void ns_browser_close(ns_browser *browser);

#endif
