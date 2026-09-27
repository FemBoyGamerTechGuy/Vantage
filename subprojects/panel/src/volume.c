/*
 * volume.c — ALSA master volume applet
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Reads and sets the REAL mixer (Master → PCM → Front fallback chain)
 * through the ALSA C API; "vol --" when no card exists, exactly like
 * the compositor-side applet did. Wheel over the button changes the
 * volume; the popover shows a vertical slider + mute button.
 */
#include "volume.h"
#include "picon.h"

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#include <string.h>

typedef struct {
    GtkWidget  *button;
    GtkWidget  *icon;
    GtkWidget  *scale;
    GtkWidget  *mute_btn;
    char        card[64];
    char        elem[64];
    long        min, max;
    int         valid;      /* mixer opened */
#ifdef HAVE_ALSA
    snd_mixer_t *mixer;
    snd_mixer_elem_t *me;
#endif
} _vol_t;

static _vol_t V = {0};

#ifdef HAVE_ALSA

static void _open_mixer(void) {
    if (V.mixer) return;
    if (snd_mixer_open(&V.mixer, 0) != 0) { V.mixer = NULL; return; }
    snd_config_update_free_global();
    if (snd_mixer_attach(V.mixer, "default") != 0 ||
        snd_mixer_selem_register(V.mixer, NULL, NULL) != 0 ||
        snd_mixer_load(V.mixer) != 0) {
        snd_mixer_close(V.mixer);
        V.mixer = NULL;
        return;
    }
    static const char *names[] = { "Master", "PCM", "Front", NULL };
    V.me = NULL;
    for (int i = 0; names[i] && !V.me; i++) {
        snd_mixer_selem_id_t *sid = NULL;
        snd_mixer_selem_id_malloc(&sid);
        if (sid) {
            snd_mixer_selem_id_set_index(sid, 0);
            snd_mixer_selem_id_set_name(sid, names[i]);
            V.me = snd_mixer_find_selem(V.mixer, sid);
            free(sid);
        }
        snprintf(V.elem, sizeof(V.elem), "%s", names[i]);
    }
    if (!V.me) {
        snd_mixer_close(V.mixer);
        V.mixer = NULL;
        return;
    }
    snd_mixer_selem_get_playback_volume_range(V.me, &V.min, &V.max);
    V.valid = 1;
}

static int _get(long *out, int *mute_out) {
    _open_mixer();
    if (!V.valid || !V.me) return -1;
    long v = 0;
    if (snd_mixer_selem_get_playback_volume(V.me, SND_MIXER_SCHN_FRONT_LEFT,
                                            &v) != 0)
        return -1;
    int mute = 0;
    if (snd_mixer_selem_has_playback_switch(V.me))
        snd_mixer_selem_get_playback_switch(V.me, SND_MIXER_SCHN_FRONT_LEFT,
                                            &mute);
    if (out) *out = v;
    if (mute_out) *mute_out = !mute;
    return 0;
}

static void _set(long v, int mute) {
    _open_mixer();
    if (!V.valid || !V.me) return;
    if (v < V.min) v = V.min;
    if (v > V.max) v = V.max;
    snd_mixer_selem_set_playback_volume_all(V.me, v);
    if (snd_mixer_selem_has_playback_switch(V.me))
        snd_mixer_selem_set_playback_switch_all(V.me, !mute);
}

#else /* !HAVE_ALSA: honest stub */

static int _get(long *out, int *mute_out) {
    (void)mute_out;
    if (out) *out = 0;
    return -1;
}
static void _set(long v, int mute) { (void)v; (void)mute; }

#endif

static int _percent(void) {
    long v;
    int mute;
    if (_get(&v, &mute) != 0) return -1;
    if (V.max == V.min) return 0;
    return (int)((v - V.min) * 100 / (V.max - V.min));
}

