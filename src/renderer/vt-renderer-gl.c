/*
 * vt-renderer-gl.c — OpenGL/EGL hardware renderer
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Compiled only when both EGL and OpenGL (or GLESv2) are available.
 * Provides:
 *   - EGL display surface/context creation
 *   - Hardware-accelerated texture upload + blit
 *   - Shaders for solid fill + textured blit
 *
 * The renderer does NOT require Vulkan. It does not assume GBM; the
 * underlying surface is provided by the backend (Wayland's wl_surface,
 * X11's window, or a headless pbuffer).
 */

#define VT_LOG_DOMAIN "renderer-gl"
#include <vantage/vt-renderer.h>

#if (defined(VT_HAVE_OPENGL) || defined(VT_HAVE_GLESV2)) && defined(VT_HAVE_EGL)

#include <EGL/egl.h>
#include <EGL/eglext.h>
#if defined(VT_HAVE_GLESV2)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define VT_GL_API_T GL_APICALL
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    EGLDisplay  dpy;
    EGLConfig   cfg;
    EGLContext  ctx;
    EGLSurface surf;
    int         w, h;
    bool        bound;
    GLuint      prog_solid;
    GLuint      prog_textured;
    GLint      vp_solid;
    GLint      vp_textured;
} _gl_state_t;

/* ---- minimal shaders ---------------------------------------------------- */
static const char *const _vs_solid =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"
    "void main() {\n"
    "  vec2 p = a_pos * 0.5 + 0.5;\n"
    "  vec2 v = u_rect.xy + p * u_rect.zw;\n"
    "  gl_Position = vec4(v * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const _fs_solid =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

static const char *const _vs_textured =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_dst;\n"
    "uniform vec4 u_src;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = a_pos * 0.5 + 0.5;\n"
    "  vec2 v = u_dst.xy + p * u_dst.zw;\n"
    "  gl_Position = vec4(v * 2.0 - 1.0, 0.0, 1.0);\n"
    "  v_uv = u_src.xy + p * u_src.zw;\n"
    "}\n";

static const char *const _fs_textured =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_tint;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, v_uv);\n"
    "  gl_FragColor = c * u_tint;\n"
    "}\n";

static GLuint _compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    GLint len = (GLint)strlen(src);
    glShaderSource(s, 1, &src, &len);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), NULL, log);
        vt_logw("GL: shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static GLuint _program(const char *vs_src, const char *fs_src) {
    GLuint vs = _compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = _compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(p, sizeof(log), NULL, log);
        vt_logw("GL: link error: %s", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static bool _gl_probe(vt_renderer_caps_t *caps) {
    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return false;
    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        eglTerminate(dpy);
        return false;
    }
    /* GL strings require a current context: create a throwaway pbuffer
     * so the probe reports the real renderer (llvmpipe / NVIR / iris /
     * radeonsi ...), GL version and GLSL version instead of "egl"/"?" */
    eglBindAPI(
#if defined(VT_HAVE_GLESV2)
        EGL_OPENGL_ES_API
#else
        EGL_OPENGL_API
#endif
    );
    EGLint const cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE,
#if defined(VT_HAVE_GLESV2)
        EGL_OPENGL_ES2_BIT,
#else
        EGL_OPENGL_BIT,
#endif
        EGL_NONE,
    };
    EGLConfig cfg = NULL;
    EGLint n = 0;
    EGLSurface pbuf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    if (eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) && n >= 1) {
        EGLint const pb_attr[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
        pbuf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
        EGLint const ctx_attr[] = {
#if defined(VT_HAVE_GLESV2)
            EGL_CONTEXT_CLIENT_VERSION, 2,
#endif
            EGL_NONE,
        };
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
        if (pbuf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT)
            eglMakeCurrent(dpy, pbuf, pbuf, ctx);
    }
    if (caps) {
        memset(caps, 0, sizeof(*caps));
        caps->hw_accel = true;
        caps->vsync = true;
        caps->shaders = true;
        caps->max_texture_size = 8192;
        caps->texture_units = 8;
        const char *v = eglQueryString(dpy, EGL_VENDOR);
        const char *r = eglQueryString(dpy, EGL_VERSION);
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        const char *glver = (const char *)glGetString(GL_VERSION);
        const char *glslver = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
        snprintf(caps->vendor, sizeof(caps->vendor), "%s", v ? v : "?");
        snprintf(caps->version, sizeof(caps->version), "%s", glver ? glver : (r ? r : "?"));
        snprintf(caps->renderer, sizeof(caps->renderer), "%s", renderer ? renderer : "egl");
        snprintf(caps->glsl_version, sizeof(caps->glsl_version), "%s", glslver ? glslver : "?");
    }
    if (pbuf != EGL_NO_SURFACE) eglDestroySurface(dpy, pbuf);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return true;
}

