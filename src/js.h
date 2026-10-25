/* Northstar Web Browser — JavaScript runtime API used by the page: scripts, events, focus.
 * Host callbacks for navigation, repaint and layout; form, select and media-control helpers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_JS_H
#define NS_JS_H

#include <glib.h>

#include <cairo.h>

#include "csp.h"
#include "fetch_policy.h"
#include "dom.h"

G_BEGIN_DECLS

#define NS_JS_EVAL_BUDGET_MAX_MS 60000

typedef struct ns_js ns_js;
typedef struct ns_js_drag_session ns_js_drag_session;

typedef void (*ns_js_log_cb)(const char *line, gpointer user_data);
typedef void (*ns_js_mutated_cb)(gpointer user_data);
typedef void (*ns_js_navigate_cb)(const char *url, gboolean reload, gpointer user_data);
typedef void (*ns_js_download_cb)(const char *url, const char *filename, gpointer user_data);
typedef void (*ns_js_scroll_to_cb)(const ns_node *target, gpointer user_data);
typedef void (*ns_js_fragment_nav_cb)(const char *url, gpointer user_data);
typedef void (*ns_js_form_submit_cb)(const ns_node *form, const ns_node *submitter,
                                     gpointer user_data);
typedef void (*ns_js_soft_nav_cb)(const char *url, gboolean replace, gpointer user_data);
typedef void (*ns_js_repaint_cb)(gpointer user_data);
typedef void (*ns_js_repaint_node_cb)(const ns_node *node, gpointer user_data);
typedef void (*ns_js_viewport_scroll_cb)(double *x, double *y,
                                         gpointer user_data);
typedef void (*ns_js_layout_flush_cb)(gpointer user_data);
typedef gboolean (*ns_js_clipboard_write_cb)(const char *text, gpointer user_data);
typedef gboolean (*ns_js_selection_cmd_cb)(const char *command, gpointer user_data);
typedef void (*ns_js_window_action_cb)(const char *action, gpointer user_data);

typedef struct {
    gint64 origin_us;
    double origin_real_ms;
    double domain_lookup_start_ms;
    double domain_lookup_end_ms;
    double connect_start_ms;
    double connect_end_ms;
    double secure_connection_start_ms;
    double request_start_ms;
    double response_start_ms;
    double response_end_ms;
    double dom_loading_ms;
    double dom_interactive_ms;
    double dom_content_loaded_event_start_ms;
    double dom_content_loaded_event_end_ms;
    double dom_complete_ms;
    double load_event_start_ms;
    double load_event_end_ms;
} ns_js_navigation_timing;

typedef struct ns_response ns_response;

ns_js_navigation_timing
ns_js_navigation_timing_from_response(const ns_response *response);

ns_js *ns_js_new(ns_js_log_cb      log_cb,  gpointer log_user_data,
                 ns_js_mutated_cb  mut_cb,  gpointer mut_user_data,
                 ns_js_navigate_cb nav_cb,  gpointer nav_user_data,
                 const ns_js_navigation_timing *navigation_timing);

void   ns_js_set_form_submit_cb(ns_js *js, ns_js_form_submit_cb cb, gpointer user_data);
void   ns_js_set_download_cb(ns_js *js, ns_js_download_cb cb, gpointer user_data);
void   ns_js_set_selection(ns_js *js, const char *text, gboolean has_range,
                           double x, double y, double w, double h);
void   ns_js_set_clipboard_write_cb(ns_js *js, ns_js_clipboard_write_cb cb,
                                    gpointer user_data);
void   ns_js_set_selection_cmd_cb(ns_js *js, ns_js_selection_cmd_cb cb,
                                  gpointer user_data);
void   ns_js_set_scroll_to_cb(ns_js *js, ns_js_scroll_to_cb cb,
                              gpointer user_data);
void   ns_js_set_fragment_nav_cb(ns_js *js, ns_js_fragment_nav_cb cb,
                                 gpointer user_data);
void   ns_js_set_soft_nav_cb(ns_js *js, ns_js_soft_nav_cb cb,
                             gpointer user_data);
void   ns_js_set_layout_flush_cb(ns_js *js, ns_js_layout_flush_cb cb, gpointer user_data);
void   ns_js_set_repaint_cb(ns_js *js, ns_js_repaint_cb cb, gpointer user_data);
void   ns_js_set_repaint_node_cb(ns_js *js, ns_js_repaint_node_cb cb);
void   ns_js_set_viewport_scroll_cb(ns_js *js, ns_js_viewport_scroll_cb cb,
                                    gpointer user_data);
void   ns_js_set_load_delay_cb(ns_js *js, gboolean (*cb)(gpointer), gpointer user_data);
void   ns_js_set_style_flush_cb(ns_js *js, ns_js_layout_flush_cb cb, gpointer user_data);
void   ns_js_set_early_inject_src(ns_js *js, const char *src);
void   ns_js_add_csp_header(ns_js *js, const char *header_value);
void   ns_js_apply_meta_csp(ns_js *js, const ns_node *doc);
gboolean ns_js_csp_form_action_allowed(const ns_js *js, const char *action_url);
const char *ns_js_current_url(const ns_js *js);
const char *ns_js_storage_partition(const ns_js *js);
void   ns_js_free(ns_js *js);

void     ns_js_run_scripts_in_doc(ns_js *js, ns_node *doc,
                                  const char *base_url, const char *charset,
                                  const char *content_type);

gboolean ns_js_consume_mutated(ns_js *js);
gboolean ns_js_consume_layout_mutated(ns_js *js);
GPtrArray *ns_js_take_layout_targets(ns_js *js);
const char *ns_js_layout_mutated_by(ns_js *js);

char  *ns_js_eval_source(ns_js *js, const char *src, const char *origin);

gboolean ns_js_dispatch_event(ns_js *js, const ns_node *target, const char *type,
                              gboolean *default_prevented);
typedef struct {
    const ns_node *control;
    int            kind;
    gboolean       checked;
    gboolean       indeterminate;
    ns_node       *checked_radio;
} ns_js_click_state;
void     ns_js_click_begin(ns_js *js, const ns_node *node,
                           ns_js_click_state *state);
gboolean ns_js_click_end(ns_js *js, const ns_node *node,
                         const ns_js_click_state *state, gboolean prevented);
gboolean ns_js_form_submission_allowed(ns_js *js, const ns_node *form,
                                       const ns_node *submitter);
gboolean ns_js_form_entry_list(ns_js *js, const ns_node *form,
                               const ns_node *submitter, GString *query,
                               gboolean *first);
gboolean ns_js_select_choose_option(ns_js *js, ns_node *option);
gboolean ns_js_select_toggle_option(ns_js *js, ns_node *option);
gboolean ns_js_select_step(ns_js *js, ns_node *select, int dir);
gboolean ns_js_select_edge(ns_js *js, ns_node *select, gboolean last);
gboolean ns_js_select_typeahead(ns_js *js, ns_node *select, const char *key);
void     ns_js_activate_element(ns_js *js, const ns_node *el);
gboolean ns_js_keyboard_activate(ns_js *js, const ns_node *el,
                                 const char *key, gboolean keyup);
gboolean ns_js_keyboard_activates(const ns_node *el, const char *key);
gboolean ns_js_dispatch_submit_event(ns_js *js, const ns_node *form,
                                     const ns_node *submitter,
                                     gboolean *default_prevented);
void     ns_js_form_reset(ns_js *js, ns_node *form);
gboolean ns_js_activate_summary(ns_js *js, const ns_node *el);

void           ns_js_set_focus(ns_js *js, const ns_node *el);
void           ns_js_note_pointer_input(ns_js *js, gboolean pointer);
void           ns_js_set_focused_node(ns_js *js, const ns_node *el);
const ns_node *ns_js_focused_node(const ns_js *js);
void           ns_js_focus_from_pointer(ns_js *js, const ns_node *target);
const ns_node *ns_js_sequential_focus_target(ns_js *js, gboolean backward);
gboolean       ns_node_is_focusable(const ns_node *el);
gboolean       ns_js_process_close_request(ns_js *js);

void ns_js_details_toggle_open(ns_js *js, ns_node *details, gboolean open);

gboolean ns_js_run_animation_frame(ns_js *js);

gboolean ns_js_has_pending_animation_frame(const ns_js *js);
gboolean ns_js_has_pending_work(const ns_js *js);
gboolean ns_js_wants_frame(const ns_js *js);
ns_fetch_policy *ns_js_fetch_policy(ns_js *js, const ns_node *node);
void     ns_media_scan(ns_js *js, ns_node *doc, const char *base_url);
void     ns_js_suspend_media(ns_js *js);

typedef enum {
    NS_MEDIA_CONTROL_NONE,
    NS_MEDIA_CONTROL_PLAY,
    NS_MEDIA_CONTROL_SEEK,
    NS_MEDIA_CONTROL_MUTE,
} ns_media_control;

typedef struct {
    double x, y, w, h;
    double play_x, play_w;
    double seek_x0, seek_x1;
    double time_x, time_w;
    double mute_x, mute_w;
} ns_media_controls_rect;

typedef struct {
    gboolean paused;
    gboolean muted;
    gboolean audible;
    double   position;
    double   duration;
} ns_media_controls_state;

gboolean ns_media_controls_layout(const ns_node *el, double x, double y,
                                  double w, double h,
                                  ns_media_controls_rect *out);
ns_media_control ns_media_controls_hit(const ns_media_controls_rect *r,
                                       double px, double py);
void     ns_media_controls_get(ns_js *js, const ns_node *el,
                               ns_media_controls_state *out);
void     ns_media_controls_activate(ns_js *js, const ns_node *el,
                                    ns_media_control control,
                                    double fraction);
void     ns_js_set_frame_time(ns_js *js, gint64 frame_time_us);

void ns_js_dump_stats(ns_js *js, GString *out);

typedef struct ns_anim ns_anim;
void ns_js_dispatch_anim_events(ns_js *js, ns_anim *anim);

void     ns_js_set_style_table(ns_js *js, GHashTable *styles);
void     ns_js_sync_window_metrics(ns_js *js);
void     ns_js_dispatch_resize(ns_js *js);
void     ns_js_note_viewport_scroll(ns_js *js, double x, double y);

struct ns_box;
void     ns_js_set_layout_root(ns_js *js, const struct ns_box *root);
void     ns_js_fire_media_load_events(ns_js *js, const struct ns_box *layout);
void     ns_js_fire_page_transition(ns_js *js, const char *type,
                                    gboolean persisted);

cairo_surface_t *ns_js_canvas_surface(ns_js *js, const ns_node *n);

void ns_js_request_repaint(ns_js *js);
void ns_js_request_repaint_node(ns_js *js, const ns_node *node);

struct ns_image_cache;
struct ns_anim;
void ns_js_set_anim(ns_js *js, struct ns_anim *anim);
struct ns_image;
void ns_js_set_image_cache(ns_js *js, struct ns_image_cache *cache);
const struct ns_image *ns_js_image_for_node(ns_js *js, const ns_node *el);

gboolean ns_js_dispatch_key_event(ns_js *js, const ns_node *target,
                                  const char *type,
                                  const char *key, const char *code, int key_code,
                                  gboolean shift, gboolean ctrl,
                                  gboolean alt,   gboolean meta,
                                  gboolean *default_prevented);
gboolean ns_js_dispatch_key_event_full(ns_js *js, const ns_node *target,
                                       const char *type,
                                       const char *key, const char *code,
                                       int key_code, int char_code,
                                       gboolean shift, gboolean ctrl,
                                       gboolean alt,   gboolean meta,
                                       gboolean *default_prevented);

gboolean ns_js_dispatch_mouse_event(ns_js *js, const ns_node *target,
                                    const char *type,
                                    double client_x, double client_y,
                                    double page_x, double page_y,
                                    int button, int buttons,
                                    gboolean shift, gboolean ctrl,
                                    gboolean alt,   gboolean meta,
                                    const ns_node *related,
                                    gboolean *default_prevented);

ns_js_drag_session *ns_js_drag_session_new(ns_js *js);
void                ns_js_drag_session_free(ns_js_drag_session *session);
void                ns_js_drag_session_set_data(ns_js_drag_session *session,
                                                const char *type,
                                                const char *data);
void                ns_js_drag_session_add_file(ns_js_drag_session *session,
                                                const char *path);
gboolean            ns_js_dispatch_drag_event(ns_js *js, ns_js_drag_session *session,
                                              const ns_node *target,
                                              const char *type,
                                              double client_x, double client_y,
                                              double page_x, double page_y,
                                              int button, int buttons,
                                              gboolean shift, gboolean ctrl,
                                              gboolean alt, gboolean meta,
                                              const ns_node *related,
                                              gboolean *default_prevented);

G_END_DECLS

#endif