static void _update_ui(void) {
    int p = _percent();
    long v;
    int mute = 0;
    int ok = _get(&v, &mute) == 0;
    const char *icon =
        !ok ? "audio-volume-muted-symbolic" :
        mute ? "audio-volume-muted-symbolic" :
        p < 33 ? "audio-volume-low-symbolic" :
        p < 66 ? "audio-volume-medium-symbolic" :
                 "audio-volume-high-symbolic";
    vp_glyph_t glyph =
        !ok || mute ? VP_GLYPH_VOLUME_MUTE :
        p < 33 ? VP_GLYPH_VOLUME_LOW :
        p < 66 ? VP_GLYPH_VOLUME_MED :
                 VP_GLYPH_VOLUME_HIGH;
    if (V.icon) {
        /* themed name when present, drawn speaker glyph otherwise —
         * unchecked set_from_icon_name can recurse to stack overflow
         * in GTK 4.18 on themes without image-missing */
        vp_icon_image_set(GTK_IMAGE(V.icon), icon, 18, glyph);
    }
    if (V.button) {
        char tip[64];
        if (!ok)
            snprintf(tip, sizeof(tip), "Volume: not available");
        else
            snprintf(tip, sizeof(tip), "Volume: %d%%%s",
                     p, mute ? " (muted)" : "");
        gtk_widget_set_tooltip_text(V.button, tip);
    }
    if (V.scale && ok)
        gtk_range_set_value(GTK_RANGE(V.scale), p);
}

static void _on_scale_value_changed(GtkRange *r, gpointer user) {
    (void)user;
    int p = (int)gtk_range_get_value(r);
    long v = V.min + (V.max - V.min) * p / 100;
    _set(v, 0);
    _update_ui();
}

static void _on_mute_toggled(GtkToggleButton *b, gpointer user) {
    (void)user;
    int mute = gtk_toggle_button_get_active(b);
    long v;
    _get(&v, NULL);
    _set(v, mute);
    _update_ui();
}

static gboolean _on_wheel(GtkEventControllerScroll *s, double dx, double dy,
                          gpointer user) {
    (void)s; (void)dx; (void)user;
    int p = _percent();
    if (p < 0) p = 50;
    p += (dy < 0 ? 5 : -5);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    long v = V.min + (V.max - V.min) * p / 100;
    _set(v, 0);
    _update_ui();
    return GDK_EVENT_STOP;
}

static gboolean _on_tick(gpointer user) {
    (void)user;
    _update_ui();
    return G_SOURCE_CONTINUE;
}

GtkWidget *volume_new(void) {
    V.icon = vp_icon_image_new("audio-volume-high-symbolic", 18,
                               VP_GLYPH_VOLUME_HIGH);

    V.button = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(V.button), V.icon);
    gtk_widget_add_css_class(V.button, "flat");
    gtk_widget_set_tooltip_text(V.button, "Volume");

    GtkWidget *pop = gtk_popover_new();
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);

    V.scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(V.scale), FALSE);
    gtk_widget_set_size_request(V.scale, -1, 140);
    gtk_range_set_inverted(GTK_RANGE(V.scale), TRUE);
    g_signal_connect(V.scale, "value-changed",
                     G_CALLBACK(_on_scale_value_changed), NULL);
    gtk_box_append(GTK_BOX(vbox), V.scale);

    V.mute_btn = gtk_toggle_button_new_with_label("Mute");
    g_signal_connect(V.mute_btn, "toggled",
                     G_CALLBACK(_on_mute_toggled), NULL);
    gtk_box_append(GTK_BOX(vbox), V.mute_btn);

    gtk_popover_set_child(GTK_POPOVER(pop), vbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(V.button), pop);

    GtkEventController *wheel = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(wheel, "scroll", G_CALLBACK(_on_wheel), NULL);
    gtk_widget_add_controller(V.button, GTK_EVENT_CONTROLLER(wheel));

    _update_ui();
    g_timeout_add_seconds(2, _on_tick, NULL);
    return V.button;
}
