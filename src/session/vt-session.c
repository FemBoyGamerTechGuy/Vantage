/*
 * vt-session.c — Session lifecycle manager
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Owns startup, autostart, environment setup, shutdown, restart, and
 * power hooks (suspend / hibernate). Hooks are user callbacks called
 * at well-defined stages so the WM, panel, etc. can clean up.
 */

#define VT_LOG_DOMAIN "session"
#include <vantage/vt-session.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>

typedef struct {
    vt_session_hook_t fn;
    void *ud;
} _hook_t;

typedef struct {
    char *path;
    char *name;
    bool run;
} _autostart_t;

struct vt_session {
    vt_vec_t       hooks;
    vt_vec_t       autostarts;
    vt_vec_t       env_keys;
    vt_vec_t       env_vals;
    vt_session_power_cb_t pwr_cb;
    void           *pwr_ud;
    vt_session_stage_t stage;
    bool           running;
};

vt_session_t *vt_session_new(void) {
    vt_session_t *s = vt_malloc0(sizeof(*s));
    vt_vec_init(&s->hooks, sizeof(_hook_t), 8);
    vt_vec_init(&s->autostarts, sizeof(_autostart_t), 8);
    vt_vec_init(&s->env_keys, sizeof(char *), 16);
    vt_vec_init(&s->env_vals, sizeof(char *), 16);
    s->stage = VT_SESSION_STAGE_INIT;
    return s;
}
void vt_session_free(vt_session_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->autostarts.size; i++) {
        _autostart_t *a = vt_vec_at(&s->autostarts, i);
        vt_free(a->path); vt_free(a->name);
    }
    vt_vec_fini(&s->autostarts);
    vt_vec_fini(&s->hooks);
    for (size_t i = 0; i < s->env_keys.size; i++) {
        char **k = vt_vec_at(&s->env_keys, i);
        char **v = vt_vec_at(&s->env_vals, i);
        vt_free(*k); vt_free(*v);
    }
    vt_vec_fini(&s->env_keys);
    vt_vec_fini(&s->env_vals);
    vt_free(s);
}

static void _run_hooks(vt_session_t *s, vt_session_stage_t st) {
    for (size_t i = 0; i < s->hooks.size; i++) {
        _hook_t *h = vt_vec_at(&s->hooks, i);
        if (h->fn) h->fn(s, st, h->ud);
    }
}
void vt_session_add_hook(vt_session_t *s, vt_session_hook_t fn, void *ud) {
    if (!s || !fn) return;
    _hook_t h = { .fn = fn, .ud = ud };
    vt_vec_push(&s->hooks, &h);
}

void vt_session_set_env(vt_session_t *s, const char *k, const char *v) {
    if (!s || !k) return;
    char **kp = vt_malloc0(sizeof(char *)); *kp = vt_strdup(k);
    char **vp = vt_malloc0(sizeof(char *)); *vp = vt_strdup(v ? v : "");
    vt_vec_push(&s->env_keys, kp);
    vt_vec_push(&s->env_vals, vp);
    setenv(k, v ? v : "", 1);
    vt_free(kp); vt_free(vp);
}
const char *vt_session_get_env(vt_session_t *s, const char *k) {
    if (!s || !k) return NULL;
    return getenv(k);
}

/* Parse XDG autostart .desktop files for Exec= lines */
int vt_session_autostart_load(vt_session_t *s) {
    if (!s) return VT_ERR_INVAL;
    /* Standard autostart locations */
    const char *dirs[] = {
        vt_strprintf("%s/autostart", vt_config_dir()),
        VT_DATADIR "/autostart",
        "/etc/xdg/autostart",
        NULL,
    };
    for (int d = 0; dirs[d]; d++) {
        size_t n = 0;
        char **files = vt_file_list_dir(dirs[d], &n);
        if (!files) continue;
        for (size_t i = 0; i < n; i++) {
            if (!vt_strendswith(files[i], ".desktop")) {
                vt_free(files[i]); continue;
            }
            char *path = vt_path_join(dirs[d], files[i]);
            size_t flen = 0;
            char *content = vt_file_read_all(path, &flen);
            if (content) {
                char *p = content;
                while (*p) {
                    char *nl = strchr(p, '\n');
                    if (!nl) break;
                    *nl = 0;
                    if (vt_strstartswith(p, "Exec=")) {
                        char *cmd = vt_strtrim(p + 5);
                        _autostart_t a = { .path = vt_strdup(cmd),
                                            .name = vt_strdup(files[i]),
                                            .run = true };
                        vt_vec_push(&s->autostarts, &a);
                        vt_free(cmd);
                    }
                    p = nl + 1;
                }
                vt_free(content);
            }
            vt_free(path);
            vt_free(files[i]);
        }
        vt_free(files);
    }
    vt_logi("session: %zu autostart entries", s->autostarts.size);
    return VT_OK;
}

