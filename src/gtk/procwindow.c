/* Northstar Web Browser — GTK browser window around the page view: toolbar and address bar.
 * Address suggestions, TLS indicator, bookmarks, downloads, fullscreen notice, session saving.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "procwindow.h"
#include <glib/gstdio.h>
#include "audio/audio.h"
#include "icons.h"
#include "procview.h"
#include "i18n.h"
#include "bookmarks.h"
#include "cache.h"
#include "config.h"
#include "css.h"
#include "download_name.h"
#include "history.h"
#include "net.h"
#include "security.h"
#include "version.h"
#include "image.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>

#define NS_PROC_APP_ID "org.northstar.WebBrowser"
#define NS_SUGGEST_MAX 8
#define NS_SUGGEST_BOOKMARKS 3

static int g_initial_win_w;
static int g_initial_win_h;

/* Feeds monitor size, scale, pointer/hover caps and reduced-motion into CSS media queries. */
static void
configure_media_inputs(void)
{
    GdkDisplay *display = gdk_display_get_default();
    ns_css_media_device device = {
        .width = 1920,
        .height = 1080,
        .resolution_dppx = 1,
        .color_bits = 8,
        .pointer = NS_CSS_MEDIA_POINTER_NONE,
        .any_pointer = NS_CSS_MEDIA_POINTER_NONE,
        .update = NS_CSS_MEDIA_UPDATE_FAST,
    };
    if (display) {
        GListModel *monitors = gdk_display_get_monitors(display);
        GdkMonitor *monitor = monitors && g_list_model_get_n_items(monitors) > 0
            ? g_list_model_get_item(monitors, 0) : NULL;
        if (monitor) {
            GdkRectangle geometry;
            gdk_monitor_get_geometry(monitor, &geometry);
            device.width = geometry.width;
            device.height = geometry.height;
            device.resolution_dppx = gdk_monitor_get_scale_factor(monitor);
            g_object_unref(monitor);
        }
        GdkSeat *seat = gdk_display_get_default_seat(display);
        GdkSeatCapabilities caps = seat
            ? gdk_seat_get_capabilities(seat) : GDK_SEAT_CAPABILITY_NONE;
        gboolean fine = (caps & (GDK_SEAT_CAPABILITY_POINTER |
                                 GDK_SEAT_CAPABILITY_TABLET_STYLUS)) != 0;
        gboolean coarse = (caps & GDK_SEAT_CAPABILITY_TOUCH) != 0;
        device.hover = fine;
        device.any_hover = fine;
        device.pointer = fine ? NS_CSS_MEDIA_POINTER_FINE :
                         coarse ? NS_CSS_MEDIA_POINTER_COARSE :
                                  NS_CSS_MEDIA_POINTER_NONE;
        device.any_pointer =
            (fine ? NS_CSS_MEDIA_POINTER_FINE : 0) |
            (coarse ? NS_CSS_MEDIA_POINTER_COARSE : 0);
    }
    ns_css_set_media_device(&device);

    ns_reduced_motion_pref motion = ns_config_get()->reduced_motion;
    GtkSettings *settings = gtk_settings_get_default();
    if (motion != NS_REDUCED_MOTION_PREF_AUTO) {
        ns_css_set_reduced_motion(motion == NS_REDUCED_MOTION_PREF_REDUCE
            ? NS_CSS_REDUCED_MOTION_REDUCE
            : NS_CSS_REDUCED_MOTION_NO_PREFERENCE);
    } else if (settings) {
        gboolean animations = TRUE;
        g_object_get(settings, "gtk-enable-animations", &animations, NULL);
        ns_css_set_reduced_motion(animations
            ? NS_CSS_REDUCED_MOTION_NO_PREFERENCE
            : NS_CSS_REDUCED_MOTION_REDUCE);
    }
}

/* Infers dark vs light desktop theme from the luma of the widget's foreground color. */
static ns_css_color_scheme
desktop_color_scheme(GtkWidget *styled)
{
    GdkRGBA fg;
    gtk_widget_get_color(styled, &fg);
    double luma = 0.2126 * fg.red + 0.7152 * fg.green + 0.0722 * fg.blue;
    return luma > 0.5 ? NS_CSS_COLOR_SCHEME_DARK : NS_CSS_COLOR_SCHEME_LIGHT;
}

void
ns_procapp_set_window_size(int width, int height)
{
    g_initial_win_w = width;
    g_initial_win_h = height;
}

typedef struct {
    GtkApplication *app;
    GtkWidget      *window;
    NsProcView     *view;
    GtkWidget      *address;
    GtkEventController *address_focus;
    gboolean        address_quiet;
    GtkWidget      *suggest_popover;
    GtkWidget      *suggest_list;
    char           *suggest_typed;
    GtkWidget      *zoom_button;
    GtkWidget      *back;
    GtkWidget      *forward;
    GtkWidget      *reload;
    GtkWidget      *stop;
    GtkWidget      *spinner;
    GtkWidget      *status;
    char           *status_base;
    guint           status_timer;
    GtkWidget      *fullscreen_notice;
    guint           fullscreen_timer;
    gulong          theme_watch[2];
    GtkWidget      *bookmarks_button;
    char           *home_url;
    ns_bookmarks   *bookmarks;
    char           *session_path;
    guint           session_timer;
    GtkWidget      *downloads_win;
    GtkWidget      *downloads_list;
} ProcWindow;

/* Returns the localized brand name plus version from a lazily filled static buffer. */
static const char *
ns_brand_versioned(void)
{
    static char brand[128];
    if (!brand[0])
        g_snprintf(brand, sizeof brand, "%s %s",
                   ns_i18n("Northstar Web Browser"), NS_VERSION);
    return brand;
}

static void
procwindow_free(gpointer data)
{
    ProcWindow *pw = data;
    if (pw->session_timer)
        g_source_remove(pw->session_timer);
    if (pw->status_timer)
        g_source_remove(pw->status_timer);
    if (pw->fullscreen_timer)
        g_source_remove(pw->fullscreen_timer);
    GtkSettings *settings = gtk_settings_get_default();
    for (int i = 0; settings && i < 2; i++)
        if (pw->theme_watch[i])
            g_signal_handler_disconnect(settings, pw->theme_watch[i]);
    g_free(pw->session_path);
    g_free(pw->home_url);
    g_free(pw->status_base);
    g_free(pw->suggest_typed);
    if (pw->bookmarks)
        ns_bookmarks_free(pw->bookmarks);
    g_free(pw);
}

/* Adds the GResource icon path and share/data icon dirs relative to the executable. */
static void
install_icon_search_paths(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    if (!theme)
        return;
    gtk_icon_theme_add_resource_path(theme, "/org/northstar/WebBrowser/icons");
    const char *exe = ns_app_self_exe();
    if (!exe)
        return;
    char *dir = g_path_get_dirname(exe);
    const char *rel[] = { "share/icons",      "../share/icons",
                          "data/icons",       "../data/icons",
                          "../../data/icons", "../../../data/icons",
                          "../../../../data/icons", NULL };
    for (int i = 0; rel[i]; i++) {
        char *p = g_build_filename(dir, rel[i], NULL);
        gtk_icon_theme_add_search_path(theme, p);
        g_free(p);
    }
    g_free(dir);
}

