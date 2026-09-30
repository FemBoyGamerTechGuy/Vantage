/*
 * vt-x11-wintree.c — dump the X window tree with geometry
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * One line per window: "0xID WxH+X+Y [OR ]NAME map=N".
 * Used by harness-xvfb.sh to locate probe windows and popovers
 * without depending on external X tools (xwininfo/xdotool).
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>

static void dump(Display *d, Window w, int depth) {
    Window r, parent, *kids = NULL;
    unsigned int nk = 0;
    if (!XQueryTree(d, w, &r, &parent, &kids, &nk)) return;
    XWindowAttributes wa;
    char name[160] = "";
    XTextProperty tp;
    if (XGetTextProperty(d, w, &tp, XA_WM_NAME) && tp.value && tp.nitems) {
        snprintf(name, sizeof name, "%s", (char *)tp.value);
        XFree(tp.value);
    }
    if (XGetWindowAttributes(d, w, &wa)) {
        printf("%*s0x%lx %ux%u+%d+%d %s%s map=%d\n", depth * 2, "",
               (unsigned long)w, wa.width, wa.height, wa.x, wa.y,
               wa.override_redirect ? "OR " : "", name,
               (int)wa.map_state);
    }
    for (unsigned i = 0; i < nk; i++) dump(d, kids[i], depth + 1);
    if (kids) XFree(kids);
}

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    dump(d, DefaultRootWindow(d), 0);
    return 0;
}
