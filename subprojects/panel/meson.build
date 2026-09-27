# vantage-panel — the Vantage desktop panel (independent subproject)
#
# SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
#
# Builds `vantage-panel`: a GTK4 panel that runs on the Vantage X11
# backend and the native Vantage Wayland compositor alike (and on any
# other compositor honoring EWMH docks / wlr-layer-shell).
#
# This is a self-contained meson project: it can be built standalone
#    meson setup build && ninja -C build
# or as a subproject of the Vantage desktop environment (the parent
# consumes the `vantage_panel_dep` dependency below).

project('vantage-panel', 'c',
  version : '0.3.0',
  default_options : [
    'c_std=c17',
    'warning_level=2',
    'default_library=both',
  ],
  license : 'LicenseRef-Vantage-Proprietary',
  meson_version : '>= 0.59',
)

cc = meson.get_compiler('c')

# ---- dependencies ---------------------------------------------------
dep_gtk = dependency('gtk4', version : '>= 4.10')
dep_gio_unix = dependency('gio-unix-2.0', required : false)
dep_layer_shell = dependency('gtk4-layer-shell-0', required : false)
dep_alsa = dependency('alsa', required : false)

panel_cargs = []
panel_deps = [dep_gtk]
if dep_gio_unix.found()
  panel_deps += [dep_gio_unix]
endif

# wlr-layer-shell docking (Wayland); without it the panel still runs —
# as a plain window on Wayland and as an EWMH dock on X11
if dep_layer_shell.found()
  panel_deps += [dep_layer_shell]
  panel_cargs += ['-DHAVE_LAYER_SHELL']
endif

# raw X11 calls for the EWMH dock properties (GTK4 does not expose
# struts/window-type itself)
dep_x11 = dependency('x11', required : false)
if dep_x11.found()
  panel_deps += [dep_x11]
  panel_cargs += ['-DHAVE_X11_DOCK']
endif

# real ALSA mixer values; "vol --" when unavailable
if dep_alsa.found()
  panel_deps += [dep_alsa]
  panel_cargs += ['-DHAVE_ALSA']
endif

panel_sources = [
  'src/main.c',
  'src/picon.c',
  'src/ipc.c',
  'src/wm.c',
  'src/dock.c',
  'src/launcher.c',
  'src/tasklist.c',
  'src/pager.c',
  'src/clock.c',
  'src/volume.c',
  'src/network.c',
  'src/session.c',
]

# the safety-net icon directory (see src/picon.c): shipped with the
# panel, installed into <prefix>/share/vantage-panel/icons
pkgdatadir = get_option('prefix') / get_option('datadir') / 'vantage-panel'
panel_cargs += '-DVP_PKGDATADIR="' + pkgdatadir + '"'

exe_vantage_panel = executable('vantage-panel',
  panel_sources,
  c_args : panel_cargs,
  dependencies : panel_deps,
  install : true,
)

install_subdir('data/icons',
  install_dir : pkgdatadir,
)

# The parent project consumes this subproject's executables/variables
# (see the root meson.build); nothing is linked INTO the parent, which
# is the point: the panel stays an independent component.