/* Installs app-wide GTK CSS for the status bar, fullscreen notice, headerbar and toolbar. */
static void
install_status_css(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        p,
        ".ns-procstatus {"
        "  padding: 2px 10px;"
        "  border-top-right-radius: 6px;"
        "  background: @theme_base_color;"
        "  border-top: 1px solid alpha(currentColor, 0.15);"
        "  border-right: 1px solid alpha(currentColor, 0.15);"
        "  font-size: smaller;"
        "}"
        ".ns-fullscreen-notice {"
        "  margin-top: 24px;"
        "  padding: 10px 22px;"
        "  border-radius: 8px;"
        "  background: rgba(30, 30, 30, 0.92);"
        "  color: #ffffff;"
        "  font-weight: bold;"
        "  box-shadow: 0 2px 12px rgba(0, 0, 0, 0.5);"
        "}"
        "headerbar, headerbar > windowhandle {"
        "  min-height: 34px;"
        "}"
        "headerbar button.titlebutton {"
        "  min-height: 26px;"
        "  min-width: 26px;"
        "  padding: 0;"
        "}"
        ".ns-toolbar menubutton > button {"
        "  min-height: 34px;"
        "  min-width: 40px;"
        "  padding: 2px 5px 1px 5px;"
        "  border-radius: 0;"
        "}"
        ".ns-toolbar menubutton > button:hover {"
        "  border-top: 1px solid #ffffff;"
        "  border-left: 1px solid #ffffff;"
        "  border-right: 1px solid #6b778c;"
        "  border-bottom: 1px solid #6b778c;"
        "  box-shadow: inset -1px -1px 0 #9aa5b8;"
        "  background-color: #e6eaf1;"
        "}"
        ".ns-toolbar menubutton > button:active, .ns-toolbar menubutton > button:checked {"
        "  border-top: 1px solid #6b778c;"
        "  border-left: 1px solid #6b778c;"
        "  border-right: 1px solid #ffffff;"
        "  border-bottom: 1px solid #ffffff;"
        "  box-shadow: inset 1px 1px 0 #9aa5b8;"
        "  background-color: #d3d9e3;"
        "}"
        ".ns-toolbar {"
        "  background-image: linear-gradient(to bottom, #fdfdfe 0%, #f1f3f7 45%, #e2e6ee 100%);"
        "  border-top: 1px solid #ffffff;"
        "  border-bottom: 1px solid #9aa5b8;"
        "  box-shadow: 0 1px 0 #6b778c;"
        "  padding: 1px 3px;"
        "}"
        ".ns-toolbar-separator {"
        "  min-width: 2px;"
        "  margin: 3px 5px;"
        "  border-left: 1px solid #9aa5b8;"
        "  border-right: 1px solid #ffffff;"
        "  background: none;"
        "}"
        ".ns-toolbar button {"
        "  min-height: 24px;"
        "  min-width: 24px;"
        "  padding: 1px 4px;"
        "  border: 1px solid transparent;"
        "  border-radius: 0;"
        "  background: transparent;"
        "  color: #000000;"
        "  box-shadow: none;"
        "  text-shadow: none;"
        "}"
        ".ns-toolbar button.ns-nav-button {"
        "  min-width: 40px;"
        "  min-height: 34px;"
        "  padding: 2px 5px 1px 5px;"
        "}"
        ".ns-toolbar button:hover {"
        "  border-top: 1px solid #ffffff;"
        "  border-left: 1px solid #ffffff;"
        "  border-right: 1px solid #6b778c;"
        "  border-bottom: 1px solid #6b778c;"
        "  box-shadow: inset -1px -1px 0 #9aa5b8;"
        "  background-color: #e6eaf1;"
        "}"
        ".ns-toolbar button:active, .ns-toolbar button:checked {"
        "  border-top: 1px solid #6b778c;"
        "  border-left: 1px solid #6b778c;"
        "  border-right: 1px solid #ffffff;"
        "  border-bottom: 1px solid #ffffff;"
        "  box-shadow: inset 1px 1px 0 #9aa5b8;"
        "  background-color: #d3d9e3;"
        "  padding: 2px 3px 0 5px;"
        "}"
        ".ns-toolbar button.ns-nav-button:active, .ns-toolbar button.ns-nav-button:checked {"
        "  padding: 3px 4px 0 6px;"
        "}"
        ".ns-toolbar button:disabled {"
        "  border: 1px solid transparent;"
        "  background: transparent;"
        "  box-shadow: none;"
        "  -gtk-icon-filter: opacity(0.35) grayscale(1);"
        "}"
        ".ns-toolbar button:disabled .ns-toolbar-label {"
        "  color: #9aa5b8;"
        "  text-shadow: 1px 1px 0 #ffffff;"
        "}"
        ".ns-toolbar-label {"
        "  font-size: 10px;"
        "  font-weight: normal;"
        "  color: #000000;"
        "  margin-top: 0;"
        "  margin-bottom: 0;"
        "}"
        ".ns-toolbar entry.ns-address {"
        "  min-height: 20px;"
        "  padding: 1px 4px;"
        "  border-top: 1px solid #9aa5b8;"
        "  border-left: 1px solid #9aa5b8;"
        "  border-right: 1px solid #ffffff;"
        "  border-bottom: 1px solid #ffffff;"
        "  box-shadow: inset 1px 1px 0 #6b778c, inset -1px -1px 0 #e6eaf1;"
        "  background-color: #ffffff;"
        "  color: #000000;"
        "  border-radius: 0;"
        "  font-size: 12px;"
        "}"
        ".ns-toolbar entry.ns-address > text {"
        "  min-height: 0;"
        "  padding: 0;"
        "}"
        ".ns-toolbar entry.ns-address > image {"
        "  margin-top: 0;"
        "  margin-bottom: 0;"
        "}"
        ".ns-toolbar entry.ns-address:focus {"
        "  box-shadow: inset 1px 1px 0 #000000, inset -1px -1px 0 #e6eaf1;"
        "}"
        ".ns-address image.left { margin-right: 5px; }"
        ".ns-suggest, .ns-suggest > contents {"
        "  box-shadow: none;"
        "}"
        ".ns-suggest > contents {"
        "  padding: 1px;"
        "  border: 1px solid #6b778c;"
        "  border-radius: 0;"
        "  background-color: #ffffff;"
        "}"
        ".ns-suggest list {"
        "  background: transparent;"
        "}"
        ".ns-suggest row {"
        "  padding: 3px 8px;"
        "  border-radius: 0;"
        "  color: #000000;"
        "}"
        ".ns-suggest row:hover {"
        "  background-color: #e6eaf1;"
        "}"
        ".ns-suggest row:selected {"
        "  background-color: #2f5aa8;"
        "  color: #ffffff;"
        "}"
        ".ns-suggest-title {"
        "  font-size: 12px;"
        "}"
        ".ns-suggest-url {"
        "  font-size: 11px;"
        "  color: #6b778c;"
        "}"
        ".ns-suggest row:selected .ns-suggest-url {"
        "  color: #dfe6f4;"
        "}"
        ".ns-zoom {"
        "  font-size: smaller;"
        "  padding: 0 5px;"
        "  min-width: 0;"
        "  border: 1px solid transparent;"
        "}"
        ".ns-throbber {"
        "  min-width: 26px;"
        "  min-height: 26px;"
        "  border-top: 1px solid #9aa5b8;"
        "  border-left: 1px solid #9aa5b8;"
        "  border-right: 1px solid #ffffff;"
        "  border-bottom: 1px solid #ffffff;"
        "  box-shadow: inset 1px 1px 0 #6b778c;"
        "  background-color: #080e22;"
        "  border-radius: 0;"
        "  padding: 1px;"
        "  margin: 1px 1px 1px 5px;"
        "}"
        ".ns-throbber:hover {"
        "  background-color: #121c3c;"
        "  border-top: 1px solid #9aa5b8;"
        "  border-left: 1px solid #9aa5b8;"
        "  border-right: 1px solid #ffffff;"
        "  border-bottom: 1px solid #ffffff;"
        "  box-shadow: inset 1px 1px 0 #6b778c;"
        "}");
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void
set_accessible_label(GtkWidget *w, const char *label)
{
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

/* Returns the GtkImage inside a toolbar button, looking into an icon+label box if present. */
static GtkImage *
toolbar_button_image(GtkWidget *button)
{
    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(button));
    if (GTK_IS_BOX(child))
        child = gtk_widget_get_first_child(child);
    return GTK_IS_IMAGE(child) ? GTK_IMAGE(child) : NULL;
}

static void
toolbar_button_icon_size(GtkWidget *button, int pixels)
{
    GtkImage *img = toolbar_button_image(button);
    if (img)
        gtk_image_set_pixel_size(img, pixels);
}

/* Creates a frameless toolbar button with icon, optional label, tooltip and a11y label. */
static GtkWidget *
toolbar_button(const char *icon, const char *label, const char *tooltip,
               GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    if (label && *label) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
        GtkWidget *img = ns_icon_image_new(icon);
        GtkWidget *lbl = gtk_label_new(label);
        gtk_widget_add_css_class(lbl, "ns-toolbar-label");
        gtk_box_append(GTK_BOX(box), img);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(b), box);
        gtk_widget_add_css_class(b, "ns-nav-button");
    } else {
        GtkWidget *img = ns_icon_image_new(icon);
        gtk_button_set_child(GTK_BUTTON(b), img);
    }
    gtk_widget_set_tooltip_text(b, tooltip);
    set_accessible_label(b, tooltip);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static NsProcView *
current_view(ProcWindow *pw)
{
    return pw->view;
}

/* Turns address-bar input into a URL: keeps schemes, maps local paths, searches, else https. */
static char *
normalize_url(const char *input)
{
    char *trimmed = g_strstrip(g_strdup(input ? input : ""));
    if (!*trimmed)
        return trimmed;
    if (g_str_has_prefix(trimmed, "about:") ||
        g_str_has_prefix(trimmed, "file:") ||
        g_str_has_prefix(trimmed, "data:") ||
        g_str_has_prefix(trimmed, "view-source:") || strstr(trimmed, "://"))
        return trimmed;
    char *local = ns_url_from_local_path(trimmed);
    if (local) {
        g_free(trimmed);
        return local;
    }
    if (ns_address_is_search(trimmed)) {
        char *out = ns_search_url_for(trimmed);
        g_free(trimmed);
        return out;
    }
    char *out = g_strconcat("https://", trimmed, NULL);
    g_free(trimmed);
    return out;
}

static void
set_loading_ui(ProcWindow *pw, gboolean loading)
{
    gtk_widget_set_visible(pw->spinner, loading);
    gtk_spinner_set_spinning(GTK_SPINNER(pw->spinner), loading);
    if (pw->stop)
        gtk_widget_set_sensitive(pw->stop, loading);
}

/* Returns the URL with any userinfo ("user:pass@") removed; caller frees. */
static char *
address_display_url(const char *url)
{
    if (!url || !*url) return g_strdup("");
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return g_strdup(url);
    const char *authority = scheme_end + 3;
    const char *authority_end = authority + strcspn(authority, "/?#");
    const char *at = NULL;
    for (const char *p = authority; p < authority_end; p++)
        if (*p == '@') at = p;
    if (!at) return g_strdup(url);
    return g_strdup_printf("%.*s%s", (int)(authority - url), url, at + 1);
}

/* Sets address-bar text without triggering the suggestion popover. */
static void
address_set_text_quiet(ProcWindow *pw, const char *text)
{
    pw->address_quiet = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(pw->address), text);
    pw->address_quiet = FALSE;
}

static void
set_address_text(ProcWindow *pw, const char *url)
{
    char *shown = address_display_url(url);
    address_set_text_quiet(pw, shown);
    g_free(shown);
}

/* Sets the address-bar TLS icon and tooltip with state, host and server IP. */
static void
update_security_indicator(ProcWindow *pw, NsProcView *v)
{
    GtkEntry *entry = GTK_ENTRY(pw->address);
    if (!entry)
        return;

    const char *url = v ? ns_proc_view_url(v) : NULL;
    int sec = v ? ns_proc_view_security(v) : NS_SEC_NONE;
    const char *icon_name = NULL, *label = NULL;
    switch (sec) {
    case NS_SEC_SECURE:
        icon_name = "security-high-symbolic";
        label = ns_i18n("Secure — the certificate is valid");
        break;
    case NS_SEC_INVALID:
        icon_name = "security-low-symbolic";
        label = ns_i18n("Not secure — the certificate is not trusted");
        break;
    case NS_SEC_PLAIN:
        icon_name = "channel-insecure-symbolic";
        label = ns_i18n("Not secure — the connection is not encrypted");
        break;
    default:
        break;
    }
    if (!icon_name || !url || !*url) {
        ns_icon_entry_set(entry, GTK_ENTRY_ICON_PRIMARY, "northstar-bookmarks");
        gtk_entry_set_icon_tooltip_text(entry, GTK_ENTRY_ICON_PRIMARY,
                                        ns_i18n("Page location"));
        return;
    }
    gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_PRIMARY, icon_name);
    gtk_entry_set_icon_activatable(entry, GTK_ENTRY_ICON_PRIMARY, FALSE);

    GString *tip = g_string_new(label);
    char *host = ns_url_host_from(url);
    if (host && *host)
        g_string_append_printf(tip, "\n%s", host);
    const char *ip = ns_proc_view_remote_ip(v);
    if (ip && *ip)
        g_string_append_printf(tip, "\n%s %s", ns_i18n("Server:"), ip);
    gtk_entry_set_icon_tooltip_text(entry, GTK_ENTRY_ICON_PRIMARY, tip->str);
    g_string_free(tip, TRUE);
    g_free(host);
}

