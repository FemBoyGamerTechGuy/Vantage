/*
 * vt-xtest-drive.c — generic XTest input injector
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Drives real pointer input into a running X11 session for testing:
 *   vt-xtest-drive move X Y
 *   vt-xtest-drive click BUTTON X Y          (press+release)
 *   vt-xtest-drive press BUTTON X Y          (press only)
 *   vt-xtest-drive release BUTTON X Y
 *   vt-xtest-drive drag BUTTON X0 Y0 X1 Y1 [MS]
 *   vt-xtest-drive key KEYCODE
 * Prints "done <op>" after each. Used by the harnesses to reproduce
 * user gesture sequences (text selection drags, context-menu right
 * clicks, menu-item clicks) against REAL toolkits.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see source header\n"); return 2; }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    int evb = 0, errb = 0, vmaj = 0, vmin = 0;
    if (!XTestQueryExtension(d, &evb, &errb, &vmaj, &vmin)) {
        fprintf(stderr, "no XTest\n");
        return 1;
    }
    const char *op = argv[1];
    if (!strcmp(op, "move") && argc >= 4) {
        XTestFakeMotionEvent(d, -1, atoi(argv[2]), atoi(argv[3]), CurrentTime);
        XSync(d, False);
    } else if (!strcmp(op, "click") && argc >= 5) {
        int b = atoi(argv[2]);
        XTestFakeMotionEvent(d, -1, atoi(argv[3]), atoi(argv[4]), CurrentTime);
        XSync(d, False);
        usleep(30000);
        XTestFakeButtonEvent(d, b, True, CurrentTime);
        XSync(d, False);
        usleep(60000);
        XTestFakeButtonEvent(d, b, False, CurrentTime);
        XSync(d, False);
    } else if (!strcmp(op, "press") && argc >= 5) {
        int b = atoi(argv[2]);
        XTestFakeMotionEvent(d, -1, atoi(argv[3]), atoi(argv[4]), CurrentTime);
        XSync(d, False);
        usleep(30000);
        XTestFakeButtonEvent(d, b, True, CurrentTime);
        XSync(d, False);
    } else if (!strcmp(op, "release") && argc >= 5) {
        int b = atoi(argv[2]);
        XTestFakeMotionEvent(d, -1, atoi(argv[3]), atoi(argv[4]), CurrentTime);
        XSync(d, False);
        usleep(20000);
        XTestFakeButtonEvent(d, b, False, CurrentTime);
        XSync(d, False);
    } else if (!strcmp(op, "drag") && argc >= 7) {
        int b = atoi(argv[2]);
        int x0 = atoi(argv[3]), y0 = atoi(argv[4]);
        int x1 = atoi(argv[5]), y1 = atoi(argv[6]);
        int ms = argc > 7 ? atoi(argv[7]) : 400;
        XTestFakeMotionEvent(d, -1, x0, y0, CurrentTime);
        XSync(d, False);
        usleep(60000);
        XTestFakeButtonEvent(d, b, True, CurrentTime);
        XSync(d, False);
        usleep(40000);
        int steps = 10;
        for (int i = 1; i <= steps; i++) {
            int x = x0 + (x1 - x0) * i / steps;
            int y = y0 + (y1 - y0) * i / steps;
            XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
            XSync(d, False);
            usleep((useconds_t)(ms * 1000 / steps));
        }
        XTestFakeButtonEvent(d, b, False, CurrentTime);
        XSync(d, False);
    } else if (!strcmp(op, "key") && argc >= 3) {
        XTestFakeKeyEvent(d, atoi(argv[2]), True, CurrentTime);
        XSync(d, False);
        usleep(30000);
        XTestFakeKeyEvent(d, atoi(argv[2]), False, CurrentTime);
        XSync(d, False);
    } else {
        fprintf(stderr, "bad op/args\n");
        return 2;
    }
    printf("done %s\n", op);
    return 0;
}
