/*
 * vt-x11-resize-probe.c — XTest edge/corner drag resize verifier
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Maps a plain (SSD-eligible) toplevel, waits for the WM to frame it,
 * then XTest-drags the frame's RIGHT EDGE and BOTTOM-RIGHT CORNER and
 * verifies the WM actually resized the window (geometry from the
 * client's ConfigureNotify + XGetGeometry). Prints:
 *   framed=yes/no            (frame window exists = reparented)
 *   edge-resize=ok/FAIL      (width changed by the drag delta)
 *   corner-resize=ok/FAIL    (w+h both changed)
 *   geometry WxH             final
 * Exit 0 only if both resizes applied.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

static Window wait_parent(Display *d, Window w, Window root) {
    for (int i = 0; i < 80; i++) {
        Window r, p, *kids = NULL;
        unsigned n = 0;
        if (XQueryTree(d, w, &r, &p, &kids, &n)) {
            if (kids) XFree(kids);
            if (p != root && p != 0) return p;   /* framed */
        }
        usleep(50000);
    }
    return 0;
}

static void drag(Display *d, int x0, int y0, int x1, int y1) {
    XTestFakeMotionEvent(d, -1, x0, y0, CurrentTime);
    XSync(d, False);
    usleep(80000);
    XTestFakeButtonEvent(d, 1, True, CurrentTime);
    XSync(d, False);
    usleep(60000);
    int steps = 12;
    for (int i = 1; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / steps;
        int y = y0 + (y1 - y0) * i / steps;
        XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
        XSync(d, False);
        usleep(25000);
    }
    XTestFakeButtonEvent(d, 1, False, CurrentTime);
    XSync(d, False);
    usleep(150000);
}

static void get_geo(Display *d, Window w, int *x, int *y, int *ww, int *hh) {
    Window r;
    unsigned bw, dep;
    int rx, ry;
    unsigned int width, height;
    XGetGeometry(d, w, &r, &rx, &ry, &width, &height, &bw, &dep);
    XTranslateCoordinates(d, w, r, 0, 0, &rx, &ry, &r);
    /* discard pending configures */
    while (XCheckTypedWindowEvent(d, w, ConfigureNotify, (XEvent *)&(XEvent){0})) {}
    *x = rx; *y = ry; *ww = (int)width; *hh = (int)height;
}

