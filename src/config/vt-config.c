/*
 * vt-config.c — Vantage configuration system
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * INI-like parser supporting:
 *   [section]            -- starts a section
 *   key = value          -- key/value (no quoting semantics, literal =-stripping)
 *   # comment            -- line ignored
 *   ; comment            -- line ignored
 *   \ at end             -- line continuation
 *
 * Live reload via inotify on Linux.
 */

#define VT_LOG_DOMAIN "config"
#include <vantage/vt-config.h>
#include <vantage/vt-paths.h>

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/select.h>

static const char *const _known_names[VT_CFG_NUM_KNOWN] = {
    [VT_CFG_BACKEND]         = "backend",
    [VT_CFG_RENDERER]        = "renderer",
    [VT_CFG_THEME]           = "theme",
    [VT_CFG_DARK_MODE]       = "dark-mode",
    [VT_CFG_WALLPAPER]       = "wallpaper",
    [VT_CFG_VIDEO_WALLPAPER] = "video-wallpaper",
    [VT_CFG_VIDEO_VOLUME]    = "video-volume",
    [VT_CFG_COMPOSITOR]      = "compositor",
    [VT_CFG_ANIMATIONS]      = "animations",
    [VT_CFG_VSYNC]           = "vsync",
    [VT_CFG_SHADOWS]         = "shadows",
    [VT_CFG_BLUR]            = "blur",
    [VT_CFG_SCALE]           = "scale",
    [VT_CFG_WORKSPACE_COUNT] = "workspace-count",
    [VT_CFG_PANEL_HEIGHT]    = "panel-height",
    [VT_CFG_PANEL_POSITION]  = "panel-position",
    [VT_CFG_DBUS]            = "dbus",
    [VT_CFG_LOG_LEVEL]       = "log-level",
};

static const char *const _known_defaults[VT_CFG_NUM_KNOWN] = {
    [VT_CFG_BACKEND]         = "auto",
    [VT_CFG_RENDERER]        = "auto",
    [VT_CFG_THEME]           = "Vantage-Dark",
    [VT_CFG_DARK_MODE]       = "true",
    [VT_CFG_WALLPAPER]       = "",
    [VT_CFG_VIDEO_WALLPAPER] = "",
    [VT_CFG_VIDEO_VOLUME]    = "0",
    [VT_CFG_COMPOSITOR]      = "true",
    [VT_CFG_ANIMATIONS]      = "true",
    [VT_CFG_VSYNC]           = "true",
    [VT_CFG_SHADOWS]         = "true",
    [VT_CFG_BLUR]            = "false",
    [VT_CFG_SCALE]           = "1.0",
    [VT_CFG_WORKSPACE_COUNT] = "4",
    [VT_CFG_PANEL_HEIGHT]    = "32",
    [VT_CFG_PANEL_POSITION]  = "top",
    [VT_CFG_DBUS]            = "auto",
    [VT_CFG_LOG_LEVEL]       = "info",
};

const char *vt_cfg_known_name(vt_cfg_known_t k) {
    return (k < VT_CFG_NUM_KNOWN) ? _known_names[k] : NULL;
}
const char *vt_cfg_known_default(vt_cfg_known_t k) {
    return (k < VT_CFG_NUM_KNOWN) ? _known_defaults[k] : NULL;
}

const char *vt_config_user_dir(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s/vantage", vt_config_dir());
    return buf;
}
const char *vt_config_sysconf_dir(void) { return VT_SYSCONFDIR; }
const char *vt_config_default_path(void) {
    /* user config -> dev-tree default -> sysconfdir (vt-paths.c);
     * result cached for the process lifetime */
    static char *buf = NULL;
    if (!buf) buf = vt_paths_config_default();
    return buf;
}

vt_config_t *vt_config_new(void) {
    vt_config_t *c = vt_malloc0(sizeof(*c));
    vt_vec_init(&c->sections, sizeof(vt_cfg_sec_t), 4);
    return c;
}

void vt_config_clear(vt_config_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->sections.size; i++) {
        vt_cfg_sec_t *s = vt_vec_at(&c->sections, i);
        vt_free(s->name);
        for (size_t j = 0; j < s->keys.size; j++) {
            vt_cfg_key_t *k = vt_vec_at(&s->keys, j);
            vt_free(k->key); vt_free(k->val);
        }
        vt_vec_fini(&s->keys);
    }
    vt_vec_clear(&c->sections);
    c->dirty = false;
}

void vt_config_free(vt_config_t *c) {
    if (!c) return;
    vt_config_unwatch(c);
    vt_config_clear(c);
    vt_vec_fini(&c->sections);
    vt_free(c->path);
    vt_free(c);
}

