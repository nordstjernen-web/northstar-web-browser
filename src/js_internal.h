/* Northstar Web Browser — internal declarations shared by the QuickJS binding sources.
 * struct ns_js plus canvas, media, MSE, perf, encoding and webcrypto glue; not a public API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_JS_INTERNAL_H
#define NS_JS_INTERNAL_H

#include <glib.h>
#include <cairo.h>
#include "ns_pango.h"
#include "quickjs_compat.h"

#include "js.h"
#include "audio/audio.h"
#include "dom.h"
#include "image.h"
#include "layout.h"

typedef struct ns_worker_host ns_worker_host;

typedef struct ns_canvas_state {
    int w, h;
    cairo_surface_t *surf;
    cairo_t         *cr;
    double fill_r, fill_g, fill_b, fill_a;
    double stroke_r, stroke_g, stroke_b, stroke_a;
    double line_width;
    char  *font;
    cairo_pattern_t *fill_pattern;
    cairo_pattern_t *stroke_pattern;
    double shadow_r, shadow_g, shadow_b, shadow_a;
    double shadow_blur, shadow_ox, shadow_oy;
    gboolean origin_clean;
} ns_canvas_state;

typedef struct ns_perf_observer {
    JSValue   cb;
    JSValue   wrapper;
    gboolean  disconnected;
    gboolean  pinned;
    GPtrArray *entry_types;
    GPtrArray *records;
} ns_perf_observer;

struct ns_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    JSContext    *main_realm_ctx;
    JSContext    *module_ctx;
    GPtrArray    *frame_ctxs;
    GHashTable   *frame_contexts;
    GHashTable   *frame_windows;
    GHashTable   *frame_docs;
    GPtrArray    *broadcast_channels;
    GArray       *font_ready_resolvers;
    ns_js_log_cb  log_cb;
    gpointer      log_user_data;
    ns_js_mutated_cb mut_cb;
    gpointer      mut_user_data;
    ns_js_navigate_cb nav_cb;
    gpointer      nav_user_data;
    ns_js_download_cb download_cb;
    gpointer      download_user_data;
    guint         next_audio_token;
    NsAudioContext *audio_context;
    GHashTable   *media_players;
    GHashTable   *mse_objects;
    GHashTable   *mse_urls;
    guint         next_mse_id;
    GPtrArray    *media_tasks;
    GPtrArray    *media_tasks_flushing;
    guint         media_poll_source;
    guint         media_task_source;
    ns_js_scroll_to_cb scroll_to_cb;
    gpointer      scroll_to_user_data;
    ns_js_fragment_nav_cb fragment_nav_cb;
    gpointer      fragment_nav_user_data;
    ns_js_form_submit_cb form_submit_cb;
    gpointer      form_submit_user_data;
    ns_js_soft_nav_cb soft_nav_cb;
    gpointer      soft_nav_user_data;
    ns_js_repaint_cb repaint_cb;
    ns_js_repaint_node_cb repaint_node_cb;
    ns_js_viewport_scroll_cb viewport_scroll_cb;
    gpointer      viewport_scroll_user_data;
    gpointer      repaint_user_data;
    ns_js_layout_flush_cb layout_flush_cb;
    gpointer      layout_flush_user_data;
    gboolean    (*load_delay_cb)(gpointer user_data);
    gpointer      load_delay_user_data;
    ns_js_layout_flush_cb style_flush_cb;
    gpointer      style_flush_user_data;
    gboolean      in_layout_flush;
    ns_js_clipboard_write_cb clipboard_write_cb;
    gpointer      clipboard_write_user_data;
    ns_js_selection_cmd_cb selection_cmd_cb;
    gpointer      selection_cmd_user_data;
    ns_js_window_action_cb window_action_cb;
    gpointer      window_action_user_data;
    JSValue       history_state;
    int           history_length;
    GPtrArray    *history_entries;
    int           history_pos;
    guint64       nav_key_seq;
    JSValue       navigation;
    char         *current_url;
    char         *document_origin;
    char         *doc_charset;
    ns_node       *current_doc;
    const ns_node *main_document;
    ns_node       *current_script;
    char         *early_inject_src;
    gboolean      mutated;
    gboolean      layout_mutated;
    const char   *layout_mutated_by;
    GHashTable   *timers;
    GMainContext *main_context;
    GPtrArray    *workers;
    ns_worker_host *worker_host;
    int           next_timer_id;
    int           timer_nesting_level;
    int           n_immediate_timers;
    gboolean      running_due_timers;
    GArray       *raf_pending;
    int           next_raf_id;
    ns_node      *raf_frame_ctx;
    JSValue       pristine_promise;
    GHashTable   *style_table;
    const struct ns_box *layout_root;
    guint         layout_font_generation;
    GHashTable   *box_lookup_cache;
    const void   *box_lookup_cache_root;
    const void   *box_lookup_pending_root;
    int           box_lookup_pending_count;
    const ns_node *focused_node;
    gboolean      pointer_input;
    gboolean      autofocus_processed;
    const ns_node *active_modal;
    GPtrArray     *close_watchers;
    GPtrArray     *modal_dialogs;
    ns_node       *dialog_pointerdown;
    GPtrArray     *popover_auto;
    GPtrArray     *popover_hint;
    GHashTable    *popover_info;
    ns_node       *popover_hint_parent;
    ns_node       *popover_pointerdown;
    const ns_node *focus_nav_start;
    gboolean       popover_showing;
    int            popover_hiding_count;
    GArray        *attr_element_refs;
    const ns_node *pointer_lock_element;
    double         last_mouse_x[2];
    double         last_mouse_y[2];
    gboolean       has_last_mouse[2];
    gint64         user_activation_us;
    gboolean       user_ever_activated;
    GHashTable   *canvas_states;
    GPtrArray    *canvas_retired;
    int           canvas_calls;
    ns_image_cache *image_cache;
    struct ns_anim *anim;
    GHashTable   *js_image_loads;
    GHashTable   *orphan_nodes;
    GPtrArray    *listeners;
    GHashTable   *listeners_by_target;
    guint         listener_tombstones;
    GHashTable   *pinned_wrappers_set;
    GPtrArray    *attr_wrappers;
    GHashTable   *attribute_maps;
    GPtrArray    *pending_fetches;
    GHashTable   *fetch_states_by_id;
    guint         next_fetch_id;
    GPtrArray    *pending_xhrs;
    GPtrArray    *pending_ws;
    GPtrArray    *pending_aborts;
    GPtrArray    *filereader_idles;
    GPtrArray    *select_event_tasks;
    GHashTable   *local_storage;
    GHashTable   *session_storage;
    char         *local_storage_origin;
    char         *local_storage_path;
    gboolean      local_storage_dirty;
    guint         local_storage_flush_source;
    gboolean      local_storage_disabled;
    char         *cookie_value;
    guint64       cookie_version;
    GHashTable   *session_storage_buckets;
    GHashTable   *cookie_buckets;
    char         *partition_key;
    guint64       opaque_counter;
    char         *referrer;
    int           ready_state;
    guint         lifecycle_source;
    GArray       *lifecycle_tasks;
    ns_node      *lifecycle_doc;
    char         *lifecycle_origin;
    int           lifecycle_phase;
    gint64        lifecycle_start_us;
    gint64        eval_deadline_us;
    gint64        js_monitor_deadline_us;
    gboolean      halted;
    gboolean      in_pump;
    gboolean      in_scroll_dispatch;
    double        viewport_scroll_x;
    double        viewport_scroll_y;
    GPtrArray    *pending_scrollend;
    gboolean      pending_scrollend_doc;
    int           eval_depth;
    guint         microtask_source;
    gboolean      draining_microtasks;
    GQueue       *message_tasks;
    guint         message_task_source;
    gboolean      running_message_tasks;
    GPtrArray    *document_write_states;
    ns_node      *parser_write_body;
    gboolean      document_write_parser_open;
    GPtrArray    *deferred_script_roots;
    GPtrArray    *async_script_roots;
    guint         async_script_source;
    gboolean      link_rescan_pending;
    GPtrArray    *pending_iframe_loads;
    GPtrArray    *deferred_iframe_loads;
    GHashTable   *pending_rejections;
    GHashTable   *iframe_globals;
    int           iframe_load_depth;
    GArray       *pending_storage_events;
    gboolean      storage_events_draining;
    gint64        last_orphan_sweep_us;
    int           dispatch_depth;
    int           listener_snapshots;
    int           callback_depth;
    int           synthetic_click_depth;
    GPtrArray    *mutation_observers;
    gboolean      mutation_drain_scheduled;
    GHashTable   *slot_assignments;
    GHashTable   *slot_roots;
    GPtrArray    *signaled_slots;
    GPtrArray    *draining_slots;
    GPtrArray    *intersection_observers;
    GPtrArray    *media_query_lists;
    GPtrArray    *resize_observers;
    guint         observer_tick_source;
    gboolean      observer_ticking;
    guint         raf_tick_source;
    gboolean      raf_host_driven;
    JSValue       iframe_doc;
    int           iframe_doc_set;
    ns_csp *csp;
    GHashTable   *doc_csp;
    ns_fetch_policy *fetch_policy;
    GHashTable   *doc_fetch_policy;
    char         *selection_text;
    gboolean      selection_has_range;
    double        selection_x, selection_y, selection_w, selection_h;
    int           module_load_count;
    gsize         module_load_bytes;
    gboolean      module_load_capped;
    GPtrArray    *import_map;
    gint64        time_origin_us;
    gint64        frame_time_us;
    gint64        last_raf_time_us;
    double        time_origin_real_ms;
    ns_js_navigation_timing navigation_timing;
    GPtrArray    *perf_entries;
    GPtrArray    *perf_observers;
    gboolean      perf_drain_scheduled;
    GPtrArray    *node_iters;
    GHashTable   *console_counts;
    GHashTable   *console_timers;
    GHashTable   *blob_urls;
    GHashTable   *ce_registry;
    GHashTable   *ce_def_docs;
    GHashTable   *platform_globals;
    GHashTable   *ce_pending;
    GHashTable   *ce_under_construction;
    ns_node      *ce_upgrading;
    void         *ce_upgrading_wrapper;
    int           ce_in_attr_callback;
    int           ce_defer_upgrades;
    int           throw_on_dynamic_markup;
    int           ignore_destructive_writes;
    int           in_error_report;
    JSValue       nodelist_decorator;
    int           nodelist_decorator_set;
    JSValue       live_html_proto;
    JSValue       live_node_proto;
    JSValue       live_radionode_proto;
    int           live_protos_set;
    JSValue       computed_style_proxy;
    int           computed_style_proxy_set;
    JSValue       url_helper;
    int           url_helper_set;
    JSValue       search_params_helper;
    int           search_params_helper_set;
    JSValue       form_data_helper;
    int           form_data_helper_set;
    JSValue       body_consumer_helper;
    int           body_consumer_helper_set;
    JSAtom        atom_capture;
    JSAtom        atom_once;
    JSAtom        atom_signal;
    JSAtom        atom_passive;
    JSAtom        atom_aborted;
    JSAtom        atom_immediate_stopped;
    JSAtom        atom_propagation_stopped;
    int           listener_atoms_set;
    guint64       dom_gen;
    JSValue       proto_node;
    JSValue       proto_element;
    JSValue       proto_htmlelement;
    JSValue       proto_htmlunknownelement;
    JSValue       proto_svgelement;
    JSValue       proto_svgaelement;
    JSValue       proto_chardata;
    JSValue       proto_text;
    JSValue       proto_comment;
    JSValue       proto_cdata;
    JSValue       proto_pi;
    JSValue       proto_doctype;
    JSValue       proto_docfrag;
    JSValue       proto_document;
    GHashTable   *per_tag_protos;
    int           dom_protos_set;
    struct {
        const void *root;
        char        kind;
        char       *key;
        guint64     gen;
        JSValue     value;
        int         set;
    } qcache[16];
    int           qcache_next;
    GPtrArray    *current_dispatch_path;
    gboolean      current_dispatch_window;
    gboolean      in_hashchange;
};

static inline ns_js *
js_from_ctx(JSContext *ctx)
{
    return ctx ? (ns_js *)JS_GetContextOpaque(ctx) : NULL;
}

double ns_arg_d(JSContext *ctx, JSValueConst v);
void ns_bind_fn(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc);
struct ns_mse_source;
struct ns_mse_source *ns_js_mse_source_for_url(ns_js *js, const char *url);
void ns_js_mse_teardown(ns_js *js);
void ns_js_mse_install(JSContext *ctx, JSValueConst global);
const ns_box *ns_box_find_by_dom(const ns_box *root, const ns_node *target);
uint32_t ns_js_array_length(JSContext *ctx, JSValueConst arr);
gboolean ns_webaudio_render_offline(JSContext *ctx, JSValueConst destination,
                                    uint32_t frames, double rate, float *out);
void ns_js_promise_reject(JSContext *ctx, JSValue resolvers[2], const char *message);
JSValue ns_make_element(JSContext *ctx, const ns_node *cnode);
const ns_node *ns_unwrap_element(JSValueConst val);
void ns_drain_microtasks(ns_js *js);
void ns_obj_adopt_global_proto(JSContext *ctx, JSValueConst obj,
                               const char *iface);
JSValue ns_returns_resolved_undefined(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
gboolean ns_js_has_transient_activation(ns_js *js);
JSValue ns_promise_reject_dom(JSContext *ctx, const char *name,
                              const char *message);
JSValue ns_throw_dom_exception(JSContext *ctx, const char *name, int code,
                               const char *message);
JSValue ns_media_time_ranges_for(JSContext *ctx, double dur);
JSValue ns_media_time_ranges_from(JSContext *ctx, const double *edges, guint n_ranges);

gboolean ns_node_is_media_element(const ns_node *n);
double   ns_media_position(JSContext *ctx, JSValueConst this_val);
gboolean ns_media_frame_counts(JSContext *ctx, JSValueConst this_val,
                               guint *presented, guint *dropped);
void     ns_media_attr_changed(ns_js *js, ns_node *el, const char *name);
void     ns_media_subtree_connected(ns_js *js, ns_node *root);
void     ns_media_subtree_disconnected(ns_js *js, ns_node *root);
void     ns_media_node_released(ns_js *js, ns_node *n);
void     ns_media_blob_updated(ns_js *js, const char *url);
void     ns_media_teardown(ns_js *js);
JSValue ns_media_play(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue ns_media_pause(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue ns_media_load(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue ns_media_fast_seek(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
JSValue ns_media_get_current_time(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_set_current_time(JSContext *ctx, JSValueConst this_val,
                                  JSValueConst val);
JSValue ns_media_get_duration(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_paused(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_ended(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_seeking(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_readyState(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_networkState(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_error(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_volume(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_set_volume(JSContext *ctx, JSValueConst this_val,
                            JSValueConst val);
JSValue ns_media_get_muted(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_set_muted(JSContext *ctx, JSValueConst this_val,
                           JSValueConst val);
JSValue ns_media_get_seekable_ranges(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_buffered_ranges(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_played_ranges(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_video_width(JSContext *ctx, JSValueConst this_val);
JSValue ns_media_get_video_height(JSContext *ctx, JSValueConst this_val);

JSValue
ns_throw_security_error(JSContext *ctx, const char *message);
gboolean
ns_image_bitmap_is(JSValueConst v);
JSValue
ns_window_create_image_bitmap(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
JSValue
ns_window_dommatrix_ctor(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv);
JSValue
ns_window_dommatrix_readonly_ctor(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
JSValue
ns_window_offscreen_canvas_ctor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv);
ns_canvas_state *
ns_canvas_state_for(ns_js *js, const ns_node *el);
gboolean
ns_js_image_origin_clean(ns_js *js, const ns_node *el, const ns_image *im,
                         gboolean cors_requested);
void ns_image_data_install(JSContext *ctx, JSValueConst global);
JSValue
ns_path2d_ctor(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_element_getContext(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
cairo_status_t
ns_canvas_png_write(void *closure, const unsigned char *data, unsigned int length);
JSValue ns_canvas_blob_from_surface(JSContext *ctx, cairo_surface_t *surface);

void ns_canvas_register_image_bitmap_class(JSRuntime *rt);
void ns_canvas_register_path2d_class(JSRuntime *rt);

void ns_make_interface_object(JSContext *ctx, JSValueConst global,
                              const char *name, JSValueConst proto);

void ns_bind_fn_if_not_callable(JSContext *ctx, JSValueConst obj, const char *name,
                                JSCFunction *fn, int argc);
gboolean ns_js_get_bool_prop(JSContext *ctx, JSValueConst obj, const char *key,
                             gboolean *was_set);

double ns_perf_now_ms(const ns_js *js);
void ns_perf_add_resource(ns_js *js, const char *url, const char *initiator,
                          double start_ms, double duration_ms, gint64 size);
double ns_perf_relative_ms(gint64 now_us, gint64 origin_us);
void ns_perf_entry_free(gpointer p);
JSValue ns_perf_supported_entry_types(JSContext *ctx);
void ns_encoding_install(JSContext *ctx, JSValueConst global);
/* Whether the realm's Content-Security-Policy permits string evaluation. */
gboolean ns_js_eval_allowed(JSContext *ctx);
gboolean ns_js_buffer_source_bytes(JSContext *ctx, JSValueConst v,
                                   const uint8_t **data, size_t *len,
                                   JSValue *hold);
void ns_webcrypto_install(JSContext *ctx, JSValueConst global,
                          JSValueConst crypto);
JSValue ns_webcrypto_clone_key(JSContext *ctx, JSValueConst v);
JSValue ns_perf_observer_ctor(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
JSValue ns_perf_observer_observe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
JSValue ns_perf_observer_disconnect(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv);
JSValue ns_perf_observer_takeRecords(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
JSValue ns_window_performance_now(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
JSValue ns_window_performance_mark(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
JSValue ns_window_performance_measure(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
JSValue ns_window_performance_clearMarks(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue ns_window_performance_clearMeasures(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntries(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
JSValue ns_window_performance_memory_get(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

#endif
