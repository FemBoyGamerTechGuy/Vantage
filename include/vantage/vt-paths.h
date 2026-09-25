/*
 * vt-paths.h — Runtime resource + binary discovery
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Vantage must run both straight from the source/build tree
 * (`./vantage-session`, `./build/src/tools/vantage-wm`) and from an
 * installed prefix (`/usr/bin/vantage-session`, `~/.local/bin/...`)
 * without behaving fundamentally differently. This module implements
 * that lookup.
 *
 * Resource (share/) search priority:
 *
 *   1. $VANTAGE_RESOURCE_DIR      — explicit override (dev trees, tests)
 *   2. executable-relative resource directory, detected by walking up
 *      from the real binary location and finding a directory that
 *      contains Vantage resources:
 *        <prefix>/bin/prog      -> <prefix>/share/vantage   (installed)
 *        <repo>/build/src/tools -> <repo>/data              (build tree)
 *   3. XDG data dirs: $XDG_DATA_HOME/vantage, $XDG_DATA_DIRS/vantage
 *   4. compiled installation prefix (VT_DATADIR, from meson)
 *
 * The same scheme locates sibling executables (the session manager
 * finding vantage-wm/vantage-panel next to itself) and the default
 * configuration file. No absolute build-machine paths are baked in.
 */
#ifndef VANTAGE_PATHS_H
#define VANTAGE_PATHS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when Vantage detects it is running from a development tree
 * (resource dir is the source tree's data/ directory). Informational. */
bool        vt_paths_is_dev_tree(void);

/* Root directory of Vantage shared resources (themes/, defaults/),
 * following the priority order above. Returns a cached string owned by
 * Vantage (do not free); NULL when nothing was found. */
const char *vt_paths_resource_dir(void);

/* Full path of a resource file given relative to the resource root,
 * e.g. vt_paths_resource_find("themes/Vantage-Dark/theme.json").
 * Returns a heap string the caller owns (vt_free), or NULL when the
 * file does not exist anywhere in the search path. */
char       *vt_paths_resource_find(const char *relpath);

/* Default configuration file path: user config first, then the
 * development-tree default, then the system sysconfdir. Heap string;
 * never NULL (falls back to the user-config path even when absent). */
char       *vt_paths_config_default(void);

/* Locate a Vantage executable by name ("vantage-wm" ...):
 *   1. $VANTAGE_BIN_DIR/<name>
 *   2. directory of the running executable (dev tree or installed bindir)
 *   3. $PATH
 *   4. compiled VT_BINDIR
 * Returns a heap string usable in a shell command line, or NULL. */
char       *vt_paths_bin_find(const char *name);

/* Install prefix this binary was configured with (VT_PREFIX). */
const char *vt_paths_prefix(void);

/* Single version source (meson project version == VT_VERSION). */
const char *vt_paths_version(void);

#ifdef __cplusplus
}
#endif
#endif