static vt_cfg_sec_t *_sec_get(vt_config_t *c, const char *name, bool create) {
    for (size_t i = 0; i < c->sections.size; i++) {
        vt_cfg_sec_t *s = vt_vec_at(&c->sections, i);
        if (vt_strcaseeq(s->name ? s->name : "", name ? name : "")) return s;
    }
    if (!create) return NULL;
    vt_cfg_sec_t ns = { .name = vt_strdup(name ? name : "") };
    vt_vec_init(&ns.keys, sizeof(vt_cfg_key_t), 4);
    vt_cfg_sec_t *p = vt_vec_push(&c->sections, &ns);
    return p;
}

static vt_cfg_key_t *_key_get(vt_cfg_sec_t *s, const char *key, bool create) {
    for (size_t i = 0; i < s->keys.size; i++) {
        vt_cfg_key_t *k = vt_vec_at(&s->keys, i);
        if (vt_strcaseeq(k->key, key)) return k;
    }
    if (!create) return NULL;
    vt_cfg_key_t nk = { .key = vt_strdup(key), .val = vt_strdup("") };
    return vt_vec_push(&s->keys, &nk);
}

vt_config_t *vt_config_new_from_file(const char *path) {
    vt_config_t *c = vt_config_new();
    if (!c) return NULL;
    if (vt_config_load(c, path) < 0) {
        vt_config_free(c);
        return NULL;
    }
    return c;
}

vt_config_t *vt_config_new_defaults(void) {
    vt_config_t *c = vt_config_new();
    if (!c) return NULL;
    for (int i = 0; i < VT_CFG_NUM_KNOWN; i++) {
        vt_config_set(c, "desktop",
                       vt_cfg_known_name(i),
                       vt_cfg_known_default(i));
    }
    /* a couple of useful non-known defaults */
    vt_config_set(c, "panel",   "position", "top");
    vt_config_set(c, "panel",   "height",   "32");
    vt_config_set(c, "wm",      "focus-new", "true");
    vt_config_set(c, "wm",      "tile-key",  "Super+Direction");
    vt_config_set(c, "audio",   "backend",  "auto");
    vt_config_set(c, "power",   "backend",  "auto");
    vt_config_set(c, "network", "backend",  "auto");
    return c;
}

int vt_config_load(vt_config_t *c, const char *path) {
    if (!c || !path) return VT_ERR_INVAL;
    size_t len = 0;
    char *buf = vt_file_read_all(path, &len);
    if (!buf) {
        vt_logw("config: %s: %s", path, strerror(errno));
        return VT_ERR_IO;
    }
    vt_config_clear(c);
    vt_free(c->path);
    c->path = vt_strdup(path);

    vt_cfg_sec_t *cur = _sec_get(c, "", true);
    char *save = NULL;
    char *line_copy = NULL;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        line_copy = vt_strndup(p, llen);
        /* continuation: trailing backslash */
        while (line_copy && line_copy[0] && line_copy[strlen(line_copy)-1] == '\\' && nl) {
            /* strip trailing backslash */
            line_copy[strlen(line_copy)-1] = 0;
            p = nl + 1;
            nl = strchr(p, '\n');
            size_t l2 = nl ? (size_t)(nl - p) : strlen(p);
            char *cont = vt_strndup(p, l2);
            char *joined = vt_strprintf("%s%s", line_copy, cont);
            vt_free(line_copy); vt_free(cont);
            line_copy = joined;
        }
        char *line = vt_strtrim(line_copy);
        if (!*line || *line == '#' || *line == ';') {
            /* comment or blank */
        } else if (*line == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = 0;
                char *name = vt_strtrim(line + 1);
                cur = _sec_get(c, name, true);
            }
        } else {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                char *k_raw = vt_strtrim(line);
                char *v_raw = vt_strtrim(eq + 1);
                char *k = vt_strdup(k_raw);
                char *v = vt_strdup(v_raw);
                _key_get(cur, k, true);
                /* overwrite value */
                for (size_t i = 0; i < cur->keys.size; i++) {
                    vt_cfg_key_t *kk = vt_vec_at(&cur->keys, i);
                    if (vt_strcaseeq(kk->key, k)) {
                        vt_free(kk->val);
                        kk->val = vt_strdup(v);
                        break;
                    }
                }
                vt_free(k); vt_free(v);
            }
        }
        vt_free(line_copy);
        if (!nl) break;
        p = nl + 1;
    }
    vt_free(buf);
    (void)save;
    struct stat st;
    if (stat(path, &st) == 0) c->mtime = (uint64_t)st.st_mtime;
    c->dirty = false;
    return VT_OK;
}

