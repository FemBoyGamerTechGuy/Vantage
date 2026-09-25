/*
 * vt-paths.c — Runtime resource + binary discovery
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Vantage must run both straight from the source/build tree
 * (`./vantage-session`, `./builddir/src/tools/vantage-wm`) and from an
 * installed prefix (`/usr/bin/vantage-session`, `~/.local/bin/...`)
 * without behaving fundamentally differently. This module implements
 * that lookup; see include/vantage/vt-paths.h for the documented
 * priority order.
 *
 * Nothing here may hardcode a build-machine path: the compiled prefix
 * (VT_DATADIR / VT_BINDIR / VT_SYSCONFDIR, from meson) is only ever the
 * last resort. Discovery is driven by the real location of the running
 * executable (/proc/self/exe), which follows symlinks — that is what
 * makes the repo-root launch symlinks (`./vantage-session`) resolve
 * their resources in the source tree's data/ directory.
 *
 * Only libc (+ pthread for the one-time init) is used.
 */

#define VT_LOG_DOMAIN "paths"
#include <vantage/vt-core.h>
#include <vantage/vt-paths.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ============================================================== state */

typedef struct {
    char *dir;    /* owned resource root */
    bool  dev;    /* this root is a source-tree data/ directory */
    char *label;  /* provenance, for the startup log line */
} vt_root_t;

static struct {
    pthread_once_t  once;
    vt_root_t      *roots;      /* ordered search path, owned */
    size_t          n_roots;
    char           *bin_dir;    /* directory of the running executable */
} _S = {
    .once    = PTHREAD_ONCE_INIT,
    .roots   = NULL,
    .n_roots = 0,
    .bin_dir = NULL,
};

/* ---------------------------------------------------------- path utils */

/* Duplicate `path` with the last component removed ("a/b/c" -> "a/b").
 * Returns NULL when there is no parent (already "/" or malformed). */
static char *_parent_dir(const char *path)
{
    if (!path) return NULL;
    char *copy = vt_strdup(path);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) {
        vt_free(copy);
        return NULL;
    }
    *slash = '\0';
    return copy;
}

/* Real path of the running executable. /proc/self/exe resolves both the
 * repo-root launch symlinks and any wrapper symlink, so it always names
 * the actual ELF. Static buffer, valid until the next call. */
static const char *_exe_path(void)
{
    static char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    return NULL;
}

/* Directory containing the running executable (cached, owned). */
static const char *_exe_dir(void)
{
    const char *exe = _exe_path();
    if (!exe) return NULL;
    if (_S.bin_dir) return _S.bin_dir;
    char *copy = vt_strdup(exe);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (slash && slash != copy) {
        *slash = '\0';
    } else if (slash == copy) {
        copy[1] = '\0';                     /* executable lives in "/" */
    } else {
        vt_free(copy);
        return NULL;
    }
    _S.bin_dir = copy;
    return _S.bin_dir;
}

/* Does `dir` hold Vantage shared resources? The bundled themes/ and
 * defaults/ directories are the markers; either is enough (an installed
 * prefix ships themes/, a source tree ships both). */
static bool _is_resource_root(const char *dir)
{
    if (!dir) return false;
    char *themes   = vt_path_join(dir, "themes");
    char *defaults = vt_path_join(dir, "defaults");
    bool ok = vt_path_is_dir(themes) || vt_path_is_dir(defaults);
    vt_free(themes);
    vt_free(defaults);
    return ok;
}

/* Is `dir` the root of the Vantage source tree? Identified by the
 * meson.build that also sits next to data/. */
static bool _is_source_tree_root(const char *dir)
{
    if (!dir) return false;
    char *mb = vt_path_join(dir, "meson.build");
    bool ok = vt_path_is_file(mb);
    vt_free(mb);
    return ok;
}

/* --------------------------------------------------- resource roots */

