/*
 * vt-x11-testclient.c — X11 test client for Vantage smoke tests
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Maps a few windows with known geometry/colors/titles, runs for a while,
 * optionally dumps a screenshot of the root window to a PPM file:
 *
 *   vt-x11-testclient [--seconds N] [--screenshot out.ppm] [--title NAME]
 *
 *   vt-x11-testclient --ewmh-probe
 *     Reads _NET_SUPPORTING_WM_CHECK from the root window and _NET_WM_NAME
 *     from the WM-check window; prints "ewmh-wm-name=<name>" and exits 0
 *     only when a conforming EWMH WM is running.
 *
 *   vt-x11-testclient --cursor-probe
 *     Verifies a VISIBLE cursor: XQueryPointer for position, then
 *     XFixesGetCursorImage at the pointer position; counts opaque
 *     pixels. Prints cursor-pos=X,Y cursor-size=WxH cursor-opaque=N and
 *     exits 0 only when N > 0 (an invisible/empty cursor fails).
 *
 *   vt-x11-testclient --frame-probe
 *     Verifies server-side decorations: maps a window, waits for the WM
 *     to manage it, then reports _NET_FRAME_EXTENTS and the reparenting
 *     parent. Prints frame-extents=l,r,t,b frame-parent=0x.. and exits 0
 *     only when the client was reparented into a frame with non-zero
 *     top extent (a real title bar).
 *
 *   vt-x11-testclient --motif-probe
 *     Verifies CSD handling: maps a window with _MOTIF_WM_HINTS
 *     decorations=0 (what GTK/Chromium/Firefox set when they draw their
 *     own headerbars) and asserts the WM leaves it UNDECORATED (no
 *     double titlebar). Prints motif-undecorated=yes on success.
 *
 *   vt-x11-testclient --focus-probe
 *     Verifies the focus policy: focuses window A, HOVERS window B
 *     (pointer warp, no click) and asserts the active window is still A
 *     (hover must not steal keyboard focus), then CLICKS window B via
 *     XTest and asserts focus moved. Prints hover-steals=no click-focus=yes
 *     on success.
 *
 *   vt-x11-testclient --drag-flow-probe
 *     Continuous geometry feed during a title-bar drag: maps a framed
 *     window, announces the title-band grab point ("grab X Y"), sleeps
 *     1 s so the harness can attach its IPC event subscriber, then
 *     XTest-drags the window for ~2 s at ~80 motions/s and prints
 *     "motions=N done". The harness's subscriber counts window-geometry
 *     events (the panel pager's live feed, throttled to ~30 fps).
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#if defined(VT_HAVE_XFIXES)
#include <X11/extensions/Xfixes.h>
#endif
#if defined(VT_HAVE_XTST)
#include <X11/extensions/XTest.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void msleep(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* EWMH identity probe: root._NET_SUPPORTING_WM_CHECK → win, then
 * win._NET_WM_NAME (UTF-8). Uses AnyPropertyType so it tolerates WMs
 * that write the check property as CARDINAL instead of WINDOW. */
static char *ewmh_wm_name(Display *d) {
    Atom check = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
    Atom wmname = XInternAtom(d, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *data = NULL;
    int s = DefaultScreen(d);
    Window root = RootWindow(d, s);

    if (XGetWindowProperty(d, root, check, 0, 1, False, AnyPropertyType,
                           &actual, &fmt, &n, &bytes, &data) != Success)
        return NULL;
    if (!data || n < 1) { if (data) XFree(data); return NULL; }
    Window wmwin = *(Window *)(void *)data;
    XFree(data); data = NULL;

    if (XGetWindowProperty(d, wmwin, wmname, 0, 64, False, utf8,
                           &actual, &fmt, &n, &bytes, &data) != Success)
        return NULL;
    if (!data || n < 1) { if (data) XFree(data); return NULL; }
    char *name = strndup((const char *)data, n);
    XFree(data);
    return name;
}

static Window make_window(Display *d, const char *title, int x, int y,
                          int w, int h, unsigned long color) {
    int s = DefaultScreen(d);
    Window win = XCreateSimpleWindow(d, RootWindow(d, s), x, y, w, h, 1,
                                     BlackPixel(d, s), color);
    XStoreName(d, win, title);
    XSizeHints sh = {0};
    sh.flags = PPosition | PSize | PMinSize;
    sh.min_width = 80; sh.min_height = 60;
    XSetWMNormalHints(d, win, &sh);
    Atom protocols = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, win, &protocols, 1);
    XWMHints hints = {0};
    hints.flags = InputHint;
    hints.input = True;
    XSetWMHints(d, win, &hints);
    XSelectInput(d, win, ExposureMask | StructureNotifyMask);
    return win;
}

