/*
 * vt-input.h — Vantage input subsystem
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Unified input device enumeration (keyboard, pointer, touch) and event
 * dispatching. Uses libxkbcommon for keyboard layout handling, independent
 * of the display backend. Falls back to a simple US layout if xkb is not
 * available.
 */
#ifndef VANTAGE_INPUT_H
#define VANTAGE_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_INPUT_KEY_PRESS   = 1,
    VT_INPUT_KEY_RELEASE = 2,
    VT_INPUT_BUTTON_PRESS= 3,
    VT_INPUT_BUTTON_REL  = 4,
    VT_INPUT_MOTION       = 5,
    VT_INPUT_AXIS         = 6,
    VT_INPUT_TOUCH_DOWN  = 7,
    VT_INPUT_TOUCH_UP     = 8,
    VT_INPUT_TOUCH_MOTION= 9,
} vt_input_event_kind_t;

typedef struct vt_input_event {
    vt_input_event_kind_t kind;
    int64_t  time_us;
    int      device_id;
    uint32_t key;        /* keysym/keycode */
    uint32_t mods;       /* bitmask */
    int      x, y;
    int      dx, dy;
    int      button;
    float    axis;
} vt_input_event_t;

typedef struct vt_input vt_input_t;

typedef void (*vt_input_cb_t)(const vt_input_event_t *ev, void *ud);

vt_input_t *vt_input_new(void);
void        vt_input_free(vt_input_t *in);
int         vt_input_init(vt_input_t *in);
int         vt_input_set_layout(vt_input_t *in, const char *layout,
                                  const char *variant);
void        vt_input_set_callback(vt_input_t *in, vt_input_cb_t cb, void *ud);
void        vt_input_dispatch(vt_input_t *in, const vt_input_event_t *ev);

/* Convert keycode to keysym using xkbcommon if available. */
uint32_t    vt_input_key_to_symsym(vt_input_t *in, uint32_t key);

/* Modifier flags */
#define VT_MOD_SHIFT  (1u<<0)
#define VT_MOD_CTRL   (1u<<1)
#define VT_MOD_ALT    (1u<<2)
#define VT_MOD_SUPER  (1u<<3)
#define VT_MOD_CAPS   (1u<<4)
#define VT_MOD_NUM    (1u<<5)

const char *vt_input_mod_str(uint32_t mods);

#ifdef __cplusplus
}
#endif
#endif
