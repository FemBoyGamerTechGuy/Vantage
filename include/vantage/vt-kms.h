/*
 * vt-kms.h — DRM/KMS scanout path for the native Wayland compositor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Everything needed to put real pixels on a real screen:
 *   - /dev/dri/card* discovery with per-card driver logging
 *   - connected-connector + preferred-mode + CRTC selection
 *   - GBM scanout buffers (gbm_bo + AddFB2) with a dumb-buffer fallback
 *   - EGL display + GLES context for honest renderer reporting
 *   - async page flips (DRM_MODE_PAGE_FLIP_EVENT + drmHandleEvent)
 *   - 64x64 ARGB hardware cursor plane with graceful degradation
 *   - VT-switch pause/resume (master drop/retake) and CRTC restore
 *
 * Nothing here requires a seat — pass NULL and the card is opened
 * directly (root or drm-group user), which keeps the headless tests
 * honest about what they exercise.
 */
#ifndef VANTAGE_KMS_H
#define VANTAGE_KMS_H

#include <stdbool.h>
#include <stdint.h>
#include <vantage/vt-seat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VT_KMS_MAX_OUT 4

typedef struct vt_kms vt_kms_t;

typedef enum {
    VT_KMS_SCANOUT_AUTO = 0,   /* GBM when possible, dumb otherwise */
    VT_KMS_SCANOUT_GBM,
    VT_KMS_SCANOUT_DUMB,
} vt_kms_scanout_t;

typedef enum {
    VT_KMS_OK = 0,
    VT_KMS_NO_CARDS,           /* no /dev/dri entries at all */
    VT_KMS_NO_CONNECTED,       /* cards exist but no connected connector */
    VT_KMS_NO_CRTC,            /* no free CRTC for the connector */
    VT_KMS_NO_MASTER,          /* drmSetMaster refused (session inactive?) */
    VT_KMS_NO_SCANOUT,         /* neither GBM nor dumb buffers worked */
} vt_kms_status_t;

const char *vt_kms_status_str(vt_kms_status_t st);

/* Discovery — fills cards[] (path + driver name), returns how many were
 * found (each card is opened, queried via DRM_IOCTL_VERSION, closed). */
typedef struct {
    char path[32];
    char driver[32];
    bool  has_connected;      /* connector in connected state */
} vt_kms_card_t;
int vt_kms_discover_cards(vt_kms_card_t *cards, int max_cards);

/* Open + mode-set pipeline on one card. `seat` may be NULL (direct open,
 * no session manager). The fd is opened through the seat when given.
 * *status explains failures; NULL return + status tells the story. */
vt_kms_t *vt_kms_open(const char *card_path, vt_seat_t *seat,
                      vt_kms_scanout_t how, vt_kms_status_t *status);
void      vt_kms_close(vt_kms_t *k);

/* informational */
int       vt_kms_output_count(const vt_kms_t *k);
/* connector name ("HDMI-A-1"), width, height, mHz refresh of output i */
const char *vt_kms_out_name(const vt_kms_t *k, int i);
int       vt_kms_out_width(const vt_kms_t *k, int i);
int       vt_kms_out_height(const vt_kms_t *k, int i);
int       vt_kms_out_refresh(const vt_kms_t *k, int i);
const char *vt_kms_renderer(const vt_kms_t *k);   /* honest GPU string */
bool      vt_kms_hw_cursor(const vt_kms_t *k);
const char *vt_kms_scanout_str(const vt_kms_t *k); /* "gbm"|"dumb" */
int       vt_kms_fd(const vt_kms_t *k);

/* First mode set happens here (NOT in _open): call once after buffers
 * are prepared — on success the CRTC scans out the first real frame and
 * `first_scanout` fires (the caller switches the VT to graphics there).
 * Returns 0 on success. Subsequent frames go through vt_kms_present. */
typedef void (*vt_kms_scanout_fn)(vt_kms_t *k, void *ud);
int  vt_kms_start(vt_kms_t *k, vt_kms_scanout_fn first_scanout, void *ud);

/* Present one frame: pixels is a w*h XRGB8888 buffer composited by the
 * caller (software composition); copied into the back scanout buffer and
 * flipped asynchronously. Must be called only after vt_kms_start. */
void vt_kms_present(vt_kms_t *k, const uint32_t *pixels, int w, int h);

/* Copy the first frame into BOTH scanout buffers so the initial mode-set
 * shows real pixels whichever buffer becomes the front. Call before
 * vt_kms_start. */
void vt_kms_prime(vt_kms_t *k, const uint32_t *pixels, int w, int h);

/* Poll the drm fd and feed page-flip events; call when readable. A
 * flip-completion callback may re-arm another frame. */
void vt_kms_handle_events(vt_kms_t *k);

/* hardware cursor plane (when available) */
bool vt_kms_cursor_set(vt_kms_t *k, const uint32_t *argb, int w, int h);
void vt_kms_cursor_move(vt_kms_t *k, int x, int y);
void vt_kms_cursor_hide(vt_kms_t *k);

/* VT-switch lifecycle: drop/retake DRM master; on resume the CRTC is
 * re-armed with the current front buffer and the cursor re-uploaded. */
void vt_kms_pause(vt_kms_t *k);
int  vt_kms_resume(vt_kms_t *k);

#ifdef __cplusplus
}
#endif
#endif