/* read the EWMH _NET_ACTIVE_WINDOW property from the root window */
static Window active_window_of(Display *d, Window root, Atom net_active) {
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *data = NULL;
    Window active = None;
    if (XGetWindowProperty(d, root, net_active, 0, 1, False,
                           AnyPropertyType, &actual, &fmt, &n,
                           &bytes,
                           &data) == Success && data && n >= 1) {
        active = *(Window *)(void *)data;
        XFree(data);
    }
    return active;
}

int main(int argc, char **argv) {
    int seconds = 4;
    const char *shot = NULL;
    const char *title = "Vantage Test";
    const char *click = NULL;         /* "x,y" before the screenshot */
    const char *clicks = NULL;        /* "x,y;x,y;…" multi-step UI driving */
    bool no_windows = false;          /* pure input driver: map nothing */
    bool ewmh_only = false;
    bool cursor_only = false;
    bool frame_only = false;
    bool motif_only = false;
    bool focus_only = false;
    bool narrow_only = false;   /* the ellipsis-loop regression probe:
                                 * a NARROW window with a LONG title —
                                 * used to wedge the WM forever in the
                                 * frame-paint truncation loop */
    bool nofreeze_only = false; /* the pointer-freeze regression probe:
                                 * plain clicks + Alt+click on a fullscreen
                                 * window must never freeze the pointer */
    bool dragflow_only = false; /* the pager live-feed probe: XTest-drags
                                 * the title bar while the harness counts
                                 * throttled window-geometry events */
    bool late_resize = false;   /* GTK-style startup: create the window
                                 * 1x1 at (0,0), MAP it, then resize to the
                                 * real size afterwards (apps that size
                                 * themselves after mapping — the reported
                                 * "XWayland apps open tiny in the corner") */
    bool race_probe = false;    /* leader + two racing windows mapped in
                                 * one burst: Xwayland creates their
                                 * wl_surfaces back-to-back, and the WM's
                                 * surface↔window association must NOT
                                 * swap them (the serial-based match) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) click = argv[++i];
        else if (!strcmp(argv[i], "--clicks") && i + 1 < argc) clicks = argv[++i];
        else if (!strcmp(argv[i], "--no-windows")) no_windows = true;
        else if (!strcmp(argv[i], "--ewmh-probe")) ewmh_only = true;
        else if (!strcmp(argv[i], "--cursor-probe")) cursor_only = true;
        else if (!strcmp(argv[i], "--frame-probe")) frame_only = true;
        else if (!strcmp(argv[i], "--focus-probe")) focus_only = true;
        else if (!strcmp(argv[i], "--motif-probe")) motif_only = true;
        else if (!strcmp(argv[i], "--narrow-probe")) narrow_only = true;
        else if (!strcmp(argv[i], "--nofreeze-probe")) nofreeze_only = true;
        else if (!strcmp(argv[i], "--drag-flow-probe")) dragflow_only = true;
        else if (!strcmp(argv[i], "--late-resize")) late_resize = true;
        else if (!strcmp(argv[i], "--race-probe")) race_probe = true;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }
    printf("connected %s\n", DisplayString(d));
    fflush(stdout);

    if (cursor_only) {
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        Window r_ret, c_ret;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (!XQueryPointer(d, root, &r_ret, &c_ret, &rx, &ry,
                           &wx, &wy, &mask)) {
            printf("cursor-pos=offscreen\n");
            XCloseDisplay(d);
            return 1;
        }
        printf("cursor-pos=%d,%d\n", rx, ry);
#if defined(VT_HAVE_XFIXES)
        int ev_base = 0, err_base = 0;
        if (!XFixesQueryExtension(d, &ev_base, &err_base)) {
            printf("cursor-image=no-xfixes\n");
            printf("cursor-opaque=-1\n");
            XCloseDisplay(d);
            return 1;
        }
        XFixesCursorImage *ci = XFixesGetCursorImage(d);
        if (!ci) {
            printf("cursor-image=none\n");
            printf("cursor-opaque=0\n");
            XCloseDisplay(d);
            return 1;
        }
        long opaque = 0;
        for (unsigned short y = 0; y < ci->height; y++) {
            for (unsigned short x = 0; x < ci->width; x++) {
                unsigned long px = (unsigned long)
                    ci->pixels[(size_t)y * ci->width + x];
                if ((px & 0xff000000UL) != 0) opaque++;
            }
        }
        printf("cursor-size=%hux%hu\n", ci->width, ci->height);
        printf("cursor-opaque=%ld\n", opaque);
        fflush(stdout);
        XFree(ci);
        XCloseDisplay(d);
        return opaque > 0 ? 0 : 1;
#else
        printf("cursor-image=no-xfixes-build\n");
        printf("cursor-opaque=-1\n");
        XCloseDisplay(d);
        return 1;
#endif
    }

    if (ewmh_only) {
        char *name = ewmh_wm_name(d);
        if (!name) {
            printf("ewmh-wm-name=(none)\n");
            XCloseDisplay(d);
            return 1;
        }
        printf("ewmh-wm-name=%s\n", name);
        int rc = strcmp(name, "Vantage") == 0 ? 0 : 2;
        free(name);
        XCloseDisplay(d);
        return rc;
    }

    if (frame_only) {
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        Window w = make_window(d, "Frame Probe", 120, 120, 300, 200,
                               0x3a5f9a);
        XMapWindow(d, w);
        XFlush(d);
        /* wait for the WM to manage it */
        Window parent = None;
        for (int t = 0; t < 50; t++) {
            Window r, *kids = NULL;
            unsigned int nk = 0;
            if (XQueryTree(d, w, &r, &parent, &kids, &nk)) {
                if (kids) XFree(kids);
                if (parent != root && parent != None) break;
            }
            msleep(50);
        }
        Atom fe = XInternAtom(d, "_NET_FRAME_EXTENTS", False);
        Atom actual;
        int fmt;
        unsigned long n, bytes;
        unsigned char *data = NULL;
        long l = 0, r_ = 0, t = 0, b = 0;
        if (XGetWindowProperty(d, w, fe, 0, 4, False, XA_CARDINAL, &actual,
                               &fmt, &n, &bytes, &data) == Success && data
                               && n >= 4) {
            long *v = (long *)(void *)data;
            l = v[0]; r_ = v[1]; t = v[2]; b = v[3];
            XFree(data);
        }
        printf("frame-parent=0x%lx frame-extents=%ld,%ld,%ld,%ld\n",
               (unsigned long)parent, l, r_, t, b);
        fflush(stdout);
        bool ok = parent != root && parent != None && t > 0;
        printf(ok ? "framed=yes\n" : "framed=no\n");
        fflush(stdout);
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        return ok ? 0 : 1;
    }

    if (motif_only) {
        /* CSD probe: set _MOTIF_WM_HINTS decorations=0 (what GTK and
         * Chromium/Firefox do when they draw their own headerbars) and
         * verify the WM does NOT double-decorate the window. */
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        Window w = make_window(d, "CSD Probe", 120, 120, 300, 200,
                               0x5f9a3a);
        /* _MOTIF_WM_HINTS: flags, functions, decorations, input, status */
        unsigned long hints[5] = { 1L << 2 /* MWM_HINTS_DECORATIONS */,
                                   0, 0, 0, 0 };
        Atom motif = XInternAtom(d, "_MOTIF_WM_HINTS", False);
        XChangeProperty(d, w, motif, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)hints, 5);
        XMapWindow(d, w);
        XFlush(d);
        Window parent = None;
        for (int t = 0; t < 50; t++) {
            Window r, *kids = NULL;
            unsigned int nk = 0;
            if (XQueryTree(d, w, &r, &parent, &kids, &nk)) {
                if (kids) XFree(kids);
                if (parent != root && parent != None) break;
            }
            msleep(50);
        }
        Atom fe = XInternAtom(d, "_NET_FRAME_EXTENTS", False);
        Atom actual;
        int fmt;
        unsigned long n, bytes;
        unsigned char *data = NULL;
        long l = 0, r_ = 0, t = 0, b = 0;
        if (XGetWindowProperty(d, w, fe, 0, 4, False, XA_CARDINAL, &actual,
                               &fmt, &n, &bytes, &data) == Success && data
                               && n >= 4) {
            long *v = (long *)(void *)data;
            l = v[0]; r_ = v[1]; t = v[2]; b = v[3];
            XFree(data);
        }
        printf("motif-parent=0x%lx motif-extents=%ld,%ld,%ld,%ld\n",
               (unsigned long)parent, l, r_, t, b);
        fflush(stdout);
        /* NOT reparented (still a root child) and zero extents */
        bool ok = (parent == root || parent == None) &&
                  l == 0 && r_ == 0 && t == 0 && b == 0;
        printf(ok ? "motif-undecorated=yes\n" : "motif-undecorated=no\n");
        fflush(stdout);
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        return ok ? 0 : 1;
    }

    if (focus_only) {
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        Window a = make_window(d, "Focus A", 100, 100, 350, 250, 0x3a5f9a);
        Window b = make_window(d, "Focus B", 500, 260, 300, 220, 0x9a3a5f);
        XMapWindow(d, a);
        XMapWindow(d, b);
        XFlush(d);
        msleep(600);      /* let the WM manage both */

        /* focus A explicitly (EWMH _NET_ACTIVE_WINDOW root form: the
         * target window travels in data.l[2]) */
        Atom net_active = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
        XEvent msg = { .type = ClientMessage };
        msg.xclient.window = root;
        msg.xclient.message_type = net_active;
        msg.xclient.format = 32;
        msg.xclient.data.l[0] = 1;              /* source: application */
        msg.xclient.data.l[1] = CurrentTime;
        msg.xclient.data.l[2] = (long)a;        /* window to activate   */
        XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask,
                   &msg);
        XFlush(d);
        msleep(400);

        Window act_a = active_window_of(d, root, net_active);

        /* HOVER window B: warp the pointer there WITHOUT clicking */
        XWarpPointer(d, None, b, 0, 0, 0, 0, 40, 40);
        XFlush(d);
        msleep(500);
        Window act_hover = active_window_of(d, root, net_active);
        bool hover_ok = (act_hover == act_a);
        printf("active-a=0x%lx active-hover=0x%lx hover-steals=%s\n",
               (unsigned long)act_a, (unsigned long)act_hover,
               hover_ok ? "no" : "YES");

        /* CLICK window B via XTest */
        bool click_ok = false;