static gboolean
current_page_bookmarked(ProcWindow *pw)
{
    NsProcView *v = current_view(pw);
    const char *url = v ? ns_proc_view_url(v) : NULL;
    return url && *url && pw->bookmarks &&
           ns_bookmarks_contains(pw->bookmarks, url);
}

/* Switches the bookmarks button icon and tooltip by whether the current page is bookmarked. */
static void
update_bookmark_indicator(ProcWindow *pw)
{
    if (!pw->bookmarks_button)
        return;
    gboolean saved = current_page_bookmarked(pw);
    GtkImage *img = toolbar_button_image(pw->bookmarks_button);
    if (img)
        ns_icon_image_set(img, saved ? "northstar-bookmarks-saved"
                                     : "northstar-bookmarks");
    gtk_widget_set_tooltip_text(pw->bookmarks_button,
                                saved ? ns_i18n("Bookmarked — open bookmarks")
                                      : ns_i18n("Bookmarks"));
}

/* Applies the configured or desktop color scheme and reloads the page if it changed. */
static void
apply_color_scheme(ProcWindow *pw)
{
    ns_color_scheme_pref pref = ns_config_get()->color_scheme;
    ns_css_color_scheme want =
        pref == NS_COLOR_SCHEME_PREF_LIGHT ? NS_CSS_COLOR_SCHEME_LIGHT :
        pref == NS_COLOR_SCHEME_PREF_DARK  ? NS_CSS_COLOR_SCHEME_DARK :
        desktop_color_scheme(pw->window);
    if (want == ns_css_get_color_scheme())
        return;
    ns_css_set_color_scheme(want);
    NsProcView *v = current_view(pw);
    const char *url = v ? ns_proc_view_url(v) : NULL;
    if (url && *url)
        ns_proc_view_reload(v);
}

static void
on_theme_changed(GObject *settings, GParamSpec *pspec, gpointer ud)
{
    (void)settings; (void)pspec;
    apply_color_scheme(ud);
}

/* Shows the zoom-percent button when zoom is not 100%, hiding it otherwise. */
static void
update_zoom_indicator(ProcWindow *pw, NsProcView *v)
{
    if (!pw->zoom_button)
        return;
    int percent = v ? ns_proc_view_zoom_percent(v) : 100;
    if (percent == 100) {
        gtk_widget_set_visible(pw->zoom_button, FALSE);
        return;
    }
    char label[16];
    g_snprintf(label, sizeof label, "%d%%", percent);
    gtk_button_set_label(GTK_BUTTON(pw->zoom_button), label);
    gtk_widget_set_visible(pw->zoom_button, TRUE);
}

static void
on_zoom_indicator_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_reset(v);
}

/* Refreshes address bar, window title, back/forward and security/zoom/bookmark indicators. */
static void
update_chrome(ProcWindow *pw)
{
    NsProcView *v = current_view(pw);
    if (!v) {
        address_set_text_quiet(pw, "");
        gtk_window_set_title(GTK_WINDOW(pw->window), ns_brand_versioned());
        gtk_widget_set_sensitive(pw->back, FALSE);
        gtk_widget_set_sensitive(pw->forward, FALSE);
        update_security_indicator(pw, NULL);
        set_loading_ui(pw, FALSE);
        return;
    }
    set_loading_ui(pw, ns_proc_view_is_loading(v));
    const char *url = ns_proc_view_url(v);
    const char *title = ns_proc_view_title(v);
    set_address_text(pw, url);
    const char *brand = ns_brand_versioned();
    char *wt = title && *title ? g_strdup_printf("%s — %s", title, brand)
                               : g_strdup(brand);
    gtk_window_set_title(GTK_WINDOW(pw->window), wt);
    g_free(wt);
    gtk_widget_set_sensitive(pw->back, ns_proc_view_can_back(v));
    gtk_widget_set_sensitive(pw->forward, ns_proc_view_can_forward(v));
    update_security_indicator(pw, v);
    update_zoom_indicator(pw, v);
    update_bookmark_indicator(pw);
}

static void proc_window_load(ProcWindow *pw, const char *url);

typedef struct {
    char      *url;
    char      *path;
    char      *name;
    GtkWidget *progress;
    GtkWidget *status;
    GtkWidget *open;
    guint      pulse;
    gboolean   ok;
    gint64     size;
} NsDownload;

static void show_downloads_window(ProcWindow *pw);
static void pw_start_download(ProcWindow *pw, const char *url,
                              const char *suggested);

static const char *
downloads_dir(void)
{
    return ns_security_downloads_dir();
}

static void
download_open_path(const char *path)
{
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) {
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
}

static void
on_download_open(GtkButton *b, gpointer ud)
{
    (void)b;
    download_open_path((const char *)ud);
}

static void
download_free_str(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void
on_open_downloads_folder(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    download_open_path(downloads_dir());
}

static gboolean
download_pulse(gpointer ud)
{
    NsDownload *d = ud;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(d->progress));
    return G_SOURCE_CONTINUE;
}

/* Main-thread completion of a download: updates its row and frees the NsDownload. */
static gboolean
download_finish_idle(gpointer ud)
{
    NsDownload *d = ud;
    if (d->pulse) { g_source_remove(d->pulse); d->pulse = 0; }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->progress),
                                  d->ok ? 1.0 : 0.0);
    if (d->ok) {
        char *sz = g_format_size((guint64)d->size);
        char *msg = g_strdup_printf("%s — %s", d->name, sz);
        gtk_label_set_text(GTK_LABEL(d->status), msg);
        gtk_widget_set_sensitive(d->open, TRUE);
        g_free(sz);
        g_free(msg);
    } else {
        char *msg = g_strdup_printf("%s — %s", d->name, ns_i18n("Failed"));
        gtk_label_set_text(GTK_LABEL(d->status), msg);
        g_free(msg);
    }
    g_free(d->url);
    g_free(d->path);
    g_free(d->name);
    g_free(d);
    return G_SOURCE_REMOVE;
}

/* Downloads requested by a page are fetched without an initiator, so
 * only network-style schemes may be saved: never file:, about: or a
 * view-source: wrapper around them. */
static gboolean
download_url_allowed(const char *url)
{
    static const char *const ok[] = {
        "http://", "https://", "ftp://", "data:", "blob:", NULL,
    };
    for (gsize i = 0; url && ok[i]; i++)
        if (g_ascii_strncasecmp(url, ok[i], strlen(ok[i])) == 0) return TRUE;
    return FALSE;
}

/* Thread body: fetches the URL, rechecks the final URL, writes the file and tags its origin. */
static gpointer
download_worker(gpointer ud)
{
    NsDownload *d = ud;
    ns_security_deny_exec_memory(FALSE);
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_blocking(d->url, NULL, &err);
    if (resp && resp->final_url && !download_url_allowed(resp->final_url)) {
        ns_response_free(resp);
        resp = NULL;
    }
    if (resp && !resp->error && resp->body &&
        g_file_set_contents(d->path, (const char *)resp->body->data,
                            resp->body->len, NULL)) {
        d->ok = TRUE;
        d->size = resp->body->len;
        ns_security_mark_download_origin(
            d->path, resp->final_url ? resp->final_url : d->url);
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    g_idle_add(download_finish_idle, d);
    return NULL;
}

/* Builds a downloads-list row; with d, adds a progress bar and wires d's widgets. */
static GtkWidget *
download_row_new(const char *name, gboolean done, const char *open_path,
                 NsDownload *d)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    GtkWidget *status = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *open = gtk_button_new_with_label(ns_i18n("Open"));
    gtk_widget_set_sensitive(open, done);
    g_signal_connect_data(open, "clicked", G_CALLBACK(on_download_open),
                          g_strdup(open_path), download_free_str, 0);
    if (d) {
        GtkWidget *progress = gtk_progress_bar_new();
        gtk_widget_set_hexpand(progress, TRUE);
        gtk_widget_set_valign(progress, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(hb), progress);
        d->progress = progress;
        d->status = status;
        d->open = open;
    } else {
        GtkWidget *spacer = gtk_label_new("");
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(hb), spacer);
    }
    gtk_box_append(GTK_BOX(hb), open);
    gtk_box_append(GTK_BOX(row), status);
    gtk_box_append(GTK_BOX(row), hb);
    return row;
}

/* Lists up to 25 regular files from the downloads dir, or an empty-state label. */
static void
downloads_populate_recent(ProcWindow *pw)
{
    const char *dir = downloads_dir();
    GDir *gd = g_dir_open(dir, 0, NULL);
    guint shown = 0;
    if (gd) {
        GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
        const char *nm;
        while ((nm = g_dir_read_name(gd)) && files->len < 200) {
            if (nm[0] == '.') continue;
            g_ptr_array_add(files, g_build_filename(dir, nm, NULL));
        }
        g_dir_close(gd);
        g_ptr_array_sort(files, (GCompareFunc)g_strcmp0);
        for (guint i = 0; i < files->len && shown < 25; i++) {
            const char *path = g_ptr_array_index(files, i);
            if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) continue;
            char *base = g_path_get_basename(path);
            GtkWidget *row = download_row_new(base, TRUE, path, NULL);
            gtk_list_box_append(GTK_LIST_BOX(pw->downloads_list), row);
            g_free(base);
            shown++;
        }
        g_ptr_array_free(files, TRUE);
    }
    if (shown == 0) {
        GtkWidget *empty = gtk_label_new(ns_i18n("Nothing downloaded yet"));
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_widget_set_margin_top(empty, 24);
        gtk_widget_set_margin_bottom(empty, 24);
        gtk_list_box_append(GTK_LIST_BOX(pw->downloads_list), empty);
    }
}

/* Hides the downloads window on close instead of destroying it. */
static gboolean
downloads_win_close(GtkWindow *win, gpointer ud)
{
    (void)ud;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE;
}

/* Presents the downloads window, creating it with its folder button and list on first use. */
static void
show_downloads_window(ProcWindow *pw)
{
    if (pw->downloads_win) {
        gtk_window_present(GTK_WINDOW(pw->downloads_win));
        return;
    }
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), ns_i18n("Downloads"));
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 420);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(pw->window));
    g_signal_connect(win, "close-request",
                     G_CALLBACK(downloads_win_close), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(header, 8);
    gtk_widget_set_margin_end(header, 8);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 4);
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *folder = gtk_button_new_with_label(ns_i18n("Open folder"));
    g_signal_connect(folder, "clicked",
                     G_CALLBACK(on_open_downloads_folder), NULL);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), folder);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_window_set_child(GTK_WINDOW(win), box);

    pw->downloads_win = win;
    pw->downloads_list = list;
    downloads_populate_recent(pw);
    gtk_window_present(GTK_WINDOW(win));
}

