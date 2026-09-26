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

    Window w1 = None, w2 = None;
    if (!no_windows) {
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
                /* WM_DELETE_WINDOW — honor it like a well-behaved client */
                printf("delete received on 0x%lx — destroying window\n",
                       (unsigned long)ev.xclient.window);
                fflush(stdout);
                XDestroyWindow(d, ev.xclient.window);
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
