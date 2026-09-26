/*
 * vt-apps.c — shared XDG application discovery (.desktop database)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * See vt-apps.h for the contract. This file is deliberately dependency
 * free (libc only) so every component — X11 panel, Wayland compositor
 * panel, tools, tests — can share the exact same database.
 */

#define VT_LOG_DOMAIN "apps"
#include <vantage/vt-apps.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>

/* ------------------------------------------------------ categories */
/* DISPLAY table — fixed menu order shown on every surface */
static const char *const _cat_labels[] = {
    "Accessories", "Development", "Education", "Games", "Graphics",
    "Internet", "Multimedia", "Office", "System", "Utilities", "Other",
};
#define _N_CATS ((int)(sizeof(_cat_labels) / sizeof(_cat_labels[0])))
#define _CAT_ACCESSORIES 0
#define _CAT_UTILITIES   9
#define _CAT_OTHER       10

int vt_apps_category_count(void) { return _N_CATS; }

const char *vt_apps_category_label(int idx) {
    return idx >= 0 && idx < _N_CATS ? _cat_labels[idx] : NULL;
}

int vt_apps_category_index(const char *label) {
    if (!label) return -1;
    for (int i = 0; i < _N_CATS; i++)
        if (vt_streq(_cat_labels[i], label)) return i;
    return -1;
}

/* MATCH priority — specific desktop categories win over the generic
 * Utility split. (idx, needs_terminal) pairs, first match wins. */
static const struct { int idx; bool needs_terminal; const char *keys[4]; }
_cat_prio[] = {
    { 5,  false, { "Network", NULL } },                       /* Internet   */
    { 6,  false, { "AudioVideo", "Audio", "Video", NULL } },  /* Multimedia */
    { 1,  false, { "Development", NULL } },                   /* Development */
    { 2,  false, { "Education", "Science", NULL } },          /* Education  */
    { 3,  false, { "Game", NULL } },                          /* Games      */
    { 4,  false, { "Graphics", NULL } },                      /* Graphics   */
    { 7,  false, { "Office", NULL } },                        /* Office     */
    { 8,  false, { "Settings", "System", "Core", NULL } },    /* System     */
    { 9,  true,  { "Utility", NULL } },                       /* Utilities  */
    { 0,  false, { "Utility", NULL } },                       /* Accessories */
};

static bool _cat_has_token(const char *cats, const char *key) {
    const char *p = cats;
    size_t kl = strlen(key);
    while (p && *p) {
        const char *semi = strchr(p, ';');
        size_t tl = semi ? (size_t)(semi - p) : strlen(p);
        if (tl == kl && strncmp(p, key, kl) == 0) return true;
        p = semi ? semi + 1 : NULL;
    }
    return false;
}

static int _category_of(const char *cats, bool terminal) {
    if (!cats || !*cats) return _CAT_OTHER;
    for (size_t i = 0; i < sizeof(_cat_prio) / sizeof(_cat_prio[0]); i++) {
        if (_cat_prio[i].needs_terminal && !terminal) continue;
        for (int k = 0; _cat_prio[i].keys[k]; k++)
            if (_cat_has_token(cats, _cat_prio[i].keys[k]))
                return _cat_prio[i].idx;
    }
    return _CAT_OTHER;
}

/* ------------------------------------------------------------ impl */
struct vt_apps {
    vt_vec_t apps;      /* vt_app_t, sorted by name */
};

/* the locale we display names in ("ru", "pt_BR", "" for C) */
static void _locale_lang(char *out, size_t out_n, char *out_region,
                         size_t region_n) {
    out[0] = 0; out_region[0] = 0;
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    if (!l || !*l) return;
    /* form: lang[_territory][.encoding][@modifier] */
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", l);
    char *dot = strpbrk(buf, ".@");
    if (dot) *dot = 0;
    char *us = strchr(buf, '_');
    if (us) {
        *us = 0;
        snprintf(out_region, region_n, "%s_%s", buf, us + 1);
    }
    snprintf(out, out_n, "%s", buf);
    /* never match the raw C/POSIX locale */
    if (vt_streq(out, "C") || vt_streq(out, "POSIX")) out[0] = 0;
}