#if defined(VT_HAVE_XTST)
        int evb, errb, vmaj, vmin;
        if (XTestQueryExtension(d, &evb, &errb, &vmaj, &vmin)) {
            XTestFakeButtonEvent(d, 1, True, CurrentTime);
            XTestFakeButtonEvent(d, 1, False, CurrentTime);
            XFlush(d);
            msleep(400);
            Window act_click = active_window_of(d, root, net_active);
            click_ok = (act_click == b);
        }
#endif
        printf("click-focus=%s\n", click_ok ? "yes" : "no");
        fflush(stdout);
        XDestroyWindow(d, a);
        XDestroyWindow(d, b);
        XCloseDisplay(d);
        return (hover_ok && click_ok) ? 0 : 1;
    }

    if (nofreeze_only) {
        /* REGRESSION PROBE — "opening any app locks the whole DE".
         *
         * A GrabModeSync passive grab on plain Button1 once stole the
         * very first click into every framed window and froze the
         * entire pointer event stream until the WM called XAllowEvents
         * — which the plain-click focus path never did. One click into
         * a fresh app = no mouse anywhere, ever again.
         *
         * This probe maps a normal window, clicks it TWICE through
         * XTest, moves the pointer, then Alt+clicks a FULLSCREEN
         * window (the other path where a passive grab fires but no
         * interactive op follows) and verifies the event stream is
         * still alive afterwards. Healthy = exit 0. */
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        Window w = XCreateSimpleWindow(d, root, 200, 200, 400, 300,
                                       0, 0, 0x303440);
        XStoreName(d, w, "NoFreeze probe");
        XSelectInput(d, w, StructureNotifyMask | ButtonPressMask |
                          PointerMotionMask);
        XMapWindow(d, w);
        /* wait for management (reparent) */
        for (int i = 0; i < 60; i++) {
            Window r_ret, p_ret, *kids = NULL; unsigned int nk = 0;
            XQueryTree(d, w, &r_ret, &p_ret, &kids, &nk);
            if (kids) XFree(kids);
            if (p_ret != root) break;
            msleep(50);
        }
        msleep(300);
        /* drain management events */
        XSync(d, False);
        while (XPending(d)) { XEvent ev; XNextEvent(d, &ev); }

        int delivered = 0, motion = 0;
#if defined(VT_HAVE_XTST)
        int evb, errb, vmaj, vmin;
        if (XTestQueryExtension(d, &evb, &errb, &vmaj, &vmin)) {
            Window child;
            int ax, ay;
            XTranslateCoordinates(d, w, root, 60, 60, &ax, &ay, &child);
            XTestFakeMotionEvent(d, -1, ax, ay, CurrentTime);
            XFlush(d); msleep(80);
            XTestFakeButtonEvent(d, 1, True, CurrentTime);
            XFlush(d); msleep(50);
            XTestFakeButtonEvent(d, 1, False, CurrentTime);
            XFlush(d); msleep(150);
            XTranslateCoordinates(d, w, root, 300, 200, &ax, &ay, &child);
            XTestFakeMotionEvent(d, -1, ax, ay, CurrentTime);
            XFlush(d); msleep(80);
            XTestFakeButtonEvent(d, 1, True, CurrentTime);
            XFlush(d); msleep(50);
            XTestFakeButtonEvent(d, 1, False, CurrentTime);
            XFlush(d); msleep(200);
            XSync(d, False);
            while (XPending(d)) {
                XEvent ev; XNextEvent(d, &ev);
                if (ev.type == ButtonPress && ev.xbutton.window == w) delivered++;
                if (ev.type == MotionNotify) motion++;
            }
            printf("plain-clicks-delivered=%d motion-events=%d\n",
                   delivered, motion);

            /* PATH 2: Alt+click on a FULLSCREEN window — the passive
             * grab fires but the WM refuses the interactive op; it
             * must still thaw the pointer or everything freezes. */
            XSelectInput(d, w, StructureNotifyMask | PropertyChangeMask);
            Atom net_state = XInternAtom(d, "_NET_WM_STATE", False);
            Atom net_fs = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
            XEvent msg = { .type = ClientMessage };
            msg.xclient.window = w;
            msg.xclient.message_type = net_state;
            msg.xclient.format = 32;
            msg.xclient.data.l[0] = 1;      /* _NET_WM_STATE_ADD */
            msg.xclient.data.l[1] = (long)net_fs;
            msg.xclient.data.l[2] = 0;
            XSendEvent(d, root, False,
                       SubstructureRedirectMask | SubstructureNotifyMask, &msg);
            XFlush(d);
            bool fs_ok = false;
            for (int i = 0; i < 40; i++) {
                msleep(50);
                Atom act_type; int fmt; unsigned long n, left;
                unsigned char *data = NULL;
                if (XGetWindowProperty(d, w, net_state, 0, 64, False,
                                       XA_ATOM, &act_type, &fmt, &n, &left,
                                       &data) == Success && data) {
                    Atom *states = (Atom *)data;
                    for (unsigned long k = 0; k < n; k++)
                        if (states[k] == net_fs) fs_ok = true;
                    XFree(data);
                }
                if (fs_ok) break;
            }
            printf("fullscreen-granted=%s\n", fs_ok ? "yes" : "no");
            msleep(200);

            /* Alt+click into the fullscreen window */
            XSelectInput(d, w, ButtonPressMask | PointerMotionMask);
            XSync(d, False);
            while (XPending(d)) { XEvent ev; XNextEvent(d, &ev); }
            KeyCode alt = XKeysymToKeycode(d, XK_Alt_L);
            XTranslateCoordinates(d, w, root, 200, 150, &ax, &ay, &child);
            XTestFakeMotionEvent(d, -1, ax, ay, CurrentTime);
            XFlush(d); msleep(80);
            XTestFakeKeyEvent(d, alt, True, CurrentTime);
            XFlush(d); msleep(60);
            XTestFakeButtonEvent(d, 1, True, CurrentTime);
            XFlush(d); msleep(80);
            XTestFakeButtonEvent(d, 1, False, CurrentTime);
            XFlush(d); msleep(60);
            XTestFakeKeyEvent(d, alt, False, CurrentTime);
            XFlush(d); msleep(200);

            /* is the pointer event stream still alive? probe with
             * motion into the window */
            int alive = 0;
            for (int i = 0; i < 6; i++) {
                XTestFakeMotionEvent(d, -1, ax + i * 9, ay + i * 6, CurrentTime);
                XFlush(d); msleep(50);
            }
            XSync(d, False);
            while (XPending(d)) {
                XEvent ev; XNextEvent(d, &ev);
                if (ev.type == MotionNotify) alive++;
            }
            printf("post-altclick-motion=%d\n", alive);
            bool ok = (delivered >= 2) && (alive >= 1);
            printf("verdict=%s\n", ok ? "healthy" : "FROZEN");
            fflush(stdout);
            XDestroyWindow(d, w);
            XCloseDisplay(d);
            return ok ? 0 : 1;
        }
#endif
        printf("verdict=SKIP (no XTest)\n");
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        return 0;
    }

    if (dragflow_only) {
        /* PAGER LIVE-FEED PROBE — "the pager/preview updates too slowly
         * while moving a window". Drags the title bar through REAL
         * XTest events (the same user path as a mouse drag) for ~2 s at
         * ~80 motions/s while the harness's IPC subscriber counts
         * window-geometry broadcasts — throttled to ~30 fps by the WM
         * (op_last_geo_us / 33 ms), the feed the panel's pager redraws
         * from. Prints "grab X Y" first so the harness can attach its
         * subscriber BEFORE the press, then "motions=N done". */
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        Window w = make_window(d, "Drag Flow Probe", 200, 240, 300, 200,
                               0x9a3a5f);
        XMapWindow(d, w);
        XFlush(d);
        /* wait for the WM to manage it (reparent into a frame) */
        Window parent = None;
        for (int t = 0; t < 50; t++) {
            Window r, *kids = NULL;
            unsigned int nk = 0;
            if (XQueryTree(d, w, &r, &parent, &kids, &nk)) {
                if (kids) XFree(kids);
                if (parent != root && parent != None) break;
            }
            msleep(50);
        }
        if (parent == root || parent == None) {
            printf("framed=no\n");
            fflush(stdout);
            XCloseDisplay(d);
            return 1;
        }
        /* true client origin after WM placement (workarea clamp) */
        int wx = 0, wy = 0;
        Window cret;
        XTranslateCoordinates(d, w, root, 0, 0, &wx, &wy, &cret);
        /* SSD title band: 26 px title + 2 px border ABOVE the client —
         * grab its middle, exactly like the Wayland geo-flow probe */
        int gx = wx + 150, gy = wy - 14;
        printf("grab %d %d\n", gx, gy);
        fflush(stdout);
        msleep(1000);   /* the harness attaches its subscriber now */
#if defined(VT_HAVE_XTST)
        XTestFakeMotionEvent(d, -1, gx, gy, CurrentTime);
        XFlush(d); msleep(80);
        XTestFakeButtonEvent(d, 1, True, CurrentTime);
        XFlush(d); msleep(80);
        int motions = 0;
        for (double t = 0.0; t < 2.0; t += 0.012) {
            int dx = (int)(220 * t / 2.0);
            int dy = (int)(60 * t / 2.0);
            XTestFakeMotionEvent(d, -1, gx + dx, gy + dy, CurrentTime);
            XFlush(d);
            motions++;
            msleep(12);
        }
        XTestFakeButtonEvent(d, 1, False, CurrentTime);
        XFlush(d); msleep(150);
        printf("motions=%d done\n", motions);
        fflush(stdout);
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        return 0;
#else
        printf("motions=0 done (no XTest)\n");
        fflush(stdout);
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        return 0;
#endif
    }

    Window w1 = None, w2 = None;
    if (narrow_only) {
        /* the ellipsis truncation path only engages when the title
         * area is wider than 16px but narrower than the text: window
         * width in [~150, title+130]. The old truncation loop grew
         * its working string by 2 bytes per iteration and NEVER
         * terminated — one such window wedged the whole WM. */
        static const int widths[] = { 152, 176, 208 };
        for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
            Window wn = make_window(d,
                "Narrow Title Probe 0123456789 abcdefghijklmnop",
                60 + (int)i * 240, 200, widths[i], 120, 0x5f3a9a);
            XMapWindow(d, wn);
            XFlush(d);
            printf("mapped 0x%lx (%d px)\n", (unsigned long)wn, widths[i]);
            fflush(stdout);
        }
    } else if (race_probe) {
        /* SURFACE-ASSOCIATION RACE: real apps (browsers, Steam, Java)
         * create a group leader + several windows and map them in one
         * burst; Xwayland then creates their wl_surfaces back-to-back.
         * The WM's window↔surface association must land on the RIGHT
         * window — a swap renders the big window at the helper's size
         * ("app effectively invisible") and the model shows the wrong
         * geometry. Windows:
         *   leader: 1x1 at (0,0), NEVER mapped (the classic group
         *           leader — stays "waiting" in the WM forever)
         *   RaceA:  500x400 at (100,100), color 0x3a5f9a
         *   RaceB:  300x200 at (620,150), color 0x9a3a5f
         * Both mapped in one XFlush burst. */
        Window leader = make_window(d, "RaceLeader", 0, 0, 1, 1, 0);
        w1 = make_window(d, "RaceA", 100, 100, 500, 400, 0x3a5f9a);
        w2 = make_window(d, "RaceB", 620, 150, 300, 200, 0x9a3a5f);
        (void)leader;
        XMapWindow(d, w1);
        XMapWindow(d, w2);
        XFlush(d);
        printf("mapped 0x%lx 0x%lx (+leader 0x%lx)\n",
               (unsigned long)w1, (unsigned long)w2, (unsigned long)leader);
        fflush(stdout);
    } else if (late_resize) {
        /* REAL-toolkit startup: the window is created 1x1 at (0,0) and
         * mapped BEFORE the app knows its real size; the resize comes
         * afterwards as a ConfigureRequest (SubstructureRedirect sends
         * it to the WM). The WM must apply it: the final model has to
         * be the REAL size at a sane position, and the surface must
         * paint the real content. */
        w1 = make_window(d, title, 0, 0, 1, 1, 0x3a5f9a);
        XMapWindow(d, w1);
        XFlush(d);
        printf("mapped-1x1 0x%lx\n", (unsigned long)w1);
        fflush(stdout);
        msleep(400);   /* map + first 1x1 draw + WM manage settle */
        XResizeWindow(d, w1, 480, 360);
        XFlush(d);
        printf("resized-to 480x360\n");
        fflush(stdout);
    } else if (!no_windows) {
        w1 = make_window(d, title, 100, 100, 400, 300, 0x3a5f9a);
        w2 = make_window(d, "Second Window", 500, 300, 300, 220, 0x9a3a5f);
        XMapWindow(d, w1);
        XMapWindow(d, w2);
        XFlush(d);
        printf("mapped 0x%lx 0x%lx\n", (unsigned long)w1, (unsigned long)w2);
        fflush(stdout);   /* harness polls this line while we are alive */
    }

    for (int t = 0; t < seconds * 10; t++) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == ClientMessage) {
                /* Only a REAL WM_DELETE_WINDOW closes the window. A WM
                 * that follows ICCCM 4.1.7 also sends WM_TAKE_FOCUS
                 * ClientMessages — treating those as delete made this
                 * client destroy its own windows the moment the WM
                 * focused them (the Xwayland windows vanished from the
                 * taskbar seconds after mapping). */
                Atom wm_protocols = XInternAtom(d, "WM_PROTOCOLS", False);
                Atom wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
                if (ev.xclient.message_type == wm_protocols &&
                    ev.xclient.format == 32 &&
                    (Atom)ev.xclient.data.l[0] == wm_delete) {
                    printf("delete received on 0x%lx — destroying window\n",
                           (unsigned long)ev.xclient.window);
                    fflush(stdout);
                    XDestroyWindow(d, ev.xclient.window);
                }
            }
        }
        msleep(100);
        XFlush(d);
    }

    if (click) {
        /* XTest click at root coordinates (panel-button probing for
         * the integration harness — exercises the REAL input path
         * through the X server, the WM and the panel) */
        int cx = 0, cy = 0;
        sscanf(click, "%d,%d", &cx, &cy);
        int evb = 0, errb = 0, vmaj = 0, vmin = 0;
        if (XTestQueryExtension(d, &evb, &errb, &vmaj, &vmin)) {
            XTestFakeMotionEvent(d, -1, cx, cy, CurrentTime);
            XSync(d, False);
            XTestFakeButtonEvent(d, 1, True, CurrentTime);
            XTestFakeButtonEvent(d, 1, False, CurrentTime);
            XSync(d, False);
            printf("clicked %d,%d\n", cx, cy);
            fflush(stdout);
            /* the panel reacts asynchronously (separate process):
             * give it time to open the menu before the screenshot */
            msleep(500);
        }
    }

    if (clicks) {
        /* multiple XTest clicks "x,y;x,y;…" — drives multi-step UI
         * (menu → category row → application row) through the REAL
         * input path. Usually combined with --no-windows so this
         * client maps nothing of its own and disturbs no focus. */
        int evb = 0, errb = 0, vmaj = 0, vmin = 0;
        if (XTestQueryExtension(d, &evb, &errb, &vmaj, &vmin)) {
            char *dup = strdup(clicks);
            char *save = NULL;
            for (char *tok = strtok_r(dup, ";", &save); tok;
                 tok = strtok_r(NULL, ";", &save)) {
                int cx = 0, cy = 0;
                if (sscanf(tok, "%d,%d", &cx, &cy) == 2) {
                    XTestFakeMotionEvent(d, -1, cx, cy, CurrentTime);
                    XSync(d, False);
                    XTestFakeButtonEvent(d, 1, True, CurrentTime);
                    XTestFakeButtonEvent(d, 1, False, CurrentTime);
                    XSync(d, False);
                    printf("clicked %d,%d\n", cx, cy);
                    fflush(stdout);
                    msleep(400);   /* the panel reacts between steps */
                }
            }
            free(dup);
        } else {
            fprintf(stderr, "XTest extension unavailable\n");
        }
    }

    if (shot) {
        int s = DefaultScreen(d);
        Window root = RootWindow(d, s);
        XWindowAttributes wa;
        XGetWindowAttributes(d, root, &wa);
        XImage *img = XGetImage(d, root, 0, 0, wa.width, wa.height,
                                AllPlanes, ZPixmap);
        if (img) {
            FILE *f = fopen(shot, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
                for (int y = 0; y < img->height; y++) {
                    for (int x = 0; x < img->width; x++) {
                        unsigned long px = XGetPixel(img, x, y);
                        unsigned char rgb[3] = {
                            (unsigned char)(px >> 16),
                            (unsigned char)(px >> 8),
                            (unsigned char)(px)
                        };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
                printf("screenshot %s (%dx%d)\n", shot, img->width, img->height);
                fflush(stdout);
            }
            XDestroyImage(img);
        } else {
            fprintf(stderr, "XGetImage failed\n");
        }
    }
    XCloseDisplay(d);
    return 0;
}