static int _gl_init(vt_renderer_t *r) {
    _gl_state_t *s = vt_malloc0(sizeof(*s));
    s->dpy = eglGetDisplay((EGLNativeDisplayType)EGL_DEFAULT_DISPLAY);
    if (s->dpy == EGL_NO_DISPLAY) { vt_free(s); return VT_ERR_NOTSUPP; }
    EGLint major, minor;
    if (!eglInitialize(s->dpy, &major, &minor)) { vt_free(s); return VT_ERR_NOTSUPP; }
    EGLint const cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE,
#if defined(VT_HAVE_GLESV2)
        EGL_OPENGL_ES2_BIT,
#else
        EGL_OPENGL_BIT,
#endif
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(s->dpy, cfg_attr, &s->cfg, 1, &n) || n < 1) {
        eglTerminate(s->dpy);
        vt_free(s);
        return VT_ERR_NOTSUPP;
    }
    eglBindAPI(
#if defined(VT_HAVE_GLESV2)
        EGL_OPENGL_ES_API
#else
        EGL_OPENGL_API
#endif
    );
    EGLint const ctx_attr[] = {
#if defined(VT_HAVE_GLESV2)
        EGL_CONTEXT_CLIENT_VERSION, 2,
#endif
        EGL_NONE,
    };
    s->ctx = eglCreateContext(s->dpy, s->cfg, EGL_NO_CONTEXT, ctx_attr);
    if (s->ctx == EGL_NO_CONTEXT) {
        eglTerminate(s->dpy);
        vt_free(s);
        return VT_ERR_NOTSUPP;
    }
    s->surf = EGL_NO_SURFACE;
    r->priv = s;
    /* Compile shaders */
    eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx);
    s->prog_solid = _program(_vs_solid, _fs_solid);
    s->prog_textured = _program(_vs_textured, _fs_textured);
    return 0;
}

static void _gl_fini(vt_renderer_t *r) {
    if (!r->priv) return;
    _gl_state_t *s = r->priv;
    if (s->prog_solid)    glDeleteProgram(s->prog_solid);
    if (s->prog_textured) glDeleteProgram(s->prog_textured);
    if (s->surf != EGL_NO_SURFACE) eglDestroySurface(s->dpy, s->surf);
    if (s->ctx != EGL_NO_CONTEXT)   eglDestroyContext(s->dpy, s->ctx);
    eglTerminate(s->dpy);
    vt_free(s);
    r->priv = NULL;
}

static void _gl_begin(vt_renderer_t *r, int w, int h) {
    _gl_state_t *s = r->priv;
    s->w = w; s->h = h;
    eglMakeCurrent(s->dpy, s->surf, s->surf, s->ctx);
    glViewport(0, 0, w, h);
}
static void _gl_end(vt_renderer_t *r) { (void)r; }

static void _gl_clear(vt_renderer_t *r, vt_color_t c) {
    _gl_state_t *s = r->priv;
    eglMakeCurrent(s->dpy, s->surf, s->surf, s->ctx);
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
    (void)s;
}

