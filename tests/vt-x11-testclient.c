/*
 * vt-x11-testclient.c — X11 test client for Vantage smoke tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#if defined(VT_HAVE_XFIXES)
#include <X11/extensions/Xfixes.h>
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

int main(int argc, char **argv) {
    int seconds = 4;
    const char *shot = NULL;
    const char *title = "Vantage Test";
    bool ewmh_only = false;
    bool cursor_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--ewmh-probe")) ewmh_only = true;
        else if (!strcmp(argv[i], "--cursor-probe")) cursor_only = true;
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

    Window w1 = make_window(d, title, 100, 100, 400, 300, 0x3a5f9a);
    Window w2 = make_window(d, "Second Window", 500, 300, 300, 220, 0x9a3a5f);
    XMapWindow(d, w1);
    XMapWindow(d, w2);
    XFlush(d);
    printf("mapped 0x%lx 0x%lx\n", (unsigned long)w1, (unsigned long)w2);
    fflush(stdout);   /* harness polls this line while we are alive */

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
