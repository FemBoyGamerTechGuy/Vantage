src_backends = ['vt-backend.c', 'vt-backend-x11.c', 'vt-backend-wayland.c',
                'vt-seat.c', 'vt-kms.c']

backend_deps = [vantage_core_dep, vantage_ipc_dep,
                vantage_integrations_dep, vantage_wallpaper_dep]
backend_cargs = []

if dep_x11.found()
  backend_deps += [dep_x11]
  backend_cargs += ['-DVT_HAVE_X11']
  if dep_xrandr.found()
    backend_deps += [dep_xrandr]
    backend_cargs += ['-DVT_HAVE_XRANDR']
  endif
  if dep_xi.found()
    backend_deps += [dep_xi]
    backend_cargs += ['-DVT_HAVE_XINPUT']
  endif
endif

# Wayland compositor backend + generated protocol code
wl_protocols_dir = dep_wayland_protocols.get_variable(
  pkgconfig: 'pkgdatadir',
  default_value: dep_wayland_protocols.get_variable(
    pkgconfig: 'wayland_protocols_path', default_value: '/usr/share/wayland-protocols'))
xdg_shell_xml = join_paths(wl_protocols_dir, 'stable', 'xdg-shell', 'xdg-shell.xml')
xdg_decoration_xml = join_paths(wl_protocols_dir, 'unstable',
  'xdg-decoration', 'xdg-decoration-unstable-v1.xml')
# vendored protocol XMLs (repo-root protocols/):
#   wlr-layer-shell (MIT, wlr-protocols — panels dock through it)
#   xwayland-shell (wayland-protocols staging — X11 window association)
protocols_dir = join_paths(meson.project_source_root(), 'protocols')
layer_shell_xml = join_paths(protocols_dir, 'wlr-layer-shell-unstable-v1.xml')
xwayland_shell_xml = join_paths(protocols_dir, 'xwayland-shell-v1.xml')

if dep_wayland_server.found() and dep_wayland_client.found() and \
   prog_wl_scanner.found() and run_command('test', '-f', xdg_shell_xml, check: false).returncode() == 0
  xdg_shell_proto_h = custom_target('xdg-shell-protocol.h',
    input: xdg_shell_xml,
    output: 'xdg-shell-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  xdg_shell_proto_c = custom_target('xdg-shell-protocol.c',
    input: xdg_shell_xml,
    output: 'xdg-shell-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  # xdg-decoration-unstable-v1: CSD apps tell us whether they draw
  # their own frame — without it a decorating compositor would
  # double-decorate every GTK/Chromium window
  xdg_decoration_proto_h = custom_target('xdg-decoration-protocol.h',
    input: xdg_decoration_xml,
    output: 'xdg-decoration-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  xdg_decoration_proto_c = custom_target('xdg-decoration-protocol.c',
    input: xdg_decoration_xml,
    output: 'xdg-decoration-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  # zwlr_layer_shell_v1 — docked shell surfaces (the panel)
  layer_shell_proto_h = custom_target('wlr-layer-shell-protocol.h',
    input: layer_shell_xml,
    output: 'wlr-layer-shell-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  layer_shell_proto_c = custom_target('wlr-layer-shell-protocol.c',
    input: layer_shell_xml,
    output: 'wlr-layer-shell-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  # xwayland_shell_v1 — explicit X11 window ↔ wl_surface association
  xwayland_shell_proto_h = custom_target('xwayland-shell-protocol.h',
    input: xwayland_shell_xml,
    output: 'xwayland-shell-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  xwayland_shell_proto_c = custom_target('xwayland-shell-protocol.c',
    input: xwayland_shell_xml,
    output: 'xwayland-shell-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  backend_deps += [dep_wayland_client, dep_wayland_server, dep_xkbcommon]
  backend_cargs += ['-DVT_HAVE_WAYLAND']
  lib_vantage_backends_extra_sources = [
    xdg_shell_proto_c, xdg_shell_proto_h,
    xdg_decoration_proto_c, xdg_decoration_proto_h,
    layer_shell_proto_c, layer_shell_proto_h,
    xwayland_shell_proto_c, xwayland_shell_proto_h,
    'vt-wl-layer.c', 'vt-wl-xwayland.c']

  # Xwayland window management needs xcb (+icccm for property types)
  if dep_xcb.found() and dep_xcb_icccm.found()
    backend_deps += [dep_xcb, dep_xcb_icccm]
    backend_cargs += ['-DVT_HAVE_XCB_XWAYLAND']
  endif
else
  lib_vantage_backends_extra_sources = []
  if dep_wayland_client.found()
    backend_deps += [dep_wayland_client]
    backend_cargs += ['-DVT_HAVE_WAYLAND']
  endif
endif

# KMS/seat/input support for the native Wayland compositor
if dep_libdrm.found()
  backend_deps += [dep_libdrm]
  backend_cargs += ['-DVT_HAVE_LIBDRM']
endif
if dep_gbm.found()
  backend_deps += [dep_gbm]
  backend_cargs += ['-DVT_HAVE_GBM']
endif
if dep_egl.found()
  backend_deps += [dep_egl]
  backend_cargs += ['-DVT_HAVE_EGL']
  if dep_glesv2.found()
    backend_deps += [dep_glesv2]
    backend_cargs += ['-DVT_HAVE_GLES']
  endif
endif
if dep_libseat.found()
  backend_deps += [dep_libseat]
  backend_cargs += ['-DVT_HAVE_LIBSEAT']
endif
if dep_libinput.found() and dep_udev.found()
  backend_deps += [dep_libinput, dep_udev]
  backend_cargs += ['-DVT_HAVE_LIBINPUT']
endif
if dep_xcursor.found()
  backend_deps += [dep_xcursor]
  backend_cargs += ['-DVT_HAVE_XCURSOR']
endif
if dep_xkbcommon.found()
  backend_cargs += ['-DVT_HAVE_XKBCOMMON']
endif
# SSD title text (real glyphs, not placeholder blocks)
if dep_freetype.found() and dep_fontconfig.found()
  backend_deps += [dep_freetype, dep_fontconfig]
  backend_cargs += ['-DVT_HAVE_FREETYPE']
endif

lib_vantage_backends = static_library('vantage-backends',
  sources : src_backends + lib_vantage_backends_extra_sources,
  include_directories : [vantage_includes, include_directories('.')],
  dependencies : backend_deps,
  c_args : backend_cargs,
  pic : true,
)

vantage_backends_dep = declare_dependency(
  link_with : lib_vantage_backends,
  include_directories : [vantage_includes, include_directories('.')],
  dependencies : [vantage_core_dep, backend_deps],
)
