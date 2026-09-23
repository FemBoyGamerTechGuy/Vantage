/*
 * vt-x11-testclient.c — X11 test client for Vantage smoke tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Maps a few windows with known geometry/colors/titles, runs for a while,
 * optionally dumps a screenshot of the root window to a PPM file:
 *
 *   vt-x11-testclient [--seconds N] [--screenshot out.ppm] [--title NAME]
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }
    printf("connected %s\n", DisplayString(d));

    Window w1 = make_window(d, title, 100, 100, 400, 300, 0x3a5f9a);
    Window w2 = make_window(d, "Second Window", 500, 300, 300, 220, 0x9a3a5f);
    XMapWindow(d, w1);
    XMapWindow(d, w2);
    XFlush(d);
    printf("mapped 0x%lx 0x%lx\n", (unsigned long)w1, (unsigned long)w2);

    for (int t = 0; t < seconds * 10; t++) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == ClientMessage) {
                /* WM_DELETE_WINDOW */
                printf("delete received on 0x%lx — ignoring\n",
                       (unsigned long)ev.xclient.window);
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
            }
            XDestroyImage(img);
        } else {
            fprintf(stderr, "XGetImage failed\n");
        }
    }
    XCloseDisplay(d);
    return 0;
}