int vt_config_save(vt_config_t *c, const char *path) {
    if (!c) return VT_ERR_INVAL;
    const char *target = path ? path : c->path;
    if (!target) return VT_ERR_INVAL;
    FILE *fp = fopen(target, "w");
    if (!fp) return VT_ERR_IO;
    for (size_t i = 0; i < c->sections.size; i++) {
        vt_cfg_sec_t *s = vt_vec_at(&c->sections, i);
        if (i) fputc('\n', fp);
        if (s->name && *s->name) fprintf(fp, "[%s]\n", s->name);
        for (size_t j = 0; j < s->keys.size; j++) {
            vt_cfg_key_t *k = vt_vec_at(&s->keys, j);
            fprintf(fp, "%s = %s\n", k->key ? k->key : "", k->val ? k->val : "");
        }
    }
    fclose(fp);
    if (!path) { vt_free(c->path); c->path = vt_strdup(target); }
    c->dirty = false;
    return VT_OK;
}

bool vt_config_reload(vt_config_t *c) {
    if (!c || !c->path) return false;
    return vt_config_load(c, c->path) == VT_OK;
}

const char *vt_config_get(vt_config_t *c, const char *sec, const char *key, const char *def) {
    if (!c || !key) return def;
    vt_cfg_sec_t *s = _sec_get(c, sec, false);
    if (!s) return def;
    vt_cfg_key_t *k = _key_get(s, key, false);
    if (!k || !k->val) return def;
    return k->val;
}

long vt_config_get_int(vt_config_t *c, const char *sec, const char *key, long def) {
    const char *v = vt_config_get(c, sec, key, NULL);
    long out;
    if (vt_parse_int(v, &out)) return out;
    return def;
}

bool vt_config_get_bool(vt_config_t *c, const char *sec, const char *key, bool def) {
    const char *v = vt_config_get(c, sec, key, NULL);
    bool out;
    if (vt_parse_bool(v, &out)) return out;
    return def;
}

double vt_config_get_double(vt_config_t *c, const char *sec, const char *key, double def) {
    const char *v = vt_config_get(c, sec, key, NULL);
    double out;
    if (vt_parse_double(v, &out)) return out;
    return def;
}

bool vt_config_set(vt_config_t *c, const char *sec, const char *key, const char *val) {
    if (!c || !key) return false;
    vt_cfg_sec_t *s = _sec_get(c, sec, true);
    vt_cfg_key_t *k = _key_get(s, key, true);
    vt_free(k->val);
    k->val = vt_strdup(val ? val : "");
    c->dirty = true;
    return true;
}

bool vt_config_set_int(vt_config_t *c, const char *sec, const char *key, long val) {
    char buf[32]; snprintf(buf, sizeof(buf), "%ld", val);
    return vt_config_set(c, sec, key, buf);
}
bool vt_config_set_bool(vt_config_t *c, const char *sec, const char *key, bool val) {
    return vt_config_set(c, sec, key, val ? "true" : "false");
}

/* --- inotify-based live reload -------------------------------------------- */
typedef struct {
    int      fd;
    int      wd;
    vt_config_cb_t cb;
    vt_config_t *cfg;
    void    *ud;
    bool     running;
    pthread_t th;
} _watch_t;

static void *_watch_thread(void *arg) {
    _watch_t *w = arg;
    char buf[4096] __attribute__((aligned(8)));
    while (w->running) {
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(w->fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int rv = select(w->fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) continue;
        ssize_t n = read(w->fd, buf, sizeof(buf));
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *e = (void *)p;
            if (e->wd == w->wd && (e->mask & IN_MODIFY)) {
                if (w->cb) w->cb(w->cfg, w->ud);
                break;
            }
            p += sizeof(*e) + e->len;
        }
    }
    return NULL;
}

int vt_config_watch(vt_config_t *c, vt_config_cb_t cb, void *ud) {
    if (!c || !c->path) return VT_ERR_INVAL;
    vt_config_unwatch(c);
    _watch_t *w = vt_malloc0(sizeof(*w));
    w->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (w->fd < 0) { vt_free(w); return VT_ERR_IO; }
    w->wd = inotify_add_watch(w->fd, c->path, IN_MODIFY);
    if (w->wd < 0) { close(w->fd); vt_free(w); return VT_ERR_IO; }
    w->cb = cb; w->cfg = c; w->ud = ud; w->running = true;
    pthread_create(&w->th, NULL, _watch_thread, w);
    c->priv = (void *)w;
    return VT_OK;
}

void vt_config_unwatch(vt_config_t *c) {
    if (!c || !c->priv) return;
    _watch_t *w = (void *)c->priv;
    w->running = false;
    pthread_join(w->th, NULL);
    close(w->fd);
    vt_free(w);
    c->priv = NULL;
}