static char *_strip_field_codes(const char *exec) {
    if (!exec) return NULL;
    char *out = vt_malloc(strlen(exec) + 1);
    size_t o = 0;
    for (size_t i = 0; exec[i];) {
        if (exec[i] == '%' && exec[i + 1] &&
            strchr("fFuUdDnNickvem", exec[i + 1])) {
            i += 2;                     /* drop the %X field code */
            continue;
        }
        out[o++] = exec[i++];
    }
    out[o] = 0;
    return vt_strtrim(out);
}

typedef struct {
    char *name;              /* Name= */
    char *name_loc;          /* Name[lang] */
    char *name_loc_region;   /* Name[lang_REGION] */
    char *exec;
    char *icon;
    char *keywords;
    char *cats;
    char *tryexec;
    char *onlyshowin;
    char *notshowin;
    bool  terminal;
    bool  nodisplay;
} _entry_t;

static void _entry_clear(_entry_t *e) {
    vt_free(e->name); vt_free(e->name_loc); vt_free(e->name_loc_region);
    vt_free(e->exec); vt_free(e->icon); vt_free(e->keywords);
    vt_free(e->cats); vt_free(e->tryexec);
    vt_free(e->onlyshowin); vt_free(e->notshowin);
    memset(e, 0, sizeof(*e));
}

/* parse one .desktop file into _entry_t; returns false when the entry
 * should not be shown at all (NoDisplay/Hidden/NotShowIn/TryExec) */
static bool _parse_desktop(const char *path, _entry_t *e,
                           const char *lang, const char *lang_region) {
    memset(e, 0, sizeof(*e));
    size_t len = 0;
    char *content = vt_file_read_all(path, &len);
    if (!content) return false;

    bool in_group = false;
    char langkey[80], regionkey[80];
    snprintf(langkey, sizeof(langkey), "Name[%s]=", lang);
    snprintf(regionkey, sizeof(regionkey), "Name[%s]=", lang_region);

    char *save = NULL;
    for (char *line = strtok_r(content, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '[') {
            in_group = strncmp(line, "[Desktop Entry]", 15) == 0;
            continue;
        }
        if (!in_group) continue;
        if (vt_strstartswith(line, "Name=") && !e->name)
            e->name = vt_strdup(line + 5);
        else if (*lang_region && vt_strstartswith(line, regionkey) &&
                 !e->name_loc_region)
            e->name_loc_region = vt_strdup(line + strlen(regionkey));
        else if (*lang && vt_strstartswith(line, langkey) && !e->name_loc)
            e->name_loc = vt_strdup(line + strlen(langkey));
        else if (vt_strstartswith(line, "Exec=") && !e->exec)
            e->exec = vt_strdup(line + 5);
        else if (vt_strstartswith(line, "Icon=") && !e->icon)
            e->icon = vt_strdup(line + 5);
        else if (vt_strstartswith(line, "Keywords=") && !e->keywords)
            e->keywords = vt_strdup(line + 9);
        else if (vt_strstartswith(line, "Categories=") && !e->cats)
            e->cats = vt_strdup(line + 11);
        else if (vt_strstartswith(line, "TryExec=") && !e->tryexec)
            e->tryexec = vt_strdup(line + 8);
        else if (vt_strstartswith(line, "OnlyShowIn=") && !e->onlyshowin)
            e->onlyshowin = vt_strdup(line + 11);
        else if (vt_strstartswith(line, "NotShowIn=") && !e->notshowin)
            e->notshowin = vt_strdup(line + 10);
        else if (vt_strstartswith(line, "Terminal=true"))
            e->terminal = true;
        else if (vt_strstartswith(line, "Terminal=1"))
            e->terminal = true;
        else if (vt_strstartswith(line, "NoDisplay=true") ||
                 vt_strstartswith(line, "Hidden=true"))
            e->nodisplay = true;
    }
    vt_free(content);
    if (e->nodisplay || !e->name || !e->exec) return false;
    /* locale preference: plain language first, then the more specific
     * lang_REGION variant last so it wins */
    if (e->name_loc)       { vt_free(e->name); e->name = e->name_loc; e->name_loc = NULL; }
    if (e->name_loc_region){ vt_free(e->name); e->name = e->name_loc_region; e->name_loc_region = NULL; }
    return true;
}