int main(int argc, char **argv) {
    int win_x = 100, win_y = 120, win_w = 400, win_h = 300;
    if (argc > 4) {
        win_x = atoi(argv[1]); win_y = atoi(argv[2]);
        win_w = atoi(argv[3]); win_h = atoi(argv[4]);
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("resize-probe: no display\n"); return 1; }
    Window root = DefaultRootWindow(d);

    /* ---- part 3 FIRST (CSD resize via _NET_WM_MOVERESIZE): the path
     * GTK4/Firefox/Chromium CSD apps use when the user drags their own
     * border/shadow margin. Set _MOTIF_WM_HINTS decorations=0 (property
     * type _MOTIF_WM_HINTS, as GTK writes it), confirm the WM does NOT
     * add a frame, then drive the EWMH interactive resize and verify
     * BOTH axes move. Direction 4 = _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT. */
    {
        Atom motif = XInternAtom(d, "_MOTIF_WM_HINTS", False);
        unsigned long hints[5] = { 2 /* DECORATIONS */, 0, 0, 0, 0 };
        Window csd = XCreateSimpleWindow(d, root, 150, 150, 320, 240,
                                         0, 0, 0xa05030);
        XStoreName(d, csd, "CsdResizeProbe");
        XChangeProperty(d, csd, motif, motif, 32, PropModeReplace,
                        (const unsigned char *)hints, 5);
        XSelectInput(d, csd, StructureNotifyMask | ExposureMask);
        XMapWindow(d, csd);
        XSync(d, False);
        sleep(1);
        Window cf = wait_parent(d, csd, root);
        printf("csd-framed=%s\n", cf ? "yes" : "no");
        fflush(stdout);
        int cx, cy, cw, ch;
        get_geo(d, csd, &cx, &cy, &cw, &ch);
        Atom mr = XInternAtom(d, "_NET_WM_MOVERESIZE", False);
        XEvent ev = { .xclient = {
            .type = ClientMessage, .window = csd, .message_type = mr,
            .format = 32,
            .data = { .l = { cx + cw - 8, cy + ch - 8, 4 /* BOTTOMRIGHT */,
                             1 /* button */, 1 } } } };
        XSendEvent(d, root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XSync(d, False);
        /* drag through the interactive resize the message started */
        drag(d, cx + cw - 8, cy + ch - 8, cx + cw + 90, cy + ch + 70);
        get_geo(d, csd, &cx, &cy, &cw, &ch);
        printf("after-csd-moveresize %dx%d\n", cw, ch);
        bool csd_ok = (cw >= 320 + 70 && ch >= 240 + 50);
        printf("csd-moveresize=%s\n", csd_ok ? "ok" : "FAIL");
        fflush(stdout);
        XDestroyWindow(d, csd);
        if (!csd_ok || cf) {
            XCloseDisplay(d);
            return 1;
        }
    }

    Window win = XCreateSimpleWindow(d, root, win_x, win_y,
                                     (unsigned)win_w, (unsigned)win_h, 0, 0, 0x3050a0);
    XStoreName(d, win, "ResizeProbe");
    XSelectInput(d, win, StructureNotifyMask | ExposureMask);
    XMapWindow(d, win);
    XSync(d, False);
    sleep(1);   /* let the WM frame it */

    Window frame = wait_parent(d, win, root);
    printf("framed=%s\n", frame ? "yes" : "no");
    fflush(stdout);

    int evb = 0, errb = 0, vmaj = 0, vmin = 0;
    if (!XTestQueryExtension(d, &evb, &errb, &vmaj, &vmin)) {
        printf("resize-probe: no XTest\n");
        return 1;
    }

    int x, y, w, h;
    get_geo(d, win, &x, &y, &w, &h);
    printf("initial-geometry %dx%d+%d+%d\n", w, h, x, y);
    fflush(stdout);

    /* frame geometry FROM THE ACTUAL POSITION (the WM may re-place):
     * border 2, title 26 → frame = (x-2, y-28, w+4, h+30) */
    int B = 2, T = 26;
    int fx = x - B, fy = y - T - B;
    int fw = w + 2 * B, fh = h + T + 2 * B;

    /* 1. right edge drag: +120px width */
    int ex = fx + fw - 2, ey = fy + fh / 2;
    drag(d, ex, ey, ex + 120, ey);
    get_geo(d, win, &x, &y, &w, &h);
    printf("after-edge %dx%d\n", w, h);
    bool edge_ok = (w >= 400 + 100);
    printf("edge-resize=%s\n", edge_ok ? "ok" : "FAIL");
    fflush(stdout);

    /* 2. bottom-right corner drag: +80 x +60 — recompute the frame
     * position from the CURRENT geometry (the edge drag moved it) */
    int w0 = w, h0 = h;
    {
        int fx2 = x - B, fy2 = y - T - B;
        int fw2 = w + 2 * B, fh2 = h + T + 2 * B;
        int cx = fx2 + fw2 - 2, cy = fy2 + fh2 - 2;
        drag(d, cx, cy, cx + 80, cy + 60);
    }
    get_geo(d, win, &x, &y, &w, &h);
    printf("after-corner %dx%d\n", w, h);
    bool corner_ok = (w >= w0 + 60 && h >= h0 + 40);
    printf("corner-resize=%s\n", corner_ok ? "ok" : "FAIL");
    fflush(stdout);

    XDestroyWindow(d, win);
    XCloseDisplay(d);
    return (edge_ok && corner_ok) ? 0 : 1;
}