/* The page-supplied name reduced to a plain file name, or NULL (see download_name.h). */
static char *
download_name_from(const char *candidate)
{
    if (!candidate || !*candidate) return NULL;
    /* g_path_get_basename() only splits on this platform's separators;
     * the sanitiser turns the other platform's into '_'. */
    g_autofree char *base = g_path_get_basename(candidate);
    if (!base || strcmp(base, ".") == 0 || strcmp(base, G_DIR_SEPARATOR_S) == 0)
        return NULL;
    return ns_download_file_name(base);
}

/* Picks a safe unique file name in the downloads dir and starts a background download. */
static void
pw_start_download(ProcWindow *pw, const char *url, const char *suggested)
{
    if (!url || !*url || !download_url_allowed(url)) return;
    char *name = download_name_from(suggested);
    if (!name) {
        char *path_part = g_strdup(url);
        char *q = strpbrk(path_part, "?#");
        if (q) *q = '\0';
        name = download_name_from(path_part);
        g_free(path_part);
    }
    if (!name) name = g_strdup("download");
    const char *dir = downloads_dir();
    char *path = g_build_filename(dir, name, NULL);
    for (int n = 1; g_file_test(path, G_FILE_TEST_EXISTS) && n < 1000; n++) {
        g_free(path);
        char *alt = g_strdup_printf("%s.%d", name, n);
        path = g_build_filename(dir, alt, NULL);
        g_free(alt);
    }

    show_downloads_window(pw);

    NsDownload *d = g_new0(NsDownload, 1);
    d->url = g_strdup(url);
    d->path = path;
    d->name = name;
    GtkWidget *row = download_row_new(name, FALSE, path, d);
    gtk_list_box_prepend(GTK_LIST_BOX(pw->downloads_list), row);
    d->pulse = g_timeout_add(120, download_pulse, d);
    GThread *t = g_thread_new("ns-download", download_worker, d);
    if (t) g_thread_unref(t);
}

static gboolean
pw_fullscreen_notice_expire(gpointer data)
{
    ProcWindow *pw = data;
    pw->fullscreen_timer = 0;
    gtk_widget_set_visible(pw->fullscreen_notice, FALSE);
    return G_SOURCE_REMOVE;
}

static void
pw_hide_fullscreen_notice(ProcWindow *pw)
{
    if (pw->fullscreen_timer) {
        g_source_remove(pw->fullscreen_timer);
        pw->fullscreen_timer = 0;
    }
    gtk_widget_set_visible(pw->fullscreen_notice, FALSE);
}

/* Shows the "host is now full screen, press Esc" notice for five seconds. */
static void
pw_show_fullscreen_notice(ProcWindow *pw)
{
    const char *url = pw->view ? ns_proc_view_url(pw->view) : NULL;
    char *host = url ? ns_url_host_from(url) : NULL;
    const char *with_host =
        ns_i18n("%s is now full screen. Press Esc to exit.");
    const char *slot = host && *host ? strstr(with_host, "%s") : NULL;
    char *text = slot
        ? g_strdup_printf("%.*s%s%s", (int)(slot - with_host), with_host,
                          host, slot + 2)
        : g_strdup(ns_i18n("You are now in full screen. Press Esc to exit."));
    g_free(host);
    gtk_label_set_text(GTK_LABEL(pw->fullscreen_notice), text);
    g_free(text);
    gtk_widget_set_visible(pw->fullscreen_notice, TRUE);
    if (pw->fullscreen_timer)
        g_source_remove(pw->fullscreen_timer);
    pw->fullscreen_timer = g_timeout_add_seconds(5, pw_fullscreen_notice_expire,
                                                 pw);
}

static void
on_window_fullscreened(GObject *window, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    ProcWindow *pw = user_data;
    if (gtk_window_is_fullscreen(GTK_WINDOW(window)))
        pw_show_fullscreen_notice(pw);
    else
        pw_hide_fullscreen_notice(pw);
}

static void
pw_render_status(ProcWindow *pw)
{
    const char *base = pw->status_base ? pw->status_base : "";
    gtk_label_set_text(GTK_LABEL(pw->status), base);
    gtk_widget_set_visible(pw->status, *base != '\0');
}

/* Sets the status bar text with no expiry, cancelling any pending timeout. */
static void
pw_set_status_persistent(ProcWindow *pw, const char *text)
{
    if (pw->status_timer) {
        g_source_remove(pw->status_timer);
        pw->status_timer = 0;
    }
    g_free(pw->status_base);
    pw->status_base = g_strdup(text ? text : "");
    pw_render_status(pw);
}

static gboolean
pw_status_expire(gpointer data)
{
    ProcWindow *pw = data;
    pw->status_timer = 0;
    g_clear_pointer(&pw->status_base, g_free);
    pw_render_status(pw);
    return G_SOURCE_REMOVE;
}

/* Sets the status bar text and schedules it to clear after five seconds. */
static void
pw_set_status(ProcWindow *pw, const char *text)
{
    if (pw->status_timer)
        g_source_remove(pw->status_timer);
    g_free(pw->status_base);
    pw->status_base = g_strdup(text ? text : "");
    pw_render_status(pw);
    pw->status_timer = *pw->status_base
        ? g_timeout_add_seconds(5, pw_status_expire, pw) : 0;
}

/* Dispatches view events (title, URL, status, history, loading, zoom, download) to chrome. */
static void
on_view_notify(NsProcView *v, NsProcEvent evt, const char *text,
               gpointer user_data)
{
    ProcWindow *pw = user_data;

    switch (evt) {
    case NS_PROC_EVT_TITLE:
        if (!ns_proc_view_is_private(v))
            ns_history_record(ns_proc_view_url(v), text);
        update_chrome(pw);
        break;
    case NS_PROC_EVT_URL:
        set_address_text(pw, text);
        pw_set_status_persistent(pw, NULL);
        break;
    case NS_PROC_EVT_STATUS:
        pw_set_status_persistent(pw, text);
        break;
    case NS_PROC_EVT_HISTORY:
        gtk_widget_set_sensitive(pw->back, ns_proc_view_can_back(v));
        gtk_widget_set_sensitive(pw->forward, ns_proc_view_can_forward(v));
        break;
    case NS_PROC_EVT_LOADING: {
        gboolean was_loading = gtk_widget_get_visible(pw->spinner);
        gboolean loading = text && *text == '1';
        set_loading_ui(pw, loading);
        if (was_loading && !loading) {
            pw_set_status(pw, ns_i18n("Done"));
            GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(pw->window));
            if (!focus || (focus != pw->address &&
                           !gtk_widget_is_ancestor(focus, pw->address)))
                ns_proc_view_focus(v);
        }
        break;
    }
    case NS_PROC_EVT_ZOOM:
        update_zoom_indicator(pw, v);
        break;
    case NS_PROC_EVT_DOWNLOAD:
        if (text && *text) {
            char **parts = g_strsplit(text, "\t", 2);
            pw_start_download(pw, parts[0],
                              parts[1] && *parts[1] ? parts[1] : NULL);
            g_strfreev(parts);
        }
        break;
    }
}

/* Normalizes the input to a URL, shows it in the address bar and loads it. */
static void
proc_window_load(ProcWindow *pw, const char *url)
{
    char *resolved = normalize_url(url);
    address_set_text_quiet(pw, resolved);
    ns_proc_view_load(pw->view, resolved);
    g_free(resolved);
}

/* Selects all address-bar text from an idle so it survives the focus click. */
static gboolean
address_select_all_idle(gpointer user_data)
{
    ProcWindow *pw = user_data;
    if (pw->address && GTK_IS_EDITABLE(pw->address))
        gtk_editable_select_region(GTK_EDITABLE(pw->address), 0, -1);
    return G_SOURCE_REMOVE;
}

static void
on_address_focus_enter(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    (void)ctrl;
    g_idle_add(address_select_all_idle, user_data);
}

static void
suggest_hide(ProcWindow *pw)
{
    if (pw->suggest_popover && gtk_widget_get_visible(pw->suggest_popover))
        gtk_popover_popdown(GTK_POPOVER(pw->suggest_popover));
}

static void
on_address_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    (void)ctrl;
    suggest_hide(user_data);
}

/* Builds a suggestion row storing its navigation target and optional address-fill text. */
static GtkWidget *
suggest_row_new(const char *title, const char *url, const char *target)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_focusable(row, FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title_label, "ns-suggest-title");
    gtk_box_append(GTK_BOX(box), title_label);
    if (url) {
        GtkWidget *url_label = gtk_label_new(url);
        gtk_label_set_xalign(GTK_LABEL(url_label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(url_label, "ns-suggest-url");
        gtk_box_append(GTK_BOX(box), url_label);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "ns-target", g_strdup(target),
                           g_free);
    if (url)
        g_object_set_data_full(G_OBJECT(row), "ns-fill", g_strdup(url),
                               g_free);
    return row;
}

/* Returns TRUE if a suggestion row with the given target URL is already listed. */
static gboolean
suggest_has_target(GtkListBox *list, const char *target)
{
    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(list)); c;
         c = gtk_widget_get_next_sibling(c)) {
        const char *t = g_object_get_data(G_OBJECT(c), "ns-target");
        if (t && strcmp(t, target) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
suggest_clear(GtkListBox *list)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(GTK_WIDGET(list))))
        gtk_list_box_remove(list, c);
}

/* Case-folded substring test against an already case-folded needle. */
static gboolean
text_contains_fold(const char *haystack, const char *needle_fold)
{
    if (!haystack || !*haystack)
        return FALSE;
    char *h = g_utf8_casefold(haystack, -1);
    gboolean hit = strstr(h, needle_fold) != NULL;
    g_free(h);
    return hit;
}