/* OnlyShowIn: permissive — an app restricted to *some* desktop shows
 * unless it explicitly excludes Vantage via NotShowIn. Hiding every
 * GNOME/KDE-flagged tool would gut whole categories on real systems. */
static bool _showin_ok(const _entry_t *e) {
    if (e->notshowin) {
        char *copy = vt_strdup(e->notshowin);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ";", &save); tok;
             tok = strtok_r(NULL, ";", &save))
            if (vt_streq(tok, "Vantage")) {
                vt_free(copy);
                return false;
            }
        vt_free(copy);
    }
    return true;
}

static void _scan_dir(vt_vec_t *apps, vt_vec_t *seen, const char *dir,
                      const char *lang, const char *lang_region) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!vt_strendswith(de->d_name, ".desktop")) continue;
        /* desktop-file id = basename; the FIRST directory that carries
         * the id owns it (XDG semantics), shown or not */
        bool dup = false;
        for (size_t i = 0; i < seen->size; i++)
            if (vt_streq(*(const char *const *)vt_vec_at(seen, i),
                         de->d_name)) { dup = true; break; }
        if (dup) continue;
        char *id = vt_strdup(de->d_name);
        vt_vec_push(seen, &id);
        char *path = vt_strprintf("%s/%s", dir, de->d_name);
        _entry_t e;
        if (_parse_desktop(path, &e, lang, lang_region) && _showin_ok(&e)) {
            if (!e.tryexec || vt_proc_find_in_path(e.tryexec)) {
                vt_app_t a = {0};
                a.id = vt_strdup(de->d_name);
                a.name = vt_strdup(e.name);
                a.exec = _strip_field_codes(e.exec);
                a.icon = e.icon ? vt_strdup(e.icon) : NULL;
                a.keywords = e.keywords ? vt_strdup(e.keywords) : NULL;
                a.terminal = e.terminal;
                a.category = vt_apps_category_label(
                    _category_of(e.cats, e.terminal));
                vt_vec_push(apps, &a);
            }
        }
        _entry_clear(&e);
        vt_free(path);
    }
    closedir(d);
}

static int _app_cmp(const void *pa, const void *pb) {
    const vt_app_t *a = pa, *b = pb;
    return strcasecmp(a->name, b->name);
}

vt_apps_t *vt_apps_load_dirs(const char *const *dirs, size_t n) {
    vt_apps_t *db = vt_malloc0(sizeof(*db));
    vt_vec_init(&db->apps, sizeof(vt_app_t), 64);
    vt_vec_t seen;
    vt_vec_init(&seen, sizeof(char *), 64);

    char lang[32], region[32];
    _locale_lang(lang, sizeof(lang), region, sizeof(region));

    for (size_t i = 0; i < n; i++)
        if (dirs[i]) _scan_dir(&db->apps, &seen, dirs[i], lang, region);

    for (size_t i = 0; i < seen.size; i++) {
        char **pp = vt_vec_at(&seen, i);
        vt_free(*pp);
    }
    vt_vec_fini(&seen);

    vt_vec_sort(&db->apps, _app_cmp);
    vt_logi("apps: %zu applications indexed from %zu directories",
            db->apps.size, n);
    return db;
}

vt_apps_t *vt_apps_load(void) {
    const char *dirv[32];
    size_t n = 0;

    const char *xh = getenv("XDG_DATA_HOME");
    if (xh && *xh) dirv[n++] = vt_strprintf("%s/applications", xh);
    else dirv[n++] = vt_strprintf("%s/.local/share/applications",
                                  vt_home_dir());

    const char *xd = getenv("XDG_DATA_DIRS");
    if (xd && *xd) {
        char *copy = vt_strdup(xd);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ":", &save); tok && n < 28;
             tok = strtok_r(NULL, ":", &save))
            if (*tok) dirv[n++] = vt_strprintf("%s/applications", tok);
        vt_free(copy);
    } else {
        dirv[n++] = vt_strdup("/usr/local/share/applications");
        dirv[n++] = vt_strdup("/usr/share/applications");
    }

    vt_apps_t *db = vt_apps_load_dirs(dirv, n);
    for (size_t i = 0; i < n; i++)
        vt_free((void *)dirv[i]);
    return db;
}

