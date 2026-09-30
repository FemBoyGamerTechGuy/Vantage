/*
 * vt-xi2-event-probe.c — raw XInput2 event delivery probe
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Maps a window and selects ButtonPress/Release/Motion via XInput2
 * (exactly what GDK4/GTK4, Firefox and Chromium use — NOT the core
 * protocol). Prints EVERY delivered event raw:
 *   press B=X/Y   release B=X/Y   motion X/Y
 * Final line: "events=N press=N release=N rclick=N"
 * Exit 0. If the WM/server eats button-3 or drag sequences, it shows
 * here with zero GTK involvement.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

int main(int argc, char **argv) {
    int wait = argc > 1 ? atoi(argv[1]) : 10;
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("no display\n"); return 1; }
    Window root = DefaultRootWindow(d);

    /* does the server have XI2? */
    int major = 2, minor = 3;
    if (XIQueryVersion(d, &major, &minor) != Success) {
        printf("xi2 unavailable\n");
        return 1;
    }

    int sx = 100, sy = 120, sw = 400, sh = 300;
    if (argc > 5) {
        sx = atoi(argv[2]); sy = atoi(argv[3]);
        sw = atoi(argv[4]); sh = atoi(argv[5]);
    }
    Window win = XCreateSimpleWindow(d, root, sx, sy, (unsigned)sw,
                                     (unsigned)sh, 0, 0, 0x3050a0);
    XStoreName(d, win, "XI2 Event Probe");
    XMapWindow(d, win);

    /* XI2 selection: button press/release/motion for all master
     * devices — the GDK4 event path */
    XIEventMask evm;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask, XI_ButtonPress);
    XISetMask(mask, XI_ButtonRelease);
    XISetMask(mask, XI_Motion);
    evm.deviceid = XIAllMasterDevices;
    evm.mask_len = sizeof(mask);
    evm.mask = mask;
    XISelectEvents(d, win, &evm, 1);
    XSync(d, False);
    /* discard the XI2 extension events... XNextEvent returns them as
     * GenericEvents; XIfEvent filter for our window */

    int n_press = 0, n_release = 0, n_motion = 0, n_rclick = 0, n_total = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (double)(now.tv_sec - t0.tv_sec) +
                    (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= wait) break;
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type != GenericEvent) continue;
            XGenericEvent *ge = (XGenericEvent *)&ev;
            if (ge->extension == -1) continue;
            XGetEventData(d, (XGenericEventCookie *)&ev);
            const XIDeviceEvent *de = (const XIDeviceEvent *)ev.xcookie.data;
            switch (ge->evtype) {
            case XI_ButtonPress:
                n_press++; n_total++;
                printf("press B=%d %.0f/%.0f\n", de->detail, de->event_x,
                       de->event_y);
                if (de->detail == 3) n_rclick++;
                break;
            case XI_ButtonRelease:
                n_release++; n_total++;
                printf("release B=%d %.0f/%.0f\n", de->detail, de->event_x,
                       de->event_y);
                break;
            case XI_Motion:
                n_motion++; n_total++;
                break;
            }
            XFreeEventData(d, (XGenericEventCookie *)&ev);
            fflush(stdout);
        }
        usleep(20000);
    }
    printf("events=%d press=%d release=%d motion=%d rclick=%d\n",
           n_total, n_press, n_release, n_motion, n_rclick);
    return 0;
}