static void _roots_push(vt_root_t **arr, size_t *n, const char *dir,
                        bool dev, const char *label)
{
    if (!dir || !*dir) return;
    for (size_t i = 0; i < *n; i++)
        if (vt_streq((*arr)[i].dir, dir)) return;      /* dedupe */
    vt_root_t *grown = vt_realloc(*arr, (*n + 1) * sizeof(**arr));
    if (!grown) return;
    *arr = grown;
    (*arr)[*n].dir   = vt_strdup(dir);
    (*arr)[*n].dev   = dev;
    (*arr)[*n].label = vt_strdup(label ? label : "");
    (*n)++;
}

/* Walk up from the executable's directory looking for either layout:
 *
 *   <D>/share/vantage   installed prefix (<prefix>/bin -> <prefix>/share)
 *   <D>/data            source tree (<repo>/<builddir>/... -> <repo>/data)
 *
 * The installed layout is checked first at each level; the source-tree
 * layout additionally requires meson.build next to data/ so a stray
 * data/ directory somewhere above the tree cannot capture the lookup.
 * Fills `out` and returns true when a layout is found.
 */
static bool _detect_exe_relative(vt_root_t *out)
{
    out->dir = NULL;
    out->dev = false;
    out->label = NULL;

    const char *exedir = _exe_dir();
    if (!exedir) return false;
    char *cur = vt_strdup(exedir);
    if (!cur) return false;

    for (int depth = 0; cur && depth < 16; depth++) {
        char *share = vt_path_join_many(cur, "share", "vantage", NULL);
        if (_is_resource_root(share)) {
            out->dir   = vt_strdup(share);
            out->dev   = false;
            out->label = vt_strdup("installed prefix");
            vt_free(share);
            vt_free(cur);
            return true;
        }
        vt_free(share);

        if (_is_source_tree_root(cur)) {
            char *data = vt_path_join(cur, "data");
            if (_is_resource_root(data)) {
                out->dir   = vt_strdup(data);
                out->dev   = true;
                out->label = vt_strdup("development tree");
                vt_free(data);
                vt_free(cur);
                return true;
            }
            vt_free(data);
        }

        char *parent = _parent_dir(cur);
        vt_free(cur);
        cur = parent;
    }
    vt_free(cur);
    return false;
}

static void _resolve(void)
{
    vt_root_t *roots = NULL;
    size_t n = 0;

    /* Detect the executable-relative root first; an explicit override
     * pointing at the same source-tree data/ directory must still be
     * reported as a development tree. */
    vt_root_t exerel;
    bool have_exerel = _detect_exe_relative(&exerel);

    /* 1. explicit override (dev trees, tests) */
    const char *ovr = getenv("VANTAGE_RESOURCE_DIR");
    if (ovr && *ovr) {
        bool same_dev = have_exerel && exerel.dev && vt_streq(exerel.dir, ovr);
        _roots_push(&roots, &n, ovr, same_dev,
                    same_dev ? "development tree (VANTAGE_RESOURCE_DIR)"
                             : "VANTAGE_RESOURCE_DIR");
    }

    /* 2. executable-relative: installed share/vantage or source data/ */
    if (have_exerel) {
        _roots_push(&roots, &n, exerel.dir, exerel.dev, exerel.label);
        vt_free(exerel.dir);
        vt_free(exerel.label);
    }

    /* 3. XDG data dirs: $XDG_DATA_HOME, then $XDG_DATA_DIRS */
    _roots_push(&roots, &n,
                vt_strprintf("%s/vantage", vt_data_dir_user()),
                false, "XDG data home");
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char *dcopy = vt_strdup(dirs);
    if (dcopy) {
        char *save = NULL, *tok = NULL;
        for (tok = strtok_r(dcopy, ":", &save); tok;
             tok = strtok_r(NULL, ":", &save)) {
            if (!*tok) continue;
            char *r = vt_strprintf("%s/vantage", tok);
            _roots_push(&roots, &n, r, false, "XDG data directory");
            vt_free(r);
        }
        vt_free(dcopy);
    }

    /* 4. compiled installation prefix (last resort) */
    _roots_push(&roots, &n, VT_DATADIR, false, "compiled prefix");

    _S.roots = roots;
    _S.n_roots = n;

    if (n > 0) {
        vt_logi("resource directory: %s (%s)",
                roots[0].dir, roots[0].label);
    } else {
        vt_logw("no resource directory found "
                "(set VANTAGE_RESOURCE_DIR or install Vantage)");
    }
}

