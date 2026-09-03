/* X11 + GLX + fixed-function GL present path. See glview.h for the plan. */
#include "cpore/glview.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>

struct GlvWindow {
    Display *dpy;
    Window win;
    GLXContext ctx;
    int win_w, win_h;
    int frame_w, frame_h;
    GLuint tex;
    Atom wm_delete;
    double fps;
    struct timespec last;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

GlvWindow *glv_open(const char *title, int frame_w, int frame_h, int scale)
{
    static int visual[] = { GLX_RGBA, GLX_DEPTH_SIZE, 8, GLX_DOUBLEBUFFER, None };
    GlvWindow *g;
    XVisualInfo *vi;
    Colormap cmap;
    XSetWindowAttributes swa;
    int screen;
    int win_w, win_h;

    if (frame_w < 8 || frame_h < 8) return NULL;
    g = (GlvWindow *)calloc(1, sizeof(GlvWindow));
    if (!g) return NULL;
    g->dpy = XOpenDisplay(NULL);
    if (!g->dpy) {
        fprintf(stderr, "glv: no X display (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        free(g);
        return NULL;
    }
    screen = DefaultScreen(g->dpy);
    vi = glXChooseVisual(g->dpy, screen, visual);
    if (!vi) {
        fprintf(stderr, "glv: no GLX RGBA visual\n");
        XCloseDisplay(g->dpy);
        free(g);
        return NULL;
    }
    cmap = XCreateColormap(g->dpy, RootWindow(g->dpy, vi->screen),
                           vi->visual, AllocNone);
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     StructureNotifyMask;
    if (scale < 1) scale = 3;
    win_w = frame_w * scale;
    win_h = frame_h * scale;
    if (win_w > 1900) { win_h = win_h * 1900 / win_w; win_w = 1900; }
    if (win_h > 1000) { win_w = win_w * 1000 / win_h; win_h = 1000; }
    g->win_w = win_w;
    g->win_h = win_h;
    g->frame_w = frame_w;
    g->frame_h = frame_h;
    g->win = XCreateWindow(g->dpy, RootWindow(g->dpy, vi->screen),
                           0, 0, (unsigned)win_w, (unsigned)win_h, 0,
                           vi->depth, InputOutput, vi->visual,
                           CWColormap | CWEventMask, &swa);
    XStoreName(g->dpy, g->win, title ? title : "cpore");
    g->wm_delete = XInternAtom(g->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g->dpy, g->win, &g->wm_delete, 1);
    XMapWindow(g->dpy, g->win);
    g->ctx = glXCreateContext(g->dpy, vi, NULL, GL_TRUE);
    XFree(vi);
    if (!g->ctx) {
        fprintf(stderr, "glv: glXCreateContext failed\n");
        XDestroyWindow(g->dpy, g->win);
        XCloseDisplay(g->dpy);
        free(g);
        return NULL;
    }
    glXMakeCurrent(g->dpy, g->win, g->ctx);

    glGenTextures(1, &g->tex);
    glBindTexture(GL_TEXTURE_2D, g->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);

    clock_gettime(CLOCK_MONOTONIC, &g->last);
    g->fps = 60.0;
    return g;
}

void glv_set_frame(GlvWindow *g, int frame_w, int frame_h)
{
    if (!g || frame_w < 8 || frame_h < 8) return;
    g->frame_w = frame_w;
    g->frame_h = frame_h;
}

void glv_close(GlvWindow *g)
{
    if (!g) return;
    if (g->dpy) {
        glXMakeCurrent(g->dpy, None, NULL);
        if (g->ctx) glXDestroyContext(g->dpy, g->ctx);
        XDestroyWindow(g->dpy, g->win);
        XCloseDisplay(g->dpy);
    }
    free(g);
}

void glv_present(GlvWindow *g, const uint8_t *rgba)
{
    float fx, fy, fw, fh;
    if (!g || !rgba) return;
    glViewport(0, 0, g->win_w, g->win_h);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindTexture(GL_TEXTURE_2D, g->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g->frame_w, g->frame_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    /* aspect-correct letterbox */
    {
        float wa = (float)g->win_w / (float)g->win_h;
        float fa = (float)g->frame_w / (float)g->frame_h;
        if (wa > fa) { fh = 1.0f; fw = fa / wa; }
        else { fw = 1.0f; fh = wa / fa; }
        fx = (1.0f - fw) * 0.5f;
        fy = (1.0f - fh) * 0.5f;
    }
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(fx * 2.0f - 1.0f, fy * 2.0f - 1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f((fx + fw) * 2.0f - 1.0f, fy * 2.0f - 1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f((fx + fw) * 2.0f - 1.0f, (fy + fh) * 2.0f - 1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(fx * 2.0f - 1.0f, (fy + fh) * 2.0f - 1.0f);
    glEnd();
    glXSwapBuffers(g->dpy, g->win);
}

void glv_poll(GlvWindow *g, int *down, int *pressed)
{
    if (!g) return;
    if (pressed) memset(pressed, 0, sizeof(int) * 256);
    while (XPending(g->dpy)) {
        XEvent ev;
        XNextEvent(g->dpy, &ev);
        if (ev.type == ClientMessage &&
            (Atom)ev.xclient.data.l[0] == g->wm_delete) {
            if (down) down[0x1B] = 1;      /* quit reads as Escape held */
            if (pressed) pressed[0x1B] = 1;
        } else if (ev.type == ConfigureNotify) {
            g->win_w = ev.xconfigure.width;
            g->win_h = ev.xconfigure.height;
        } else if (ev.type == KeyPress || ev.type == KeyRelease) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int is_press = (ev.type == KeyPress);
            int k = -1;
            if (ks >= 'a' && ks <= 'z') k = (int)ks;
            else if (ks >= 'A' && ks <= 'Z') k = (int)ks - 'A' + 'a';
            else if (ks >= '0' && ks <= '9') k = (int)ks;
            else if (ks == XK_space) k = 32;
            else if (ks == XK_Escape) k = 0x1B;
            else if (ks == XK_Left) k = 0x51;
            else if (ks == XK_Up) k = 0x52;
            else if (ks == XK_Right) k = 0x53;
            else if (ks == XK_Down) k = 0x54;
            else if (ks == XK_Return) k = 0x0D;
            if (k >= 0 && k < 256 && down) {
                if (is_press && !down[k] && pressed) pressed[k] = 1;
                down[k] = is_press ? 1 : 0;
            }
            if (ks == XK_Escape && is_press) return;  /* checked by caller */
        }
    }
}

double glv_tick(GlvWindow *g, int fps)
{
    double target, now, dt;
    struct timespec rq;
    if (!g || fps < 1) return g ? g->fps : 0.0;
    target = 1.0 / (double)fps;
    now = now_s();
    dt = now - (g->last.tv_sec + g->last.tv_nsec * 1e-9);
    if (dt < target) {
        double sleep_for = target - dt;
        rq.tv_sec = (time_t)sleep_for;
        rq.tv_nsec = (long)((sleep_for - rq.tv_sec) * 1e9);
        nanosleep(&rq, NULL);
        now = now_s();
        dt = now - (g->last.tv_sec + g->last.tv_nsec * 1e-9);
    }
    g->fps = g->fps * 0.95 + (dt > 1e-6 ? 1.0 / dt : 60.0) * 0.05;
    clock_gettime(CLOCK_MONOTONIC, &g->last);
    return g->fps;
}

double glv_fps(const GlvWindow *g) { return g ? g->fps : 0.0; }
