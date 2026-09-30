/*
 * vt-glx-probe.c — GLX hardware-rendering probe for the X11 session
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Does exactly what an OpenGL game/application does at startup:
 * XOpenDisplay → glXChooseFBConfig → glXCreateWindow → glXCreateContext
 * (direct!) → make current → draw → glXSwapBuffers. Reports each step
 * so a failure identifies WHERE games die (display, visual, direct
 * context, window mapping, or swap).
 *
 * Usage: vt-glx-probe [frames]      (default 20 frames)
 * Exit 0 = full pipeline OK (a "game" rendered). Prints step lines.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 20;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("glx-probe: XOpenDisplay FAILED\n");
        return 1;
    }
    printf("glx-probe: display %s opened\n", XDisplayName(NULL));

    int attr[] = {
        GLX_RGBA, GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 24, None
    };
    XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr);
    if (!vi) {
        /* modern path: FBConfig */
        int fbattr[] = {
            GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_DOUBLEBUFFER, True,
            GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
            GLX_DEPTH_SIZE, 24, None
        };
        int n = 0;
        GLXFBConfig *fbc = glXChooseFBConfig(dpy, DefaultScreen(dpy),
                                             fbattr, &n);
        if (!fbc || n < 1) {
            printf("glx-probe: no FBConfig/visual — GLX unusable\n");
            return 1;
        }
        vi = glXGetVisualFromFBConfig(dpy, fbc[0]);
        printf("glx-probe: FBConfig path (%d configs)\n", n);
    }
    printf("glx-probe: visual 0x%lx depth=%d\n",
           (unsigned long)vi->visualid, vi->depth);

    /* colormap for this visual (games with non-default visuals need the
     * WM to honor WM_COLORMAP_WINDOWS; creating it here exercises that) */
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, vi->screen),
                                    vi->visual, AllocNone);
    XSetWindowAttributes swa = {
        .colormap = cmap,
        .event_mask = ExposureMask | StructureNotifyMask |
                      KeyPressMask | ButtonPressMask,
    };
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen),
                               100, 100, 400, 300, 0, vi->depth,
                               InputOutput, vi->visual,
                               CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "GLX probe");
    XMapWindow(dpy, win);
    printf("glx-probe: window 0x%lx created+mapped\n", (unsigned long)win);

    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True /* DIRECT */);
    if (!ctx) {
        printf("glx-probe: glXCreateContext FAILED (no direct context)\n");
        return 1;
    }
    printf("glx-probe: context created%s\n",
           glXIsDirect(dpy, ctx) ? " (DIRECT)" : " (INDIRECT!)");

    if (!glXMakeCurrent(dpy, win, ctx)) {
        printf("glx-probe: glXMakeCurrent FAILED\n");
        return 1;
    }
    printf("glx-probe: context current; GL_RENDERER=%s GL_VERSION=%s\n",
           (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    int mapped = 0, reported = 0;
    for (int f = 0; f < frames; f++) {
        /* draw a moving triangle (classic game-like load) */
        float t = (float)f / (float)frames;
        glClearColor(0.05f, 0.05f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.2f, 0.2f);
        glVertex2f(-0.5f + t * 0.2f, -0.4f);
        glColor3f(0.2f, 1.0f, 0.3f);
        glVertex2f(0.5f - t * 0.2f, -0.4f);
        glColor3f(0.3f, 0.4f, 1.0f);
        glVertex2f(0.0f, 0.5f + t * 0.1f);
        glEnd();
        glXSwapBuffers(dpy, win);

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == MapNotify) mapped = 1;
            if (ev.type == ConfigureNotify) reported = 1;
        }
        usleep(30000);
    }
    printf("glx-probe: %d frames swapped; map-notify=%d configure-notify=%d\n",
           frames, mapped, reported);
    if (argc > 2 && !strcmp(argv[2], "--query")) {
        Window root, parent, *kids;
        unsigned nkids;
        if (XQueryTree(dpy, win, &root, &parent, &kids, &nkids)) {
            printf("glx-probe: parent=0x%lx root=0x%lx (framed if parent!=root)\n",
                   (unsigned long)parent, (unsigned long)root);
            if (kids) XFree(kids);
        }
    }
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    printf("glx-probe: OK\n");
    return 0;
}