int vt_session_autostart_run(vt_session_t *s) {
    if (!s) return VT_ERR_INVAL;
    for (size_t i = 0; i < s->autostarts.size; i++) {
        _autostart_t *a = vt_vec_at(&s->autostarts, i);
        if (!a->run || !a->path) continue;
        pid_t pid = fork();
        if (pid == 0) {
            /* child */
            execl("/bin/sh", "sh", "-c", a->path, (char *)NULL);
            _exit(127);
        }
        vt_logi("session: autostarted '%s' pid=%d", a->path, pid);
    }
    return VT_OK;
}

void vt_session_set_power_handler(vt_session_t *s, vt_session_power_cb_t cb, void *ud) {
    if (s) { s->pwr_cb = cb; s->pwr_ud = ud; }
}

static int _default_power(vt_session_t *s, vt_session_end_t op) {
    /* Try /proc/sys/kernel sysrq-style reboot, or via init.
     * Real implementation uses power integration (see vt-power.c). */
    (void)s;
    switch (op) {
    case VT_SESSION_END_LOGOUT:
        /* Session will exit normally — caller handles. */
        return 0;
    case VT_SESSION_END_SHUTDOWN:
        return system("shutdown -h now");
    case VT_SESSION_END_REBOOT:
        return system("shutdown -r now");
    case VT_SESSION_END_SUSPEND:
        return system("systemctl suspend 2>/dev/null || echo disk > /sys/power/state");
    case VT_SESSION_END_HIBERNATE:
        return system("systemctl hibernate 2>/dev/null || echo disk > /sys/power/state");
    case VT_SESSION_END_RESTART:
        return 0;
    }
    return -1;
}

int vt_session_start(vt_session_t *s) {
    if (!s) return VT_ERR_INVAL;
    s->stage = VT_SESSION_STAGE_EARLY;
    _run_hooks(s, VT_SESSION_STAGE_EARLY);

    /* Standard environment defaults */
    if (!getenv("XDG_CURRENT_DESKTOP"))
        vt_session_set_env(s, "XDG_CURRENT_DESKTOP", "Vantage");
    if (!getenv("XDG_SESSION_DESKTOP"))
        vt_session_set_env(s, "XDG_SESSION_DESKTOP", "Vantage");
    vt_session_set_env(s, "DESKTOP_SESSION", "Vantage");

    s->stage = VT_SESSION_STAGE_COMPONENTS;
    _run_hooks(s, VT_SESSION_STAGE_COMPONENTS);

    vt_session_autostart_run(s);

    s->stage = VT_SESSION_STAGE_READY;
    _run_hooks(s, VT_SESSION_STAGE_READY);
    s->running = true;
    vt_logi("session: started");
    return VT_OK;
}

int vt_session_run(vt_session_t *s) {
    if (!s) return VT_ERR_INVAL;
    if (!s->running) vt_session_start(s);
    while (s->running) {
        vt_time_sleep_ms(100);
    }
    return VT_OK;
}

void vt_session_end(vt_session_t *s, vt_session_end_t how) {
    if (!s) return;
    int rc;
    if (s->pwr_cb) rc = s->pwr_cb(s, how, s->pwr_ud);
    else rc = _default_power(s, how);
    (void)rc;
    s->stage = VT_SESSION_STAGE_SHUTDOWN;
    _run_hooks(s, VT_SESSION_STAGE_SHUTDOWN);
    if (how == VT_SESSION_END_LOGOUT || how == VT_SESSION_END_RESTART) {
        s->running = false;
    }
}

vt_session_stage_t vt_session_stage(const vt_session_t *s) {
    return s ? s->stage : VT_SESSION_STAGE_INIT;
}
