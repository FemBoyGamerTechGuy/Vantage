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

typedef struct {
    char    *name;
    char    *cmd;
    pid_t    pid;
    pid_t    pgid;         /* process group id (== first spawn pid) */
    bool     critical;
    bool     cmd_missing;
    int      restarts;
    uint64_t last_start_ms;
    uint64_t restart_delay_ms;
} _managed_t;

struct vt_session {
    vt_vec_t       hooks;
    vt_vec_t       autostarts;
    vt_vec_t       env_keys;
    vt_vec_t       env_vals;
    vt_session_power_cb_t pwr_cb;
    void           *pwr_ud;
    vt_session_stage_t stage;
    bool           running;
    void           *managed;      /* vt_vec_t of _managed_t */
    vt_session_end_t end_action;
};

vt_session_t *vt_session_new(void) {
    vt_session_t *s = vt_malloc0(sizeof(*s));
    vt_vec_init(&s->hooks, sizeof(_hook_t), 8);
    vt_vec_init(&s->autostarts, sizeof(_autostart_t), 8);
    vt_vec_init(&s->env_keys, sizeof(char *), 16);
    vt_vec_init(&s->env_vals, sizeof(char *), 16);
    vt_vec_t *m = vt_malloc0(sizeof(vt_vec_t));
    vt_vec_init(m, sizeof(_managed_t), 8);
    s->managed = m;
    s->stage = VT_SESSION_STAGE_INIT;
    s->end_action = VT_SESSION_END_LOGOUT;
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
    if (s->managed) {
        vt_vec_t *v = (vt_vec_t *)s->managed;
        for (size_t i = 0; i < v->size; i++) {
            _managed_t *m = vt_vec_at(v, i);
            vt_free(m->name);
            vt_free(m->cmd);
        }
        vt_vec_fini(v);
        vt_free(v);
    }
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
                char *exec = NULL, *hidden = NULL, *onlyin = NULL,
                     *notin = NULL, *tryexec = NULL;
                char *p = content;
                while (*p) {
                    char *nl = strchr(p, '\n');
                    if (!nl) break;
                    *nl = 0;
                    if (vt_strstartswith(p, "Exec=") && !exec)
                        exec = vt_strdup(vt_strtrim(p + 5));
                    else if (vt_strstartswith(p, "Hidden=") &&
                             strstr(p, "true"))
                        hidden = vt_strdup("1");
                    else if (vt_strstartswith(p, "OnlyShowIn=") && !onlyin)
                        onlyin = vt_strdup(p + 12);
                    else if (vt_strstartswith(p, "NotShowIn=") && !notin)
                        notin = vt_strdup(p + 11);
                    else if (vt_strstartswith(p, "TryExec=") && !tryexec)
                        tryexec = vt_strdup(p + 9);
                    p = nl + 1;
                }
                bool run = exec && !hidden;
                if (run && onlyin && !strstr(onlyin, "Vantage;") &&
                    !strstr(onlyin, "GNOME;") && !strstr(onlyin, "XFCE;"))
                    run = false;
                if (run && notin && strstr(notin, "Vantage;"))
                    run = false;
                if (run && tryexec && *tryexec && !vt_proc_find_in_path(tryexec))
                    run = false;
                /* user autostart overrides system entries by filename */
                if (run) {
                    for (size_t k = 0; k < s->autostarts.size; k++) {
                        _autostart_t *prev = vt_vec_at(&s->autostarts, k);
                        if (vt_streq(prev->name, files[i])) {
                            vt_free(prev->path);
                            prev->path = vt_strdup(exec);
                            run = false; /* replaced */
                            break;
                        }
                    }
                }
                if (run) {
                    _autostart_t a = { .path = vt_strdup(exec),
                                       .name = vt_strdup(files[i]),
                                       .run = true };
                    vt_vec_push(&s->autostarts, &a);
                }
                vt_free(exec);
                vt_free(hidden);
                vt_free(onlyin);
                vt_free(notin);
                vt_free(tryexec);
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
    s->end_action = how;
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
bool vt_session_is_running(const vt_session_t *s) {
    return s ? s->running : false;
}

/* ============================================================ supervisor */
int vt_session_spawn_managed(vt_session_t *s, const char *name,
                             const char *cmd, bool critical) {
    if (!s || !name || !cmd) return VT_ERR_INVAL;
    vt_vec_t *v = (vt_vec_t *)s->managed;
    _managed_t m = { .name = vt_strdup(name), .cmd = vt_strdup(cmd),
                     .pid = -1, .pgid = -1, .critical = critical,
                     .restarts = 0, .last_start_ms = 0,
                     .restart_delay_ms = 500 };
    pid_t pid = fork();
    if (pid < 0) return VT_ERR;
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    m.pid = pid;
    m.pgid = pid;      /* setsid() in the child makes it group leader */
    m.last_start_ms = vt_time_now_ms();
    vt_vec_push(v, &m);
    vt_logi("session: started '%s' pid=%d (critical=%d)", name, pid, critical);
    return VT_OK;
}

void vt_session_supervise(vt_session_t *s) {
    if (!s || !s->managed) return;
    vt_vec_t *v = (vt_vec_t *)s->managed;
    /* Always reap — including during shutdown (zombies must not make
     * children_alive() report them as running). */
    int status;
    pid_t dead;
    while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
        _managed_t *m = NULL;
        for (size_t i = 0; i < v->size; i++) {
            _managed_t *p = vt_vec_at(v, i);
            if (p->pid == dead) { m = p; break; }
        }
        if (!m) continue; /* autostart child — not managed */
        vt_logw("session: '%s' (pid %d) exited (status %d)", m->name, dead,
                WEXITSTATUS(status));
        if (WEXITSTATUS(status) == 127)
            m->cmd_missing = true;   /* exec failed — do not restart */
        m->pid = -1;
    }
    if (s->stage == VT_SESSION_STAGE_SHUTDOWN) return; /* no restarts now */
    uint64_t now = vt_time_now_ms();
    for (size_t i = 0; i < v->size; i++) {
        _managed_t *m = vt_vec_at(v, i);
        if (m->pid > 0) continue;
        if (m->cmd_missing) continue;   /* command not found — give up */
        if (m->restarts >= 8 && m->critical) {
            vt_loge("session: critical component '%s' failed %d times — "
                    "ending session", m->name, m->restarts);
            vt_session_end(s, VT_SESSION_END_LOGOUT);
            return;
        }
        uint64_t delay = m->critical ? m->restart_delay_ms : 1000;
        if (now - m->last_start_ms < delay) continue;
        m->restarts++;
        m->restart_delay_ms *= 2;
        if (m->restart_delay_ms > 16000) m->restart_delay_ms = 16000;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", m->cmd, (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            m->pid = pid;
            m->last_start_ms = now;
            vt_logi("session: restarted '%s' pid=%d (attempt %d)",
                    m->name, pid, m->restarts);
        }
    }
}