static void _ensure(void)
{
    pthread_once(&_S.once, _resolve);
}

/* ============================================================== API */

bool vt_paths_is_dev_tree(void)
{
    _ensure();
    /* Only counts when the source tree's data/ directory actually won
     * the lookup (or was selected explicitly via the override). */
    return (_S.n_roots > 0 && _S.roots[0].dev);
}

const char *vt_paths_resource_dir(void)
{
    _ensure();
    return (_S.n_roots > 0) ? _S.roots[0].dir : NULL;
}

char *vt_paths_resource_find(const char *relpath)
{
    if (!relpath || !*relpath) return NULL;
    _ensure();
    for (size_t i = 0; i < _S.n_roots; i++) {
        char *cand = vt_path_join(_S.roots[i].dir, relpath);
        if (vt_path_exists(cand)) return cand;
        vt_free(cand);
    }
    return NULL;
}

char *vt_paths_config_default(void)
{
    /* 1. user configuration always wins when present */
    char *user = vt_path_join_many(vt_config_dir(), "vantage",
                                    "vantage.conf", NULL);
    if (vt_path_is_file(user)) return user;

    /* 2. resource default: the source tree's data/defaults/vantage.conf
     *    or an installed <share>/vantage/defaults/vantage.conf */
    _ensure();
    for (size_t i = 0; i < _S.n_roots; i++) {
        char *cand = vt_path_join_many(_S.roots[i].dir, "defaults",
                                       "vantage.conf", NULL);
        if (vt_path_is_file(cand)) {
            vt_free(user);
            return cand;
        }
        vt_free(cand);
    }

    /* 3. system sysconfdir (packagers install the default there) */
    char *sys = vt_strprintf(VT_SYSCONFDIR "/vantage.conf");
    if (vt_path_is_file(sys)) {
        vt_free(user);
        return sys;
    }
    vt_free(sys);

    /* 4. never NULL: fall back to the (future) user config path */
    return user;
}

char *vt_paths_bin_find(const char *name)
{
    if (!name || !*name || strchr(name, '/')) return NULL;

    /* 1. explicit override */
    const char *bindir = getenv("VANTAGE_BIN_DIR");
    if (bindir && *bindir) {
        char *cand = vt_path_join(bindir, name);
        if (access(cand, X_OK) == 0) return cand;
        vt_free(cand);
    }

    /* 2. next to the running executable: the build-tree output
     *    directory (all tools live in <builddir>/src/tools) or the
     *    installed bindir */
    _ensure();
    if (_S.bin_dir) {
        char *cand = vt_path_join(_S.bin_dir, name);
        if (access(cand, X_OK) == 0) return cand;
        vt_free(cand);
    }

    /* 3. $PATH */
    const char *path = getenv("PATH");
    if (path && *path) {
        char *copy = vt_strdup(path);
        if (copy) {
            char *save = NULL, *tok = NULL;
            for (tok = strtok_r(copy, ":", &save); tok;
                 tok = strtok_r(NULL, ":", &save)) {
                if (!*tok) continue;
                char *cand = vt_path_join(tok, name);
                if (access(cand, X_OK) == 0) {
                    vt_free(copy);
                    return cand;
                }
                vt_free(cand);
            }
            vt_free(copy);
        }
    }

    /* 4. compiled installation prefix */
    char *cand = vt_strprintf(VT_BINDIR "/%s", name);
    if (access(cand, X_OK) == 0) return cand;
    vt_free(cand);

    return NULL;
}

const char *vt_paths_prefix(void)  { return VT_PREFIX; }
const char *vt_paths_version(void) { return VT_VERSION; }