size_t vt_apps_n(const vt_apps_t *a) { return a ? a->apps.size : 0; }

const vt_app_t *vt_apps_at(const vt_apps_t *a, size_t i) {
    return a && i < a->apps.size ? vt_vec_at((vt_vec_t *)&a->apps, i)
                                  : NULL;
}

int vt_apps_in_category(const vt_apps_t *a, int idx) {
    if (!a || idx < 0 || idx >= _N_CATS) return 0;
    const char *label = _cat_labels[idx];
    int n = 0;
    for (size_t i = 0; i < a->apps.size; i++) {
        vt_app_t *app = vt_vec_at((vt_vec_t *)&a->apps, i);
        if (app->category == label) n++;
    }
    return n;
}

const vt_app_t *vt_apps_category_row(const vt_apps_t *a, int idx,
                                     size_t row) {
    if (!a || idx < 0 || idx >= _N_CATS) return NULL;
    size_t r = 0;
    const char *label = _cat_labels[idx];
    for (size_t i = 0; i < a->apps.size; i++) {
        vt_app_t *app = vt_vec_at((vt_vec_t *)&a->apps, i);
        if (app->category == label) {
            if (r == row) return app;
            r++;
        }
    }
    return NULL;
}

const vt_app_t *vt_apps_find_id(const vt_apps_t *a, const char *id) {
    if (!a || !id) return NULL;
    for (size_t i = 0; i < a->apps.size; i++) {
        vt_app_t *app = vt_vec_at((vt_vec_t *)&a->apps, i);
        if (vt_streq(app->id, id)) return app;
    }
    return NULL;
}

char *vt_apps_launch_cmd(const vt_app_t *app) {
    if (!app || !app->exec) return NULL;
    if (!app->terminal) return vt_strdup(app->exec);

    /* Terminal=true — wrap in a real terminal emulator */
    static const struct { const char *name; const char *fmt; } terms[] = {
        { "kitty",              "%s -e %s" },
        { "alacritty",          "%s -e %s" },
        { "foot",               "%s -- %s" },
        { "wezterm",            "%s -e %s" },
        { "xterm",              "%s -e %s" },
        { "st",                 "%s -e %s" },
        { "x-terminal-emulator","%s -e %s" },
        { "konsole",            "%s -e %s" },
        { "gnome-terminal",     "%s -- %s" },
        { "xfce4-terminal",     "%s -x %s" },
    };
    const char *t = getenv("VANTAGE_TERMINAL");
    if (t && *t && !vt_proc_find_in_path(t)) {
        vt_logw("apps: VANTAGE_TERMINAL='%s' not found in PATH — "
                "searching standard terminals", t);
        t = NULL;
    }
    if (!t || !*t) {
        for (size_t i = 0; i < VT_ARRAY_SIZE(terms) && (!t || !*t); i++)
            if (vt_proc_find_in_path(terms[i].name)) t = terms[i].name;
    }
    if (!t || !*t) {
        vt_logw("apps: no terminal emulator found — launching '%s' "
                "without one (it needs a terminal)", app->exec);
        return vt_strdup(app->exec);
    }
    /* per-emulator exec conventions */
    const char *sep = "-e";
    if (vt_streq(t, "foot")) sep = "--";
    else if (vt_streq(t, "gnome-terminal")) sep = "--";
    else if (vt_streq(t, "xfce4-terminal")) sep = "-x";
    return vt_strprintf("%s %s %s", t, sep, app->exec);
}

void vt_apps_free(vt_apps_t *a) {
    if (!a) return;
    for (size_t i = 0; i < a->apps.size; i++) {
        vt_app_t *app = vt_vec_at(&a->apps, i);
        vt_free(app->id); vt_free(app->name); vt_free(app->exec);
        vt_free(app->icon); vt_free(app->keywords);
    }
    vt_vec_fini(&a->apps);
    vt_free(a);
}