void vt_session_term_children(vt_session_t *s) {
    if (!s || !s->managed) return;
    vt_vec_t *v = (vt_vec_t *)s->managed;
    for (size_t i = 0; i < v->size; i++) {
        _managed_t *m = vt_vec_at(v, i);
        /* children run in their own session (setsid at spawn), so the
         * process group id == the spawned pid. Signal the whole group:
         * `sh -c` wrappers may fork, orphaning the real process. */
        if (m->pid > 0) kill(-m->pid, SIGTERM);
        if (m->pid > 0) kill(m->pid, SIGTERM);
    }
}

int vt_session_stop_children(vt_session_t *s, int sig) {
    if (!s || !s->managed) return 0;
    vt_vec_t *v = (vt_vec_t *)s->managed;
    int n = 0;
    for (size_t i = 0; i < v->size; i++) {
        _managed_t *m = vt_vec_at(v, i);
        if (m->pid > 0) { kill(-m->pid, sig); kill(m->pid, sig); n++; }
        else if (kill(-m->pgid, sig) == 0) n++;
    }
    return n;
}

size_t vt_session_children_alive(const vt_session_t *s) {
    if (!s || !s->managed) return 0;
    vt_vec_t *v = (vt_vec_t *)s->managed;
    size_t n = 0;
    for (size_t i = 0; i < v->size; i++) {
        _managed_t *m = vt_vec_at(v, i);
        if (m->pid > 0 && kill(m->pid, 0) == 0) { n++; continue; }
        /* direct child reaped, but the process group may still hold the
         * actual component (sh -c wrappers fork) */
        if (m->pgid > 0 && kill(-m->pgid, 0) == 0) { n++; continue; }
    }
    return n;
}