/* Rebuilds the address suggestions from search, bookmarks and history and shows the popover. */
static void
suggest_refresh(ProcWindow *pw)
{
    char *text =
        g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(pw->address))));
    if (!*text) {
        g_free(text);
        suggest_hide(pw);
        return;
    }
    g_free(pw->suggest_typed);
    pw->suggest_typed = text;
    GtkListBox *list = GTK_LIST_BOX(pw->suggest_list);
    suggest_clear(list);
    guint rows = 0;
    gboolean offer_search = !strstr(text, "://") &&
                            !g_str_has_prefix(text, "about:") &&
                            !g_str_has_prefix(text, "file:");
    gboolean search_first = offer_search && ns_address_is_search(text);
    char *search_url = ns_search_url_for(text);
    char *search_label = g_strconcat(ns_i18n("Search for"), " “", text, "”",
                                     NULL);
    if (search_first) {
        gtk_list_box_append(list, suggest_row_new(search_label, NULL,
                                                  search_url));
        rows++;
    }
    char *fold = g_utf8_casefold(text, -1);
    guint n = pw->bookmarks ? ns_bookmarks_count(pw->bookmarks) : 0;
    guint bookmark_rows = 0;
    for (guint i = 0; i < n && bookmark_rows < NS_SUGGEST_BOOKMARKS; i++) {
        const ns_bookmark *b = ns_bookmarks_get(pw->bookmarks, i);
        if (!text_contains_fold(b->url, fold) &&
            !text_contains_fold(b->title, fold))
            continue;
        if (suggest_has_target(list, b->url))
            continue;
        const char *title = b->title && *b->title ? b->title : b->url;
        gtk_list_box_append(list, suggest_row_new(title, b->url, b->url));
        rows++;
        bookmark_rows++;
    }
    GPtrArray *hits = ns_history_search(text, NS_SUGGEST_MAX);
    for (guint i = 0; i < hits->len && rows < NS_SUGGEST_MAX; i++) {
        ns_history_match *m = g_ptr_array_index(hits, i);
        if (suggest_has_target(list, m->url))
            continue;
        const char *title = *m->title ? m->title : m->url;
        gtk_list_box_append(list, suggest_row_new(title, m->url, m->url));
        rows++;
    }
    g_ptr_array_unref(hits);
    if (offer_search && !search_first) {
        gtk_list_box_append(list, suggest_row_new(search_label, NULL,
                                                  search_url));
        rows++;
    }
    g_free(fold);
    g_free(search_label);
    g_free(search_url);
    if (!rows) {
        suggest_hide(pw);
        return;
    }
    gtk_list_box_unselect_all(list);
    gtk_widget_set_size_request(pw->suggest_popover,
                                gtk_widget_get_width(pw->address), -1);
    if (!gtk_widget_get_visible(pw->suggest_popover))
        gtk_popover_popup(GTK_POPOVER(pw->suggest_popover));
    else
        gtk_popover_present(GTK_POPOVER(pw->suggest_popover));
}

/* Refreshes suggestions on user edits while the address bar has focus. */
static void
on_address_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    ProcWindow *pw = user_data;
    if (pw->address_quiet || !pw->suggest_popover)
        return;
    if (!gtk_event_controller_focus_contains_focus(
            GTK_EVENT_CONTROLLER_FOCUS(pw->address_focus)))
        return;
    suggest_refresh(pw);
}

/* Navigates to a suggestion row's target and moves focus to the page. */
static void
suggest_load(ProcWindow *pw, GtkListBoxRow *row)
{
    char *target = g_strdup(g_object_get_data(G_OBJECT(row), "ns-target"));
    suggest_hide(pw);
    if (!target)
        return;
    NsProcView *v = current_view(pw);
    address_set_text_quiet(pw, target);
    ns_proc_view_load(v, target);
    if (v)
        ns_proc_view_focus(v);
    g_free(target);
}

static void
on_suggest_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer ud)
{
    (void)list;
    suggest_load(ud, row);
}

/* Moves the suggestion selection by delta, wrapping through "none" back to the typed text. */
static void
suggest_move(ProcWindow *pw, int delta)
{
    GtkListBox *list = GTK_LIST_BOX(pw->suggest_list);
    int count = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(list)); c;
         c = gtk_widget_get_next_sibling(c))
        count++;
    if (!count)
        return;
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(list);
    int index = selected ? gtk_list_box_row_get_index(selected) : -1;
    index += delta;
    if (index < -1)
        index = count - 1;
    else if (index >= count)
        index = -1;
    if (index < 0) {
        gtk_list_box_unselect_all(list);
        address_set_text_quiet(pw, pw->suggest_typed ? pw->suggest_typed : "");
    } else {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, index);
        gtk_list_box_select_row(list, row);
        const char *fill = g_object_get_data(G_OBJECT(row), "ns-fill");
        address_set_text_quiet(pw, fill ? fill
                                        : pw->suggest_typed ? pw->suggest_typed
                                                            : "");
    }
    gtk_editable_set_position(GTK_EDITABLE(pw->address), -1);
}

/* Handles Esc/Up/Down/Enter for the open suggestion popover; returns TRUE if consumed. */
static gboolean
on_address_suggest_key(ProcWindow *pw, guint keyval, GdkModifierType state)
{
    if (!pw->suggest_popover || !gtk_widget_get_visible(pw->suggest_popover))
        return FALSE;
    if (keyval == GDK_KEY_Escape) {
        suggest_hide(pw);
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
        suggest_move(pw, keyval == GDK_KEY_Down ? 1 : -1);
        return TRUE;
    }
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        !(state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
        GtkListBoxRow *row =
            gtk_list_box_get_selected_row(GTK_LIST_BOX(pw->suggest_list));
        if (row) {
            suggest_load(pw, row);
            return TRUE;
        }
        suggest_hide(pw);
    }
    return FALSE;
}

/* Creates the non-focusable, non-autohide suggestion popover parented to the address bar. */
static GtkWidget *
suggest_popover_new(ProcWindow *pw)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "ns-suggest");
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_widget_set_can_focus(popover, FALSE);
    pw->suggest_list = gtk_list_box_new();
    gtk_widget_set_focusable(pw->suggest_list, FALSE);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(pw->suggest_list),
                                    GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(pw->suggest_list),
                                              TRUE);
    g_signal_connect(pw->suggest_list, "row-activated",
                     G_CALLBACK(on_suggest_row_activated), pw);
    gtk_popover_set_child(GTK_POPOVER(popover), pw->suggest_list);
    gtk_widget_set_parent(popover, pw->address);
    return popover;
}

/* Unparents the suggestion popover when the address entry is destroyed. */
static void
on_address_destroy_unparent_suggest(GtkWidget *address, gpointer user_data)
{
    (void)address;
    ProcWindow *pw = user_data;
    if (pw->suggest_popover) {
        gtk_widget_unparent(pw->suggest_popover);
        pw->suggest_popover = NULL;
        pw->suggest_list = NULL;
    }
}

/* Loads the normalized address-bar text in the current view and focuses the page. */
static void
on_address_activate(GtkEntry *entry, gpointer user_data)
{
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    suggest_hide(pw);
    char *resolved = normalize_url(gtk_editable_get_text(GTK_EDITABLE(entry)));
    if (!*resolved) {
        g_free(resolved);
        return;
    }
    address_set_text_quiet(pw, resolved);
    ns_proc_view_load(v, resolved);
    if (v)
        ns_proc_view_focus(v);
    g_free(resolved);
}

/* Ctrl/Shift+Enter completes a bare name to www.NAME.com/.net/.org and navigates. */
static gboolean
on_address_key_pressed(GtkEventControllerKey *controller, guint keyval,
                       guint keycode, GdkModifierType state, gpointer ud)
{
    (void)controller;
    (void)keycode;
    ProcWindow *pw = ud;
    if (on_address_suggest_key(pw, keyval, state))
        return TRUE;
    if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter)
        return FALSE;
    gboolean ctrl  = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    if (!ctrl && !shift)
        return FALSE;
    char *text =
        g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(pw->address))));
    if (!*text || strstr(text, "://") || strchr(text, ' ') ||
        g_str_has_prefix(text, "about:") ||
        g_str_has_prefix(text, "file:") ||
        g_str_has_prefix(text, "data:") ||
        g_str_has_prefix(text, "view-source:")) {
        g_free(text);
        return FALSE;
    }
    const char *suffix = ctrl && shift ? ".org" : shift ? ".net" : ".com";
    const char *prefix = g_str_has_prefix(text, "www.") ? "" : "www.";
    char *host = g_str_has_suffix(text, suffix)
        ? g_strconcat(prefix, text, NULL)
        : g_strconcat(prefix, text, suffix, NULL);
    g_free(text);
    address_set_text_quiet(pw, host);
    g_free(host);
    on_address_activate(GTK_ENTRY(pw->address), pw);
    return TRUE;
}

static void
on_back_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_back(v);
}

static void
on_forward_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_forward(v);
}

static void
on_reload_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_reload(v);
}

static void
on_stop_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_stop(v);
}

static void
on_home_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    ProcWindow *pw = ud;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, pw->home_url ? pw->home_url : "about:start");
}

static void
on_logo_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_load(v, "https://nordstjernen.org");
}

static void
on_print_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    ns_proc_view_print(current_view(ud));
}

static void
on_downloads_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    show_downloads_window(ud);
}

static void
act_back(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_back(v);
}

static void
act_forward(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_forward(v);
}

static void
act_reload(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_reload(v);
}

/* Clears the HTTP cache before reloading the current view. */
static void
act_hard_reload(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (!v) return;
    ns_cache_clear();
    ns_proc_view_reload(v);
}

static void
act_find(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_find_open(v);
}

static void
act_console(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_toggle_console(v);
}

/* Adds or removes the current page as a bookmark; returns FALSE if there is no page URL. */
static gboolean
toggle_bookmark_current(ProcWindow *pw)
{
    NsProcView *v = current_view(pw);
    if (!v || !pw->bookmarks)
        return FALSE;
    const char *url = ns_proc_view_url(v);
    if (!url || !*url)
        return FALSE;
    if (ns_bookmarks_contains(pw->bookmarks, url)) {
        ns_bookmarks_remove(pw->bookmarks, url);
        pw_set_status(pw, ns_i18n("Bookmark removed"));
    } else {
        ns_bookmarks_add(pw->bookmarks, url, ns_proc_view_title(v));
        pw_set_status(pw, ns_i18n("Bookmark added"));
    }
    update_bookmark_indicator(pw);
    return TRUE;
}

static void
act_bookmark_page(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    toggle_bookmark_current(ud);
}

/* Loads view-source: for the current URL unless it is already a source view. */
static void
act_view_source(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (!v) return;
    const char *url = ns_proc_view_url(v);
    if (!url || !*url || g_str_has_prefix(url, "view-source:"))
        return;
    char *src_url = g_strconcat("view-source:", url, NULL);
    ns_proc_view_load(v, src_url);
    g_free(src_url);
}

