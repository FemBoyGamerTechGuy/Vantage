/*
 * vt-seat.c — Session / seat acquisition (libseat preferred, direct VT fallback)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The negotiation is deliberately chatty: every step is logged so a real
 * TTY run pinpoints exactly where session setup stops.
 *
 * libseat path (logind == elogind on the wire — org.freedesktop.login1):
 *   libseat_open_seat() → enable_seat() callback → session devices are
 *   opened through libseat_open_device(); VT switches arrive as
 *   disable/enable callbacks that MUST be acked (libseat_disable_seat()).
 *
 * direct path (no session manager; user booted to a TTY and ran us):
 *   open /dev/ttyN (VANTAGE_VT → XDG_VTNR → active tty → ctty)
 *   VT_SETMODE(VT_PROCESS) with relsig=SIGRTMIN acqsig=SIGRTMIN+1 via
 *   signalfd — SIGUSR1 stays free because the Wayland backend uses it
 *   for the screenshot test hook. drmSetMaster() on the DRM fd.
 *
 * VT ioctls use the real kernel API (VT_GETSTATE + struct vt_stat,
 * VT_ACTIVATE/VT_WAITACTIVE, VT_RELDISP).
 */
#define VT_LOG_DOMAIN "seat"
#include <vantage/vt-seat.h>
#include <vantage/vt-core.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <linux/kd.h>

#if defined(VT_HAVE_LIBSEAT)
#include <libseat.h>
#endif

#if defined(VT_HAVE_LIBDRM)
#include <xf86drm.h>
#endif

#define _SEAT_SIG_REL SIGRTMIN       /* kernel: VT wants to switch away */
#define _SEAT_SIG_ACQ (SIGRTMIN + 1) /* kernel: we became active */

struct vt_seat {
    vt_seat_mode_t   mode;
    char            *name;
    int              vt;
    bool             active;
    bool             graphics;      /* KD mode we last set */
    int              tty_fd;        /* direct mode: VT fd (owned) */
    int              sfd;           /* direct mode: signalfd (owned) */
    int              kd_fd;         /* libseat mode: VT fd opened only for
                                       KDSETMODE ioctls (owned, lazily) */
#if defined(VT_HAVE_LIBSEAT)
    struct libseat  *ls;
#endif
    vt_seat_notify_fn notify;
    void            *notify_ud;
    bool             disabling;     /* inside disable callback */
};

/* ------------------------------------------------------------- helpers */

static int _vt_from_env(void) {
    const char *v = getenv("VANTAGE_VT");
    if (v && *v) {
        int n = atoi(v);
        if (n > 0) return n;
    }
    v = getenv("XDG_VTNR");
    if (v && *v) {
        int n = atoi(v);
        if (n > 0) return n;
    }
    return -1;
}

static int _vt_from_sys_active(void) {
    FILE *f = fopen("/sys/class/tty/tty0/active", "r");
    if (!f) return -1;
    char buf[32] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    if (strncmp(buf, "tty", 3) == 0) {
        int n = atoi(buf + 3);
        if (n > 0) return n;
    }
    return -1;
}

static int _open_vt(int n) {
    if (n <= 0) return -1;
    char path[32];
    snprintf(path, sizeof(path), "/dev/tty%d", n);
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        vt_logd("seat: cannot open %s: %s", path, strerror(errno));
    return fd;
}

static bool _is_vt_fd(int fd) {
    unsigned char kb = 0;
    return ioctl(fd, KDGKBTYPE, &kb) == 0;
}

/* ------------------------------------------------------- direct VT mode */

static void _direct_unblock_signals(struct vt_seat *s) {
    (void)s;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, _SEAT_SIG_REL);
    sigaddset(&mask, _SEAT_SIG_ACQ);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

