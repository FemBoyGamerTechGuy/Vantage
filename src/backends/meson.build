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
primary_selection_xml = join_paths(wl_protocols_dir, 'unstable',
  'primary-selection', 'primary-selection-unstable-v1.xml')
# foot/kitty/GTK4-needed staging protocols: server-side cursors
# (cursor-shape-v1), focus/urgency (xdg-activation-v1), HiDPI
# negotiation (fractional-scale-v1), taskbar icons
# (xdg-toplevel-icon-v1)
cursor_shape_xml = join_paths(wl_protocols_dir, 'staging',
  'cursor-shape', 'cursor-shape-v1.xml')
xdg_activation_xml = join_paths(wl_protocols_dir, 'staging',
  'xdg-activation', 'xdg-activation-v1.xml')
fractional_scale_xml = join_paths(wl_protocols_dir, 'staging',
  'fractional-scale', 'fractional-scale-v1.xml')
toplevel_icon_xml = join_paths(wl_protocols_dir, 'staging',
  'xdg-toplevel-icon', 'xdg-toplevel-icon-v1.xml')
# cursor-shape-v1's generated code references the tablet-tool interface
# (get_tablet_tool_v2) — link the tablet-unstable-v2 generated code too
# or the interface symbol stays undefined at link time
tablet_v2_xml = join_paths(wl_protocols_dir, 'unstable',
  'tablet', 'tablet-unstable-v2.xml')
# wp_viewporter: clients combine it with fractional-scale; with our
# constant integer scale of 1.0 the viewport is always the identity
viewporter_xml = join_paths(wl_protocols_dir, 'stable',
  'viewporter', 'viewporter.xml')
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

  # zwp_primary_selection_device_manager_v1 — the PRIMARY selection
  # (X11-style middle-click paste). foot/kitty/GTK terminals drive their
  # selection clipboard through it; without the global they warn and
  # lose selection-paste, and some builds fail outright.
  primary_selection_proto_h = custom_target('primary-selection-protocol.h',
    input: primary_selection_xml,
    output: 'primary-selection-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  primary_selection_proto_c = custom_target('primary-selection-protocol.c',
    input: primary_selection_xml,
    output: 'primary-selection-protocol.c',
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

  cursor_shape_proto_h = custom_target('cursor-shape-protocol.h',
    input: cursor_shape_xml,
    output: 'cursor-shape-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  cursor_shape_proto_c = custom_target('cursor-shape-protocol.c',
    input: cursor_shape_xml,
    output: 'cursor-shape-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  xdg_activation_proto_h = custom_target('xdg-activation-protocol.h',
    input: xdg_activation_xml,
    output: 'xdg-activation-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  xdg_activation_proto_c = custom_target('xdg-activation-protocol.c',
    input: xdg_activation_xml,
    output: 'xdg-activation-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  fractional_scale_proto_h = custom_target('fractional-scale-protocol.h',
    input: fractional_scale_xml,
    output: 'fractional-scale-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  fractional_scale_proto_c = custom_target('fractional-scale-protocol.c',
    input: fractional_scale_xml,
    output: 'fractional-scale-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  toplevel_icon_proto_h = custom_target('xdg-toplevel-icon-protocol.h',
    input: toplevel_icon_xml,
    output: 'xdg-toplevel-icon-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  toplevel_icon_proto_c = custom_target('xdg-toplevel-icon-protocol.c',
    input: toplevel_icon_xml,
    output: 'xdg-toplevel-icon-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  tablet_v2_proto_c = custom_target('tablet-v2-protocol.c',
    input: tablet_v2_xml,
    output: 'tablet-v2-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  viewporter_proto_h = custom_target('viewporter-protocol.h',
    input: viewporter_xml,
    output: 'viewporter-protocol.h',
    command: [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  viewporter_proto_c = custom_target('viewporter-protocol.c',
    input: viewporter_xml,
    output: 'viewporter-protocol.c',
    command: [prog_wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  backend_deps += [dep_wayland_client, dep_wayland_server, dep_xkbcommon]
  backend_cargs += ['-DVT_HAVE_WAYLAND']
  lib_vantage_backends_extra_sources = [
    xdg_shell_proto_c, xdg_shell_proto_h,
    xdg_decoration_proto_c, xdg_decoration_proto_h,
    layer_shell_proto_c, layer_shell_proto_h,
    xwayland_shell_proto_c, xwayland_shell_proto_h,
    primary_selection_proto_c, primary_selection_proto_h,
    cursor_shape_proto_c, cursor_shape_proto_h,
    xdg_activation_proto_c, xdg_activation_proto_h,
    fractional_scale_proto_c, fractional_scale_proto_h,
    toplevel_icon_proto_c, toplevel_icon_proto_h,
    tablet_v2_proto_c,
    viewporter_proto_c, viewporter_proto_h,
    'vt-wl-layer.c', 'vt-wl-xwayland.c', 'vt-wl-protocols.c']

  # Xwayland window management needs xcb (+icccm for property types).
  # xcb-composite: the WM-side CompositeRedirectSubwindows(root, MANUAL)
  # is what makes Xwayland give each X window a wl_surface — without it
  # X11 apps under the Wayland session render NOTHING (windows are
  # X-side bookkeeping only, invisible on screen).
  if dep_xcb.found() and dep_xcb_icccm.found()
    backend_deps += [dep_xcb, dep_xcb_icccm]
    backend_cargs += ['-DVT_HAVE_XCB_XWAYLAND']
    if dep_xcb_composite.found()
      backend_deps += [dep_xcb_composite]
      backend_cargs += ['-DVT_HAVE_XCB_COMPOSITE']
    endif
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