static void
act_home(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, pw->home_url ? pw->home_url : "about:start");
}

static void
act_focus_address(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    gtk_widget_grab_focus(pw->address);
    gtk_editable_select_region(GTK_EDITABLE(pw->address), 0, -1);
}

/* Esc handler: reverts address edits, else closes find bar or stops loading; focuses page. */
static void
act_focus_page(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    NsProcView *v = current_view(pw);
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(pw->window));
    gboolean editing_address = focus && pw->address &&
        (focus == pw->address || gtk_widget_is_ancestor(focus, pw->address));
    if (editing_address)
        set_address_text(pw, v ? ns_proc_view_url(v) : "");
    else if (ns_proc_view_find_close(v))
        return;
    else if (v && ns_proc_view_is_loading(v))
        ns_proc_view_stop(v);
    if (v)
        ns_proc_view_focus(v);
}

static void
act_zoom_in(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_in(v);
}

static void
act_zoom_out(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_out(v);
}

static void
act_zoom_reset(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_reset(v);
}

static void
act_quit(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    gtk_window_close(GTK_WINDOW(pw->window));
}

static void act_about(GSimpleAction *action, GVariant *parameter,
                      gpointer user_data);
static void act_settings(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data);
static void act_new_window(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data);
static void act_history(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data);
static void act_print(GSimpleAction *action, GVariant *parameter,
                      gpointer user_data);
static void act_save_pdf(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data);
static void act_save_image(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data);
static void act_fullscreen(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data);
static void on_bookmarks_clicked(GtkButton *button, gpointer user_data);

static void
act_downloads(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    show_downloads_window((ProcWindow *)user_data);
}

/* Registers a win.NAME action bound to cb and sets its keyboard accelerators. */
static void
install_action(ProcWindow *pw, const char *name, GCallback cb,
               const char *const *accels)
{
    GSimpleAction *act = g_simple_action_new(name, NULL);
    g_signal_connect(act, "activate", cb, pw);
    g_action_map_add_action(G_ACTION_MAP(pw->window), G_ACTION(act));
    g_object_unref(act);
    if (accels) {
        char *full = g_strconcat("win.", name, NULL);
        gtk_application_set_accels_for_action(pw->app, full, accels);
        g_free(full);
    }
}

/* Registers all window actions and their keyboard shortcuts. */
static void
install_shortcuts(ProcWindow *pw)
{
    install_action(pw, "back", G_CALLBACK(act_back),
                   (const char *[]){ "<Alt>Left", NULL });
    install_action(pw, "forward", G_CALLBACK(act_forward),
                   (const char *[]){ "<Alt>Right", NULL });
    install_action(pw, "reload", G_CALLBACK(act_reload),
                   (const char *[]){ "<Ctrl>r", "F5", NULL });
    install_action(pw, "hard-reload", G_CALLBACK(act_hard_reload),
                   (const char *[]){ "<Ctrl><Shift>r", "<Ctrl>F5", NULL });
    install_action(pw, "find", G_CALLBACK(act_find),
                   (const char *[]){ "<Ctrl>f", NULL });
    install_action(pw, "console", G_CALLBACK(act_console),
                   (const char *[]){ "<Ctrl><Shift>j", "F12", NULL });
    install_action(pw, "view-source", G_CALLBACK(act_view_source),
                   (const char *[]){ "<Ctrl>u", NULL });
    install_action(pw, "bookmark-page", G_CALLBACK(act_bookmark_page),
                   (const char *[]){ "<Ctrl>d", NULL });
    install_action(pw, "home", G_CALLBACK(act_home),
                   (const char *[]){ "<Alt>Home", NULL });
    install_action(pw, "focus-address", G_CALLBACK(act_focus_address),
                   (const char *[]){ "<Ctrl>l", NULL });
    install_action(pw, "focus-page", G_CALLBACK(act_focus_page),
                   (const char *[]){ "Escape", NULL });
    install_action(pw, "zoom-in", G_CALLBACK(act_zoom_in),
                   (const char *[]){ "<Ctrl>plus", "<Ctrl>equal",
                                     "<Ctrl>KP_Add", NULL });
    install_action(pw, "zoom-out", G_CALLBACK(act_zoom_out),
                   (const char *[]){ "<Ctrl>minus", "<Ctrl>KP_Subtract",
                                     NULL });
    install_action(pw, "zoom-reset", G_CALLBACK(act_zoom_reset),
                   (const char *[]){ "<Ctrl>0", "<Ctrl>KP_0", NULL });
    install_action(pw, "downloads", G_CALLBACK(act_downloads),
                   (const char *[]){ "<Ctrl>j", NULL });
    install_action(pw, "about", G_CALLBACK(act_about), NULL);
    install_action(pw, "new-window", G_CALLBACK(act_new_window),
                   (const char *[]){ "<Ctrl>n", NULL });
    install_action(pw, "history", G_CALLBACK(act_history),
                   (const char *[]){ "<Ctrl>h", NULL });
    install_action(pw, "print", G_CALLBACK(act_print),
                   (const char *[]){ "<Ctrl>p", NULL });
    install_action(pw, "save-pdf", G_CALLBACK(act_save_pdf),
                   (const char *[]){ "<Ctrl><Shift>p", NULL });
    install_action(pw, "save-image", G_CALLBACK(act_save_image), NULL);
    install_action(pw, "fullscreen", G_CALLBACK(act_fullscreen),
                   (const char *[]){ "F11", NULL });
    install_action(pw, "settings", G_CALLBACK(act_settings),
                   (const char *[]){ "<Ctrl>comma", NULL });
    install_action(pw, "quit", G_CALLBACK(act_quit),
                   (const char *[]){ "<Ctrl>q", NULL });
}

/* Capture-phase handler: Esc leaves fullscreen and re-maximizes the window. */
static gboolean
on_window_key_pressed(GtkEventControllerKey *controller, guint keyval,
                      guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    ProcWindow *pw = user_data;
    GtkWindow *win = GTK_WINDOW(pw->window);
    if (keyval == GDK_KEY_Escape && gtk_window_is_fullscreen(win)) {
        gtk_window_unfullscreen(win);
        gtk_window_maximize(win);
        return TRUE;
    }
    return FALSE;
}