static int _direct_setup(struct vt_seat *s, int vt_hint) {
    int vt = vt_hint;
    int fd = -1;

    if (vt > 0) {
        fd = _open_vt(vt);
        if (fd < 0)
            vt_logw("seat: VANTAGE_VT/XDG_VTNR says VT %d but it cannot "
                    "be opened: %s", vt, strerror(errno));
    }
    if (fd < 0) {
        vt = _vt_from_sys_active();
        if (vt > 0) {
            fd = _open_vt(vt);
            if (fd >= 0)
                vt_logi("seat: using currently active VT %d "
                        "(/sys/class/tty/tty0/active)", vt);
        }
    }
    if (fd < 0) {
        /* controlling terminal of this process (running from a TTY
         * without any session manager) */
        fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (fd >= 0 && _is_vt_fd(fd)) {
            /* find its number via VT_GETSTATE on tty0 — or fall back to
             * the active one; /dev/tty of a TTY session IS the active VT
             * at login time. */
            struct vt_stat st;
            int tty0 = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
            if (tty0 >= 0 && ioctl(tty0, VT_GETSTATE, &st) == 0)
                vt = st.v_active;
            if (tty0 >= 0) close(tty0);
            if (vt <= 0) vt = _vt_from_sys_active();
            vt_logi("seat: using controlling terminal as VT %d", vt);
        } else {
            if (fd >= 0) { close(fd); fd = -1; }
            vt_loge("seat: no usable VT found — not started from a TTY "
                    "and no session manager is running");
            return -1;
        }
    }
    if (!_is_vt_fd(fd)) {
        vt_loge("seat: fd is not a virtual console (KDGKBTYPE failed)");
        close(fd);
        return -1;
    }

    /* become the VT process owner: receive release/acquire signals */
    struct vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    vm.mode = VT_PROCESS;
    vm.relsig = (short)_SEAT_SIG_REL;
    vm.acqsig = (short)_SEAT_SIG_ACQ;
    vm.waitv = 0;                    /* frsig — unused */
    if (ioctl(fd, VT_SETMODE, &vm) < 0) {
        vt_loge("seat: VT_SETMODE(VT_PROCESS) failed on VT %d: %s "
                "(another compositor/X server owns this VT?)",
                vt, strerror(errno));
        close(fd);
        return -1;
    }

    /* route both signals to a signalfd instead of default handlers */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, _SEAT_SIG_REL);
    sigaddset(&mask, _SEAT_SIG_ACQ);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        vt_loge("seat: sigprocmask: %s", strerror(errno));
        close(fd);
        return -1;
    }
    s->sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (s->sfd < 0) {
        vt_loge("seat: signalfd: %s", strerror(errno));
        _direct_unblock_signals(s);
        close(fd);
        return -1;
    }

    /* are we already active? */
    struct vt_stat st;
    int tty0 = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (tty0 >= 0) {
        if (ioctl(tty0, VT_GETSTATE, &st) == 0)
            s->active = (st.v_active == vt);
        close(tty0);
    }

    s->tty_fd = fd;
    s->vt = vt;
    return 0;
}

static void _direct_dispatch(struct vt_seat *s) {
    for (;;) {
        struct signalfd_siginfo si;
        ssize_t n = read(s->sfd, &si, sizeof(si));
        if (n != (ssize_t)sizeof(si)) break;   /* EAGAIN → done */
        if (si.ssi_signo == (uint32_t)_SEAT_SIG_REL) {
            vt_logi("seat: VT %d release requested — dropping DRM master",
                    s->vt);
            s->active = false;
            if (s->notify)
                s->notify(s, VT_SEAT_NOTIFY_DISABLE, s->notify_ud);
            if (ioctl(s->tty_fd, VT_RELDISP, 1) < 0)
                vt_logw("seat: VT_RELDISP failed: %s", strerror(errno));
        } else if (si.ssi_signo == (uint32_t)_SEAT_SIG_ACQ) {
            vt_logi("seat: VT %d acquired — retaking DRM master", s->vt);
            s->active = true;
            if (ioctl(s->tty_fd, VT_RELDISP, VT_ACKACQ) < 0)
                vt_logd("seat: VT_RELDISP(ACKACQ): %s", strerror(errno));
            if (s->notify)
                s->notify(s, VT_SEAT_NOTIFY_ENABLE, s->notify_ud);
        }
    }
}