static vt_texture_t *_gl_texture_create(vt_renderer_t *r, uint32_t w, uint32_t h,
                                          vt_pixel_format_t fmt) {
    (void)r;
    vt_texture_t *t = vt_malloc0(sizeof(*t));
    t->w = w; t->h = h; t->fmt = fmt;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
static void _gl_texture_free(vt_renderer_t *r, vt_texture_t *t) {
    if (!t) return;
    if (t->id) glDeleteTextures(1, &t->id);
    if (t->owns_pixels) vt_free(t->pixels);
    vt_free(t);
    (void)r;
}
static void _gl_texture_upload(vt_renderer_t *r, vt_texture_t *t,
                                 const void *pixels) {
    (void)r;
    if (!t || !pixels) return;
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}
static void _gl_texture_draw(vt_renderer_t *r, vt_texture_t *t, vt_rect_t dst,
                              vt_rect_t *src, vt_color_t *tint) {
    _gl_state_t *s = r->priv;
    if (!s || !s->prog_textured) return;
    vt_rect_t sr = src ? *src : (vt_rect_t){0, 0, (int)t->w, (int)t->h};
    glUseProgram(s->prog_textured);
    GLint loc = glGetUniformLocation(s->prog_textured, "u_dst");
    glUniform4f(loc, (float)dst.x, (float)dst.y, (float)dst.w, (float)dst.h);
    loc = glGetUniformLocation(s->prog_textured, "u_src");
    glUniform4f(loc, (float)sr.x, (float)sr.y, (float)sr.w, (float)sr.h);
    loc = glGetUniformLocation(s->prog_textured, "u_tint");
    if (!tint) glUniform4f(loc, 1.0f, 1.0f, 1.0f, 1.0f);
    else glUniform4f(loc, tint->r, tint->g, tint->b, tint->a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->id);
    GLint tu = glGetUniformLocation(s->prog_textured, "u_tex");
    glUniform1i(tu, 0);
    GLfloat verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLint pos = glGetAttribLocation(s->prog_textured, "a_pos");
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray((GLuint)pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)pos);
}

static void _gl_fill_rect(vt_renderer_t *r, vt_rect_t rc, vt_color_t c) {
    _gl_state_t *s = r->priv;
    if (!s || !s->prog_solid) return;
    glUseProgram(s->prog_solid);
    GLint loc = glGetUniformLocation(s->prog_solid, "u_rect");
    glUniform4f(loc, (float)rc.x, (float)rc.y, (float)rc.w, (float)rc.h);
    loc = glGetUniformLocation(s->prog_solid, "u_color");
    glUniform4f(loc, c.r, c.g, c.b, c.a);
    GLfloat verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLint pos = glGetAttribLocation(s->prog_solid, "a_pos");
    glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray((GLuint)pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)pos);
}

static void _gl_present(vt_renderer_t *r) {
    _gl_state_t *s = r->priv;
    if (s->surf != EGL_NO_SURFACE) eglSwapBuffers(s->dpy, s->surf);
}

static const char *_gl_name(void) { return "opengl-egl"; }

const vt_renderer_ops_t vt_renderer_gl_ops = {
    .init            = _gl_init,
    .fini            = _gl_fini,
    .begin           = _gl_begin,
    .end             = _gl_end,
    .clear           = _gl_clear,
    .texture_create  = _gl_texture_create,
    .texture_upload  = _gl_texture_upload,
    .texture_free    = _gl_texture_free,
    .texture_draw    = _gl_texture_draw,
    .fill_rect       = _gl_fill_rect,
    .blit            = NULL,
    .present         = _gl_present,
    .probe           = _gl_probe,
    .name            = _gl_name,
};

#else
/* No EGL/OpenGL available — emit a stub so the renderer file still compiles
 * if Meson chose to build it. The probe returns false. */
#include <vantage/vt-renderer.h>
#include <string.h>
static bool _no_gl_probe(vt_renderer_caps_t *caps) {
    (void)caps;
    return false;
}
static const char *_no_gl_name(void) { return "opengl (unavailable)"; }
static int _no_gl_init(vt_renderer_t *r) { (void)r; return -1; }
const vt_renderer_ops_t vt_renderer_gl_ops = {
    .init = _no_gl_init, .probe = _no_gl_probe, .name = _no_gl_name,
};
#endif