static void
menu_append_accel(GMenu *menu, const char *label, const char *action,
                  const char *accel)
{
    GMenuItem *item = g_menu_item_new(label, action);
    if (accel)
        g_menu_item_set_attribute(item, "accel", "s", accel);
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

static void
menu_append_section(GMenu *menu, GMenu *section)
{
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
}

/* Builds the sectioned app menu model with accelerator hints. */
static GMenuModel *
build_app_menu_model(void)
{
    GMenu *menu = g_menu_new();

    GMenu *window = g_menu_new();
    menu_append_accel(window, ns_i18n("New Window"), "win.new-window",
                      "<Ctrl>n");
    menu_append_section(menu, window);

    GMenu *view = g_menu_new();
    menu_append_accel(view, ns_i18n("Zoom In"), "win.zoom-in", "<Ctrl>plus");
    menu_append_accel(view, ns_i18n("Zoom Out"), "win.zoom-out",
                      "<Ctrl>minus");
    menu_append_accel(view, ns_i18n("Reset Zoom"), "win.zoom-reset",
                      "<Ctrl>0");
    menu_append_accel(view, ns_i18n("Full Screen"), "win.fullscreen", "F11");
    menu_append_accel(view, ns_i18n("Find in Page"), "win.find", "<Ctrl>f");
    menu_append_section(menu, view);

    GMenu *page = g_menu_new();
    menu_append_accel(page, ns_i18n("Bookmark This Page"),
                      "win.bookmark-page", "<Ctrl>d");
    menu_append_accel(page, ns_i18n("History"), "win.history", "<Ctrl>h");
    menu_append_accel(page, ns_i18n("Downloads"), "win.downloads", "<Ctrl>j");
    menu_append_accel(page, ns_i18n("Print…"), "win.print", "<Ctrl>p");
    menu_append_accel(page, ns_i18n("Save Page as PDF…"), "win.save-pdf",
                      "<Ctrl><Shift>p");
    menu_append_accel(page, ns_i18n("Save Page as Image…"),
                      "win.save-image", NULL);
    menu_append_section(menu, page);

    GMenu *tools = g_menu_new();
    menu_append_accel(tools, ns_i18n("Page Source"), "win.view-source",
                      "<Ctrl>u");
    menu_append_accel(tools, ns_i18n("JavaScript Console"), "win.console",
                      "<Ctrl><Shift>j");
    menu_append_accel(tools, ns_i18n("Settings"), "win.settings",
                      "<Ctrl>comma");
    menu_append_section(menu, tools);

    GMenu *about = g_menu_new();
    g_menu_append(about, ns_i18n("About Northstar Web Browser"), "win.about");
    menu_append_section(menu, about);

    GMenu *quit = g_menu_new();
    menu_append_accel(quit, ns_i18n("Quit"), "win.quit", "<Ctrl>q");
    menu_append_section(menu, quit);

    return G_MENU_MODEL(menu);
}

/* Creates a browser window with toolbar, address bar, view, actions and theme watchers. */
static ProcWindow *
proc_window_new(GtkApplication *app, const char *home_url,
                gboolean private_mode)
{
    ProcWindow *pw = g_new0(ProcWindow, 1);
    pw->app = app;
    pw->home_url = g_strdup(home_url && *home_url ? home_url : "about:start");
    pw->bookmarks = ns_bookmarks_load();
    pw->view = ns_proc_view_new();
    ns_proc_view_set_private(pw->view, private_mode);
    ns_proc_view_set_notify(pw->view, on_view_notify, pw);
    pw->window = gtk_application_window_new(app);
    g_object_set_data_full(G_OBJECT(pw->window), "ns-procwindow", pw,
                           (GDestroyNotify)procwindow_free);
    gtk_window_set_title(GTK_WINDOW(pw->window), ns_brand_versioned());
    if (g_initial_win_w > 0 && g_initial_win_h > 0) {
        gtk_window_set_default_size(GTK_WINDOW(pw->window),
                                    g_initial_win_w, g_initial_win_h);
    } else {
        gtk_window_set_default_size(GTK_WINDOW(pw->window), 1024, 768);
        gtk_window_maximize(GTK_WINDOW(pw->window));
    }

    GtkEventController *winkeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(winkeys, GTK_PHASE_CAPTURE);
    g_signal_connect(winkeys, "key-pressed",
                     G_CALLBACK(on_window_key_pressed), pw);
    gtk_widget_add_controller(pw->window, winkeys);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(pw->window), header);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(toolbar, "ns-toolbar");
    gtk_widget_set_margin_top(toolbar, 1);
    gtk_widget_set_margin_bottom(toolbar, 1);
    gtk_widget_set_margin_start(toolbar, 4);
    gtk_widget_set_margin_end(toolbar, 4);

    pw->back = toolbar_button("northstar-back", ns_i18n("Back"),
                              ns_i18n("Go back one page"),
                              G_CALLBACK(on_back_clicked), pw);
    pw->forward = toolbar_button("northstar-forward", ns_i18n("Forward"),
                                 ns_i18n("Go forward one page"),
                                 G_CALLBACK(on_forward_clicked), pw);
    pw->reload = toolbar_button("northstar-reload", ns_i18n("Reload"),
                                ns_i18n("Reload this page"),
                                G_CALLBACK(on_reload_clicked), pw);
    pw->stop = toolbar_button("northstar-stop", ns_i18n("Stop"),
                              ns_i18n("Stop loading this page"),
                              G_CALLBACK(on_stop_clicked), pw);
    gtk_widget_set_sensitive(pw->stop, FALSE);
    GtkWidget *home = toolbar_button("northstar-home", ns_i18n("Home"),
                                     ns_i18n("Go to the home page"),
                                     G_CALLBACK(on_home_clicked), pw);
    GtkWidget *print = toolbar_button("northstar-print", ns_i18n("Print"),
                                      ns_i18n("Print this page"),
                                      G_CALLBACK(on_print_clicked), pw);
    GtkWidget *downloads = toolbar_button("northstar-downloads",
                                          ns_i18n("Downloads"),
                                          ns_i18n("Show downloads"),
                                          G_CALLBACK(on_downloads_clicked), pw);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(sep, "ns-toolbar-separator");
    toolbar_button_icon_size(pw->back, 20);
    toolbar_button_icon_size(pw->forward, 20);
    toolbar_button_icon_size(pw->reload, 20);
    toolbar_button_icon_size(pw->stop, 20);
    toolbar_button_icon_size(home, 20);
    toolbar_button_icon_size(print, 20);
    toolbar_button_icon_size(downloads, 20);

    pw->spinner = gtk_spinner_new();
    gtk_widget_set_tooltip_text(pw->spinner, ns_i18n("Loading"));
    gtk_widget_set_valign(pw->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(pw->spinner, FALSE);

    pw->address = gtk_entry_new();
    gtk_widget_set_hexpand(pw->address, TRUE);
    gtk_widget_set_valign(pw->address, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(pw->address, "ns-address");
    ns_icon_entry_set(GTK_ENTRY(pw->address), GTK_ENTRY_ICON_PRIMARY,
                      "northstar-bookmarks");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(pw->address),
                                    GTK_ENTRY_ICON_PRIMARY,
                                    ns_i18n("Page location"));
    gtk_entry_set_placeholder_text(GTK_ENTRY(pw->address),
                                   ns_i18n("Search or enter a URL"));
    set_accessible_label(pw->address, ns_i18n("Address and search bar"));
    g_signal_connect(pw->address, "activate",
                     G_CALLBACK(on_address_activate), pw);
    GtkEventController *addr_focus = gtk_event_controller_focus_new();
    g_signal_connect(addr_focus, "enter",
                     G_CALLBACK(on_address_focus_enter), pw);
    g_signal_connect(addr_focus, "leave",
                     G_CALLBACK(on_address_focus_leave), pw);
    gtk_widget_add_controller(pw->address, addr_focus);
    pw->address_focus = addr_focus;
    pw->suggest_popover = suggest_popover_new(pw);
    g_signal_connect(pw->address, "changed",
                     G_CALLBACK(on_address_changed), pw);
    g_signal_connect(pw->address, "destroy",
                     G_CALLBACK(on_address_destroy_unparent_suggest), pw);
    GtkEventController *addr_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(addr_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(addr_keys, "key-pressed",
                     G_CALLBACK(on_address_key_pressed), pw);
    gtk_widget_add_controller(pw->address, addr_keys);

    pw->zoom_button = gtk_button_new_with_label("100%");
    gtk_button_set_has_frame(GTK_BUTTON(pw->zoom_button), FALSE);
    gtk_widget_add_css_class(pw->zoom_button, "ns-zoom");
    gtk_widget_set_tooltip_text(pw->zoom_button,
                                ns_i18n("Reset zoom (Ctrl+0)"));
    set_accessible_label(pw->zoom_button, ns_i18n("Reset zoom"));
    gtk_widget_set_visible(pw->zoom_button, FALSE);
    g_signal_connect(pw->zoom_button, "clicked",
                     G_CALLBACK(on_zoom_indicator_clicked), pw);

    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(sep2, "ns-toolbar-separator");
    pw->bookmarks_button = toolbar_button("northstar-bookmarks",
                                          ns_i18n("Bookmarks"),
                                          ns_i18n("Bookmarks"),
                                          G_CALLBACK(on_bookmarks_clicked), pw);
    toolbar_button_icon_size(pw->bookmarks_button, 20);

    GMenuModel *appmenu = build_app_menu_model();
    GtkWidget *menu_button = gtk_menu_button_new();
    GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(menu_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(menu_box, GTK_ALIGN_CENTER);
    GtkWidget *menu_icon = gtk_image_new_from_icon_name("open-menu-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(menu_icon), 20);
    GtkWidget *menu_label = gtk_label_new(ns_i18n("Menu"));
    gtk_widget_add_css_class(menu_label, "ns-toolbar-label");
    gtk_box_append(GTK_BOX(menu_box), menu_icon);
    gtk_box_append(GTK_BOX(menu_box), menu_label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_button), menu_box);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), appmenu);
    ns_popover_menu_fit(GTK_WIDGET(gtk_menu_button_get_popover(
                            GTK_MENU_BUTTON(menu_button))));
    gtk_widget_set_tooltip_text(menu_button, ns_i18n("Menu"));
    set_accessible_label(menu_button, ns_i18n("Menu"));
    gtk_widget_set_valign(menu_button, GTK_ALIGN_CENTER);
    g_object_unref(appmenu);

    GtkWidget *logo = ns_icon_image_new("northstar");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 22);
    GtkWidget *logo_button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(logo_button), logo);
    gtk_button_set_has_frame(GTK_BUTTON(logo_button), FALSE);
    gtk_widget_add_css_class(logo_button, "ns-throbber");
    gtk_widget_set_tooltip_text(logo_button, ns_i18n("Visit nordstjernen.org"));
    set_accessible_label(logo_button, ns_i18n("Visit nordstjernen.org"));
    g_signal_connect(logo_button, "clicked", G_CALLBACK(on_logo_clicked), pw);

    gtk_box_append(GTK_BOX(toolbar), pw->back);
    gtk_box_append(GTK_BOX(toolbar), pw->forward);
    gtk_box_append(GTK_BOX(toolbar), pw->reload);
    gtk_box_append(GTK_BOX(toolbar), pw->stop);
    gtk_box_append(GTK_BOX(toolbar), home);
    gtk_box_append(GTK_BOX(toolbar), print);
    gtk_box_append(GTK_BOX(toolbar), downloads);
    gtk_box_append(GTK_BOX(toolbar), sep);
    gtk_box_append(GTK_BOX(toolbar), pw->spinner);
    gtk_box_append(GTK_BOX(toolbar), pw->address);
    gtk_box_append(GTK_BOX(toolbar), pw->zoom_button);
    gtk_box_append(GTK_BOX(toolbar), sep2);
    gtk_box_append(GTK_BOX(toolbar), pw->bookmarks_button);
    gtk_box_append(GTK_BOX(toolbar), menu_button);
    gtk_box_append(GTK_BOX(toolbar), logo_button);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    GtkWidget *page = ns_proc_view_widget(pw->view);
    gtk_widget_set_hexpand(page, TRUE);
    gtk_widget_set_vexpand(page, TRUE);

    pw->status = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(pw->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(pw->status), 90);
    gtk_widget_add_css_class(pw->status, "ns-procstatus");
    gtk_widget_set_halign(pw->status, GTK_ALIGN_START);
    gtk_widget_set_valign(pw->status, GTK_ALIGN_END);
    gtk_widget_set_can_target(pw->status, FALSE);
    gtk_widget_set_visible(pw->status, FALSE);

    pw->fullscreen_notice = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(pw->fullscreen_notice),
                            PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(pw->fullscreen_notice), 80);
    gtk_widget_add_css_class(pw->fullscreen_notice, "ns-fullscreen-notice");
    gtk_widget_set_halign(pw->fullscreen_notice, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(pw->fullscreen_notice, GTK_ALIGN_START);
    gtk_widget_set_can_target(pw->fullscreen_notice, FALSE);
    gtk_widget_set_visible(pw->fullscreen_notice, FALSE);
    g_signal_connect(pw->window, "notify::fullscreened",
                     G_CALLBACK(on_window_fullscreened), pw);

    GtkWidget *page_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(page_overlay), page);
    gtk_overlay_add_overlay(GTK_OVERLAY(page_overlay), pw->status);
    gtk_overlay_add_overlay(GTK_OVERLAY(page_overlay), pw->fullscreen_notice);
    gtk_widget_set_hexpand(page_overlay, TRUE);
    gtk_widget_set_vexpand(page_overlay, TRUE);
    gtk_box_append(GTK_BOX(vbox), page_overlay);

    gtk_window_set_child(GTK_WINDOW(pw->window), vbox);
    install_shortcuts(pw);

    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        pw->theme_watch[0] = g_signal_connect(
            settings, "notify::gtk-theme-name",
            G_CALLBACK(on_theme_changed), pw);
        pw->theme_watch[1] = g_signal_connect(
            settings, "notify::gtk-application-prefer-dark-theme",
            G_CALLBACK(on_theme_changed), pw);
    }

    return pw;
}

static void
act_about(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, "about:northstar");
}

static void
act_settings(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, "about:settings");
}

static void
act_history(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, "about:history");
}

/* Opens a new window with the same home URL and private mode, loading home. */
static void
act_new_window(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    ProcWindow *nw = proc_window_new(pw->app, pw->home_url,
                                     ns_proc_view_is_private(pw->view));
    gtk_window_present(GTK_WINDOW(nw->window));
    proc_window_load(nw, nw->home_url);
}