static int _direct_activate(struct vt_seat *s, int timeout_ms) {
    if (s->active) return 0;
    if (ioctl(s->tty_fd, VT_ACTIVATE, s->vt) < 0) {
        vt_logw("seat: VT_ACTIVATE(%d): %s", s->vt, strerror(errno));
        return -1;
    }
    int waited = 0;
    while (!s->active && waited < timeout_ms) {
        struct pollfd p = { .fd = s->sfd, .events = POLLIN };
        int rc = poll(&p, 1, 50);
        if (rc < 0 && errno != EINTR) break;
        _direct_dispatch(s);
        waited += 50;
    }
    if (!s->active) {
        vt_logw("seat: VT %d did not become active within %d ms", s->vt,
                timeout_ms);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------- libseat mode */

#if defined(VT_HAVE_LIBSEAT)

static void _ls_enable(struct libseat *ls, void *ud) {
    struct vt_seat *s = ud;
    (void)ls;
    vt_logi("seat: libseat session enabled (seat '%s', VT %d)",
            s->name ? s->name : "?", s->vt);
    s->active = true;
    if (s->notify && !s->disabling)
        s->notify(s, VT_SEAT_NOTIFY_ENABLE, s->notify_ud);
}

static void _ls_disable(struct libseat *ls, void *ud) {
    struct vt_seat *s = ud;
    (void)ls;
    vt_logi("seat: libseat session disabled — dropping devices/master");
    s->active = false;
    s->disabling = true;
    if (s->notify)
        s->notify(s, VT_SEAT_NOTIFY_DISABLE, s->notify_ud);
    s->disabling = false;
    /* MUST ack or the provider revokes devices forcibly */
    if (libseat_disable_seat(ls) < 0)
        vt_loge("seat: libseat_disable_seat failed: %s", strerror(errno));
}

#endif /* VT_HAVE_LIBSEAT */

/* ----------------------------------------------------------------- api */

void vt_seat_set_notify(vt_seat_t *s, vt_seat_notify_fn fn, void *ud) {
    if (!s) return;
    s->notify = fn;
    s->notify_ud = ud;
}

vt_seat_t *vt_seat_acquire(void) {
    const char *force = getenv("VT_SEAT_BACKEND");
    vt_seat_t *s = vt_malloc0(sizeof(*s));
    s->tty_fd = -1;
    s->sfd = -1;
    s->kd_fd = -1;
    s->vt = _vt_from_env();

    vt_logi("seat: acquiring (force=%s, VANTAGE_VT=%d)",
            force ? force : "auto", s->vt);

#if defined(VT_HAVE_LIBSEAT)
    bool want_libseat = true;
    if (force && (vt_strcaseeq(force, "direct")))
        want_libseat = false;
    if (want_libseat) {
        /* libseat honours LIBSEAT_BACKEND itself; map our friendly names
         * (elogind shares logind's bus name and protocol). */
        if (force && *force) {
            if (vt_strcaseeq(force, "elogind"))
                setenv("LIBSEAT_BACKEND", "logind", 1);
            else if (vt_strcaseeq(force, "logind") ||
                     vt_strcaseeq(force, "seatd") ||
                     vt_strcaseeq(force, "builtin"))
                setenv("LIBSEAT_BACKEND", force, 1);
            else
                vt_logw("seat: unknown VT_SEAT_BACKEND '%s' — trying "
                        "libseat auto-probe", force);
        }
        static const struct libseat_seat_listener lst = {
            .enable_seat = _ls_enable,
            .disable_seat = _ls_disable,
        };
        vt_logi("seat: trying libseat (logind → elogind-compatible → "
                "seatd → builtin)");
        s->ls = libseat_open_seat(&lst, s);
        if (s->ls) {
            s->mode = VT_SEAT_MODE_LIBSEAT;
            const char *nm = libseat_seat_name(s->ls);
            s->name = vt_strdup(nm ? nm : "seat0");
            if (s->vt <= 0) s->vt = _vt_from_sys_active();
            vt_logi("seat: libseat opened seat '%s' (VT %d)",
                    s->name, s->vt);
            /* the enable callback fires during dispatch below; some
             * backends need a kick first */
            libseat_dispatch(s->ls, 0);
            return s;
        }
        vt_logi("seat: libseat unavailable (%s) — falling back to direct "
                "VT ioctls", strerror(errno));
    }
#else
    (void)force;
    vt_logi("seat: built without libseat — direct VT ioctls only");
#endif

    if (_direct_setup(s, s->vt) < 0) {
        vt_free(s);
        return NULL;
    }
    s->mode = VT_SEAT_MODE_DIRECT;
    s->name = vt_strdup("seat-direct");
    vt_logi("seat: direct VT session ready (VT %d, fd %d, signalfd %d)",
            s->vt, s->tty_fd, s->sfd);
    return s;
}

void vt_seat_release(vt_seat_t *s) {
    if (!s) return;
    if (s->graphics)
        vt_seat_vt_set_graphics(s, false);
#if defined(VT_HAVE_LIBSEAT)
    if (s->mode == VT_SEAT_MODE_LIBSEAT && s->ls) {
        libseat_close_seat(s->ls);
        s->ls = NULL;
    }
#endif
    if (s->sfd >= 0) close(s->sfd);
    if (s->tty_fd >= 0) {
        struct vt_mode vm;
        memset(&vm, 0, sizeof(vm));
        vm.mode = VT_AUTO;
        ioctl(s->tty_fd, VT_SETMODE, &vm);
        close(s->tty_fd);
    }
    if (s->kd_fd >= 0) close(s->kd_fd);
    vt_free(s->name);
    vt_free(s);
}

vt_seat_mode_t vt_seat_mode(const vt_seat_t *s) {
    return s ? s->mode : VT_SEAT_MODE_NONE;
}
const char *vt_seat_mode_str(const vt_seat_t *s) {
    if (!s) return "none";
    return s->mode == VT_SEAT_MODE_LIBSEAT ? "libseat" :
           s->mode == VT_SEAT_MODE_DIRECT  ? "direct"  : "none";
}
const char *vt_seat_name(const vt_seat_t *s) {
    return s && s->name ? s->name : "seat0";
}
int vt_seat_vt(const vt_seat_t *s) {
    return s ? s->vt : -1;
}
bool vt_seat_is_active(const vt_seat_t *s) {
    return s ? s->active : false;
}

int vt_seat_fd(const vt_seat_t *s) {
    if (!s) return -1;
#if defined(VT_HAVE_LIBSEAT)
    if (s->mode == VT_SEAT_MODE_LIBSEAT && s->ls)
        return libseat_get_fd(s->ls);
#endif
    return s->sfd;
}

void vt_seat_dispatch(vt_seat_t *s) {
    if (!s) return;
#if defined(VT_HAVE_LIBSEAT)
    if (s->mode == VT_SEAT_MODE_LIBSEAT && s->ls) {
        if (libseat_dispatch(s->ls, 0) < 0)
            vt_logw("seat: libseat_dispatch: %s", strerror(errno));
        return;
    }
#endif
    if (s->mode == VT_SEAT_MODE_DIRECT && s->sfd >= 0)
        _direct_dispatch(s);
}

int vt_seat_open_device(vt_seat_t *s, const char *path) {
    if (!s || !path) return -1;
#if defined(VT_HAVE_LIBSEAT)
    if (s->mode == VT_SEAT_MODE_LIBSEAT && s->ls) {
        int fd = -1;
        int id = libseat_open_device(s->ls, path, &fd);
        if (id < 0 || fd < 0) {
            vt_logw("seat: libseat_open_device(%s): %s",
                    path, strerror(errno));
            return -1;
        }
        vt_logi("seat: opened %s through the seat (fd %d, devid %d)",
                path, fd, id);
        return fd;   /* device id is tracked by libseat by fd on close */
    }
#endif
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        vt_logw("seat: open(%s): %s", path, strerror(errno));
    return fd;
}

void vt_seat_close_device(vt_seat_t *s, int fd) {
    if (!s || fd < 0) return;
#if defined(VT_HAVE_LIBSEAT)
    if (s->mode == VT_SEAT_MODE_LIBSEAT && s->ls) {
        /* libseat wants the device id; for logind/seatd backends the
         * mapping fd→id is kept internally per session — the close-by-fd
         * variant is libseat_close_device(seat, device_id). We do not
         * have the id here, so close the fd only; the seat daemon
         * revokes the device at session end anyway. */
        close(fd);
        return;
    }
#endif
    close(fd);
}

int vt_seat_drm_set_master(vt_seat_t *s, int drm_fd) {
    (void)s;
#if defined(VT_HAVE_LIBDRM)
    if (drmSetMaster(drm_fd) == 0)
        return 0;
    /* EACCES with logind usually means the session is not active yet —
     * the enable callback or VT activation will make it succeed. */
    vt_logw("seat: drmSetMaster: %s (%s — master may be granted once the "
            "session/VT becomes active)",
            strerror(errno),
            errno == EACCES ? "EACCES" : "error");
    return -1;
#else
    (void)drm_fd;
    return -1;
#endif
}

int vt_seat_drm_drop_master(vt_seat_t *s, int drm_fd) {
    (void)s;
#if defined(VT_HAVE_LIBDRM)
    if (drmDropMaster(drm_fd) == 0) return 0;
    if (errno == EPERM || errno == EINVAL)
        return 0;    /* not master (already revoked) — fine */
    return -1;
#else
    (void)drm_fd;
    return 0;
#endif
}

int vt_seat_vt_activate(vt_seat_t *s, int timeout_ms) {
    if (!s) return -1;
#if defined(VT_HAVE_LIBSEAT)
    if (s->mode == VT_SEAT_MODE_LIBSEAT && s->ls) {
        if (s->active) return 0;
        /* libseat_switch_session() switches to the session by number;
         * for logind our session is already bound to its VT. */
        if (s->vt > 0 && libseat_switch_session(s->ls, s->vt) == 0) {
            int waited = 0;
            while (!s->active && waited < timeout_ms) {
                struct pollfd p = { .fd = vt_seat_fd(s), .events = POLLIN };
                poll(&p, 1, 50);
                vt_seat_dispatch(s);
                waited += 50;
            }
            return s->active ? 0 : -1;
        }
        vt_logw("seat: libseat_switch_session(%d) failed", s->vt);
        return -1;
    }
#endif
    if (s->mode == VT_SEAT_MODE_DIRECT)
        return _direct_activate(s, timeout_ms);
    return -1;
}

/* KDSETMODE needs a VT fd we own; with libseat open the session VT
 * briefly for the ioctl (permitted for the active session). */
static int _kd_fd_get(struct vt_seat *s) {
    if (s->tty_fd >= 0) return s->tty_fd;          /* direct mode */
    if (s->kd_fd >= 0) return s->kd_fd;
    if (s->vt <= 0) return -1;
    s->kd_fd = _open_vt(s->vt);
    if (s->kd_fd < 0)
        vt_logw("seat: cannot open /dev/tty%d for KDSETMODE: %s "
                "(logind will still switch VTs; text/graphics mode is "
                "then managed by the session manager)",
                s->vt, strerror(errno));
    return s->kd_fd;
}

int vt_seat_vt_set_graphics(vt_seat_t *s, bool graphics) {
    if (!s || s->graphics == graphics) return 0;
    int fd = _kd_fd_get(s);
    if (fd < 0) return -1;
    if (ioctl(fd, KDSETMODE,
              graphics ? KD_GRAPHICS : KD_TEXT) < 0) {
        vt_logw("seat: KDSETMODE(%s): %s",
                graphics ? "graphics" : "text", strerror(errno));
        return -1;
    }
    s->graphics = graphics;
    vt_logi("seat: VT %d switched to %s mode",
            s->vt, graphics ? "KD_GRAPHICS" : "KD_TEXT");
    return 0;
}
