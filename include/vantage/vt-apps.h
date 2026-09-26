/*
 * vt-apps.h — shared XDG application discovery for every Vantage surface
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * ONE application database for the whole desktop: the X11 panel, the
 * native Wayland compositor panel, and the remote tooling all resolve
 * .desktop entries through this module. There is deliberately no
 * second, backend-private app list — the menu must look identical on
 * both backends.
 *
 * Responsibilities:
 *   - .desktop group parsing ([Desktop Entry] only)
 *   - locale-aware Name selection (LC_ALL > LC_MESSAGES > LANG,
 *     Name[lang_COUNTRY] > Name[lang] > Name=) so non-English desktop
 *     entries (e.g. Russian) display correctly instead of leaking
 *     whichever localized line happens to come first in the file
 *   - Exec field-code stripping (%f %F %u %U %i %c %k %d %n %v %m)
 *   - category normalization onto the fixed menu category table
 *   - NoDisplay / Hidden / NotShowIn=Vantage filtering, TryExec checks
 *   - Terminal=true wrapping with a real terminal emulator
 */
#ifndef VANTAGE_APPS_H
#define VANTAGE_APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vt_apps vt_apps_t;

typedef struct {
    char       *id;        /* desktop-file id (basename) */
    char       *name;      /* locale-resolved display name (UTF-8) */
    char       *exec;      /* cleaned command line */
    char       *icon;      /* icon name or absolute path (may be NULL) */
    char       *keywords;  /* search keywords (may be NULL) */
    const char *category;  /* label from the fixed category table */
    bool        terminal;  /* Terminal=true — needs a terminal */
} vt_app_t;

/* Fixed display-category table, in menu order:
 * Accessories, Development, Education, Games, Graphics, Internet,
 * Multimedia, Office, System, Utilities, Other. */
int         vt_apps_category_count(void);
const char *vt_apps_category_label(int idx);
int         vt_apps_category_index(const char *label);

/* Scan the standard XDG application directories (user dir first, then
 * XDG_DATA_DIRS, then /usr/share + /usr/local/share fallbacks) and
 * return the sorted application list. Never returns NULL. */
vt_apps_t *vt_apps_load(void);

/* Same, but from an explicit directory list (used by tests and by
 * embedders that want an isolated database). Dirs are scanned in the
 * given order; earlier wins on id collisions. */
vt_apps_t *vt_apps_load_dirs(const char *const *dirs, size_t n);

size_t           vt_apps_n(const vt_apps_t *a);
const vt_app_t  *vt_apps_at(const vt_apps_t *a, size_t i);
int              vt_apps_in_category(const vt_apps_t *a, int idx);
/* row-th (0-based) application inside category idx, or NULL */
const vt_app_t  *vt_apps_category_row(const vt_apps_t *a, int idx,
                                      size_t row);
const vt_app_t  *vt_apps_find_id(const vt_apps_t *a, const char *id);

/* Full shell command for launching (Terminal=true wrapped in a real
 * terminal emulator found in PATH; VANTAGE_TERMINAL overrides). Caller
 * frees. */
char *vt_apps_launch_cmd(const vt_app_t *app);

void vt_apps_free(vt_apps_t *a);

#ifdef __cplusplus
}
#endif
#endif