static void
act_print(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ns_proc_view_print(current_view(user_data));
}

static void
act_save_pdf(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ns_proc_view_save_pdf(current_view(user_data));
}

static void
act_save_image(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ns_proc_view_save_image(current_view(user_data));
}

/* Toggles fullscreen; leaving it re-maximizes the window. */
static void
act_fullscreen(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    GtkWindow *win = GTK_WINDOW(pw->window);
    if (gtk_window_is_fullscreen(win)) {
        gtk_window_unfullscreen(win);
        gtk_window_maximize(win);
    } else {
        gtk_window_fullscreen(win);
    }
}

/* Loads the clicked bookmark's URL and closes the enclosing popover. */
static void
on_bookmark_activate(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "ns-bm-url");
    NsProcView *v = current_view(pw);
    if (url && v)
        ns_proc_view_load(v, url);
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(button),
                                             GTK_TYPE_POPOVER);
    if (pop)
        gtk_popover_popdown(GTK_POPOVER(pop));
}

/* Removes the bookmark and its row from the popover list. */
static void
on_bookmark_remove(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "ns-bm-url");
    if (url && pw->bookmarks) {
        ns_bookmarks_remove(pw->bookmarks, url);
        GtkWidget *row = gtk_widget_get_parent(GTK_WIDGET(button));
        GtkWidget *list = row ? gtk_widget_get_parent(row) : NULL;
        if (list && row)
            gtk_box_remove(GTK_BOX(list), row);
        update_bookmark_indicator(pw);
    }
}

static void
on_add_bookmark(GtkButton *button, gpointer user_data)
{
    if (!toggle_bookmark_current(user_data))
        return;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(button),
                                             GTK_TYPE_POPOVER);
    if (pop)
        gtk_popover_popdown(GTK_POPOVER(pop));
}

/* Builds the bookmarks popover: add/remove-current button plus a scrollable list. */
static GtkWidget *
build_bookmarks_popover(ProcWindow *pw)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_size_request(box, 320, -1);

    GtkWidget *add = gtk_button_new_with_label(
        current_page_bookmarked(pw) ? ns_i18n("Remove this bookmark")
                                    : ns_i18n("Bookmark this page"));
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_bookmark), pw);
    gtk_box_append(GTK_BOX(box), add);
    gtk_box_append(GTK_BOX(box),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),
                                               320);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    guint n = pw->bookmarks ? ns_bookmarks_count(pw->bookmarks) : 0;
    if (n == 0) {
        GtkWidget *empty = gtk_label_new(ns_i18n("No bookmarks yet"));
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_widget_set_margin_top(empty, 12);
        gtk_widget_set_margin_bottom(empty, 12);
        gtk_box_append(GTK_BOX(list), empty);
    }
    for (guint i = 0; i < n; i++) {
        const ns_bookmark *bm = ns_bookmarks_get(pw->bookmarks, i);
        if (!bm || !bm->url) continue;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *open = gtk_button_new_with_label(
            (bm->title && *bm->title) ? bm->title : bm->url);
        gtk_button_set_has_frame(GTK_BUTTON(open), FALSE);
        gtk_widget_set_hexpand(open, TRUE);
        gtk_widget_set_halign(open, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(open, bm->url);
        g_object_set_data_full(G_OBJECT(open), "ns-bm-url",
                               g_strdup(bm->url), g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(on_bookmark_activate), pw);
        GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(del), FALSE);
        gtk_widget_set_tooltip_text(del, ns_i18n("Remove bookmark"));
        set_accessible_label(del, ns_i18n("Remove bookmark"));
        g_object_set_data_full(G_OBJECT(del), "ns-bm-url",
                               g_strdup(bm->url), g_free);
        g_signal_connect(del, "clicked", G_CALLBACK(on_bookmark_remove), pw);
        gtk_box_append(GTK_BOX(row), open);
        gtk_box_append(GTK_BOX(row), del);
        gtk_box_append(GTK_BOX(list), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(box), scroll);

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

/* Unparents and drops a closed popover from an idle, outside its signal emission. */
static gboolean
popover_unparent_idle(gpointer pop)
{
    if (gtk_widget_get_parent(GTK_WIDGET(pop)))
        gtk_widget_unparent(GTK_WIDGET(pop));
    g_object_unref(pop);
    return G_SOURCE_REMOVE;
}

static void
on_popover_closed_unparent(GtkPopover *pop, gpointer user_data)
{
    (void)user_data;
    g_idle_add(popover_unparent_idle, g_object_ref(pop));
}

/* Builds a fresh bookmarks popover under the button; it is unparented when closed. */
static void
on_bookmarks_clicked(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = user_data;
    GtkWidget *pop = build_bookmarks_popover(pw);
    gtk_widget_set_parent(pop, GTK_WIDGET(button));
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    g_signal_connect(pop, "closed",
                     G_CALLBACK(on_popover_closed_unparent), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

typedef struct {
    char    *url;
    char    *session_path;
    gboolean recover;
    gboolean private_mode;
} ProcAppCtx;

/* Returns TRUE for http, https, ftp and file URLs that may be restored after a crash. */
static gboolean
session_url_recoverable(const char *u)
{
    return u && (g_str_has_prefix(u, "http://") ||
                 g_str_has_prefix(u, "https://") ||
                 g_str_has_prefix(u, "ftp://") ||
                 g_str_has_prefix(u, "file://"));
}

/* Periodically writes the current URL to the session file (empty when private). */
static gboolean
write_session_cb(gpointer data)
{
    ProcWindow *pw = data;
    if (!pw->session_path)
        return G_SOURCE_REMOVE;
    const char *url = ns_proc_view_url(pw->view);
    char *contents = !ns_proc_view_is_private(pw->view) &&
                     session_url_recoverable(url)
                   ? g_strconcat(url, "\n", NULL) : g_strdup("");
    g_file_set_contents(pw->session_path, contents, -1, NULL);
    g_free(contents);
    return G_SOURCE_CONTINUE;
}

/* App activate: sets up media, icons and CSS, opens a window, loads recovered or start URL. */
static void
on_proc_activate(GtkApplication *app, gpointer user_data)
{
    ProcAppCtx *ctx = user_data;
    setlocale(LC_NUMERIC, "C");
    configure_media_inputs();
    install_icon_search_paths();
    ns_icon_install_window_icon("northstar");
    install_status_css();
    ProcWindow *pw = proc_window_new(app, "about:start", ctx->private_mode);
    pw->session_path = g_strdup(ctx->session_path);

    char *recovered_url = NULL;
    if (ctx->recover && !ctx->private_mode && ctx->session_path) {
        char *contents = NULL;
        if (g_file_get_contents(ctx->session_path, &contents, NULL, NULL)) {
            char **lines = g_strsplit(contents, "\n", -1);
            for (int i = 0; lines && lines[i]; i++) {
                if (session_url_recoverable(lines[i])) {
                    g_free(recovered_url);
                    recovered_url = g_strdup(lines[i]);
                }
            }
            g_strfreev(lines);
        }
        g_free(contents);
    }
    gtk_window_present(GTK_WINDOW(pw->window));
    apply_color_scheme(pw);
    proc_window_load(pw, recovered_url ? recovered_url
                                      : ctx->url ? ctx->url : "about:start");
    if (recovered_url)
        pw_set_status(pw, ns_i18n("Recovered the previous session after "
                                  "an unexpected exit"));
    g_free(recovered_url);

    if (pw->session_path)
        pw->session_timer = g_timeout_add_seconds(4, write_session_cb, pw);
}

/* Recursively deletes files older than min_age_s (all if <= 0) under a cache subdir. */
static void
procapp_clear_cache_dir(const char *name, gint64 min_age_s)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "northstar",
                                 name, NULL);
    gint64 cutoff = g_get_real_time() / G_USEC_PER_SEC - min_age_s;
    GQueue *stack = g_queue_new();
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_queue_push_head(stack, g_strdup(dir));
    guint guard = 0;
    while (!g_queue_is_empty(stack) && guard++ < 100000) {
        char *d = g_queue_pop_head(stack);
        GDir *gd = g_dir_open(d, 0, NULL);
        if (gd) {
            const char *e;
            while ((e = g_dir_read_name(gd))) {
                char *child = g_build_filename(d, e, NULL);
                if (g_file_test(child, G_FILE_TEST_IS_SYMLINK) ||
                    !g_file_test(child, G_FILE_TEST_IS_DIR)) {
                    GStatBuf st;
                    if (min_age_s <= 0 ||
                        (g_lstat(child, &st) == 0 && st.st_mtime < cutoff))
                        g_unlink(child);
                    g_free(child);
                } else {
                    g_queue_push_head(stack, child);
                }
            }
            g_dir_close(gd);
        }
        g_ptr_array_add(dirs, d);
    }
    for (guint i = dirs->len; i > 1; i--)
        g_rmdir(g_ptr_array_index(dirs, i - 1));
    g_queue_free_full(stack, g_free);
    g_ptr_array_free(dirs, TRUE);
    g_free(dir);
}

/* Prunes object caches (all at exit, else older than 1h) and stream caches older than 1h. */
static void
procapp_clear_http_caches(gboolean at_exit)
{
    static const char *const object_dirs[] = {
        "cache", "jsbc", "webfonts", "frames",
    };
    static const char *const stream_dirs[] = { "msaudio" };
    for (gsize i = 0; i < G_N_ELEMENTS(object_dirs); i++)
        procapp_clear_cache_dir(object_dirs[i], at_exit ? 0 : 3600);
    for (gsize i = 0; i < G_N_ELEMENTS(stream_dirs); i++)
        procapp_clear_cache_dir(stream_dirs[i], 3600);
}

/* Runs the GTK browser app, pruning caches before and after and shutting audio down. */
int
ns_procapp_run(const char *startup_url, const char *session_path,
               gboolean recover, gboolean private_mode)
{
    procapp_clear_http_caches(FALSE);
    ProcAppCtx ctx = {
        .url = g_strdup(startup_url),
        .session_path = g_strdup(session_path),
        .recover = recover,
        .private_mode = private_mode,
    };
    GtkApplication *app =
        gtk_application_new(NS_PROC_APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_proc_activate), &ctx);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    ns_audio_shutdown();
    procapp_clear_http_caches(TRUE);
    g_free(ctx.url);
    g_free(ctx.session_path);
    return status;
}
