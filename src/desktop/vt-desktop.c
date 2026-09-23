/*
 * vt-desktop.c — Desktop surface (root window)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hosts the wallpaper, optional desktop icons, and the desktop
 * context menu.
 */

#define VT_LOG_DOMAIN "desktop"
#include <vantage/vt-desktop.h>
#include <string.h>
#include <stdlib.h>

vt_desktop_t *vt_desktop_new(vt_renderer_t *r) {
    vt_desktop_t *d = vt_malloc0(sizeof(*d));
    d->renderer = r;
    vt_vec_init(&d->icons, sizeof(vt_desktop_icon_t), 8);
    return d;
}
void vt_desktop_free(vt_desktop_t *d) {
    if (!d) return;
    for (size_t i = 0; i < d->icons.size; i++) {
        vt_desktop_icon_t *ic = vt_vec_at(&d->icons, i);
        vt_free(ic->name); vt_free(ic->path); vt_free(ic->icon);
    }
    vt_vec_fini(&d->icons);
    vt_free(d);
}
int vt_desktop_start(vt_desktop_t *d) {
    if (!d) return VT_ERR_INVAL;
    d->running = true;
    vt_logi("desktop: started");
    return VT_OK;
}
void vt_desktop_stop(vt_desktop_t *d) {
    if (d) d->running = false;
}
void vt_desktop_set_show_icons(vt_desktop_t *d, bool on) {
    if (d) d->show_icons = on;
}
int vt_desktop_add_icon(vt_desktop_t *d, const char *name,
                        const char *path, const char *icon, int x, int y) {
    if (!d) return VT_ERR_INVAL;
    vt_desktop_icon_t ic = {
        .name = vt_strdup(name),
        .path = vt_strdup(path),
        .icon = vt_strdup(icon ? icon : ""),
        .x = x, .y = y, .w = 64, .h = 64,
    };
    if (!vt_vec_push(&d->icons, &ic)) return VT_ERR_NOMEM;
    return VT_OK;
}
void vt_desktop_remove_icon(vt_desktop_t *d, const char *name) {
    if (!d || !name) return;
    for (size_t i = 0; i < d->icons.size; i++) {
        vt_desktop_icon_t *ic = vt_vec_at(&d->icons, i);
        if (vt_streq(ic->name, name)) {
            vt_free(ic->name); vt_free(ic->path); vt_free(ic->icon);
            vt_vec_remove(&d->icons, i);
            return;
        }
    }
}
void vt_desktop_render(vt_desktop_t *d) {
    if (!d || !d->renderer) return;
    /* The desktop is below everything else, so its draw is a noop
     * without a compositor; the compositor calls this in the right
     * order. For now we just draw a fallback background. */
    vt_color_t bg = { 0.1f, 0.1f, 0.12f, 1.0f };
    vt_rect_t r = {0, 0, 1920, 1080};
    vt_renderer_fill_rect(d->renderer, r, bg);
}
void vt_desktop_show_menu(vt_desktop_t *d, int x, int y) {
    if (!d) return;
    vt_logi("desktop: context menu at (%d, %d)", x, y);
    /* TODO: open popup via compositor surface */
}
