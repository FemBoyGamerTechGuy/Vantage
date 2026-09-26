/*
 * vt-input.c — Input device event dispatching
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Uses libxkbcommon for layout handling when available. Falls back to
 * a hardcoded US layout when xkb is unavailable.
 */

#define VT_LOG_DOMAIN "input"
#include <vantage/vt-input.h>

#if defined(VT_HAVE_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

#include <string.h>
#include <stdlib.h>

struct vt_input {
    char  *layout;
    char  *variant;
    vt_input_cb_t cb;
    void  *ud;
#if defined(VT_HAVE_XKBCOMMON)
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_map;
    struct xkb_state   *xkb_state;
#endif
};

vt_input_t *vt_input_new(void) {
    vt_input_t *in = vt_malloc0(sizeof(*in));
    in->layout = vt_strdup("us");
    in->variant = vt_strdup("");
#if defined(VT_HAVE_XKBCOMMON)
    in->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (in->xkb_ctx) {
        struct xkb_rule_names names = {
            .rules = "evdev",
            .model = "pc105",
            .layout = in->layout,
            .variant = in->variant,
            .options = "",
        };
        in->xkb_map = xkb_keymap_new_from_names(in->xkb_ctx, &names,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (in->xkb_map)
            in->xkb_state = xkb_state_new(in->xkb_map);
    }
#endif
    return in;
}
void vt_input_free(vt_input_t *in) {
    if (!in) return;
    vt_free(in->layout); vt_free(in->variant);
#if defined(VT_HAVE_XKBCOMMON)
    if (in->xkb_state) xkb_state_unref(in->xkb_state);
    if (in->xkb_map)   xkb_keymap_unref(in->xkb_map);
    if (in->xkb_ctx)   xkb_context_unref(in->xkb_ctx);
#endif
    vt_free(in);
}
int vt_input_init(vt_input_t *in) { return in ? VT_OK : VT_ERR_INVAL; }
int vt_input_set_layout(vt_input_t *in, const char *layout, const char *variant) {
    if (!in) return VT_ERR_INVAL;
    vt_free(in->layout); vt_free(in->variant);
    in->layout = vt_strdup(layout ? layout : "us");
    in->variant = vt_strdup(variant ? variant : "");
#if defined(VT_HAVE_XKBCOMMON)
    if (in->xkb_state) { xkb_state_unref(in->xkb_state); in->xkb_state = NULL; }
    if (in->xkb_map) { xkb_keymap_unref(in->xkb_map); in->xkb_map = NULL; }
    if (in->xkb_ctx) {
        struct xkb_rule_names names = {
            .rules = "evdev", .model = "pc105",
            .layout = in->layout, .variant = in->variant,
            .options = "",
        };
        in->xkb_map = xkb_keymap_new_from_names(in->xkb_ctx, &names,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (in->xkb_map) in->xkb_state = xkb_state_new(in->xkb_map);
    }
#endif
    return VT_OK;
}
void vt_input_set_callback(vt_input_t *in, vt_input_cb_t cb, void *ud) {
    if (in) { in->cb = cb; in->ud = ud; }
}
void vt_input_dispatch(vt_input_t *in, const vt_input_event_t *ev) {
    if (in && in->cb) in->cb(ev, in->ud);
}
uint32_t vt_input_key_to_symsym(vt_input_t *in, uint32_t key) {
#if defined(VT_HAVE_XKBCOMMON)
    if (in && in->xkb_state) {
        return xkb_state_key_get_one_sym(in->xkb_state, key);
    }
#endif
    return key;
}
const char *vt_input_mod_str(uint32_t mods) {
    static __thread char buf[64];
    buf[0] = 0;
    if (mods & VT_MOD_SHIFT) strcat(buf, "Shift+");
    if (mods & VT_MOD_CTRL)  strcat(buf, "Ctrl+");
    if (mods & VT_MOD_ALT)   strcat(buf, "Alt+");
    if (mods & VT_MOD_SUPER) strcat(buf, "Super+");
    size_t l = strlen(buf);
    if (l && buf[l - 1] == '+') buf[l - 1] = 0;
    return buf;
}
