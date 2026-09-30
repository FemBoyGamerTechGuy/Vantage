tests_c_args = []
tests_deps = [
  vantage_core_dep, vantage_config_dep, vantage_ipc_dep,
  vantage_renderer_dep, vantage_gpu_dep, vantage_backends_dep,
]

test_core = executable('test-core',
  sources : 'test-core.c',
  dependencies : tests_deps,
)
test('core', test_core)

test_config = executable('test-config',
  sources : 'test-config.c',
  dependencies : tests_deps,
)
test('config', test_config)

test_ipc = executable('test-ipc',
  sources : 'test-ipc.c',
  dependencies : tests_deps,
)
test('ipc', test_ipc)

test_icons = executable('test-icons',
  sources : 'test-icons.c',
  dependencies : tests_deps + [vantage_wallpaper_dep],
)
test('icons', test_icons)

# ------------------------------------------------------------------
# Integration test clients (used by harness-xvfb.sh / harness-wayland.sh)
# ------------------------------------------------------------------
testclient_deps = [tests_deps]
testclient_cargs = []

if dep_x11.found()
  x11_testclient_deps = tests_deps + [dep_x11]
  x11_testclient_cargs = []
  if dep_xfixes.found()
    x11_testclient_deps += [dep_xfixes]
    x11_testclient_cargs += ['-DVT_HAVE_XFIXES']
  endif
  if dep_xtst.found()
    x11_testclient_deps += [dep_xtst]
    x11_testclient_cargs += ['-DVT_HAVE_XTST']
  endif
  x11_testclient = executable('vt-x11-testclient',
    sources : 'vt-x11-testclient.c',
    dependencies : x11_testclient_deps,
    c_args : x11_testclient_cargs,
    install : true,
    install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
    install_rpath : vt_extra_rpath,
  )
endif

# Wayland test client: needs wayland-client + xdg-shell client header
wl_client_ok = dep_wayland_client.found() and prog_wl_scanner.found() and \
   run_command('test', '-f', xdg_shell_xml, check: false).returncode() == 0
if wl_client_ok
  xdg_shell_client_h = custom_target('xdg-shell-client-protocol.h',
    input: xdg_shell_xml,
    output: 'xdg-shell-client-protocol.h',
    command: [prog_wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

  # xdg-decoration (SSD request mode for the resize tests)
  xdg_decoration_xml = join_paths(wl_protocols_dir, 'unstable',
                                  'xdg-decoration', 'xdg-decoration-unstable-v1.xml')
  have_xdg_decoration = run_command('test', '-f', xdg_decoration_xml,
                                    check: false).returncode() == 0
  if have_xdg_decoration
    xdg_decoration_client_h = custom_target('xdg-decoration-client-protocol.h',
      input: xdg_decoration_xml,
      output: 'xdg-decoration-client-protocol.h',
      command: [prog_wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    wl_testclient_sources = ['vt-wayland-testclient.c', xdg_shell_client_h,
                             xdg_decoration_client_h]
    wl_testclient_cargs = ['-DVT_HAVE_XDG_DECORATION']
  else
    wl_testclient_sources = ['vt-wayland-testclient.c', xdg_shell_client_h]
    wl_testclient_cargs = []
  endif

  wayland_testclient = executable('vt-wayland-testclient',
    sources : wl_testclient_sources,
    include_directories : [vantage_includes, include_directories('.')],
    dependencies : tests_deps + [dep_wayland_client],
    c_args : wl_testclient_cargs,
    install : true,
    install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
    install_rpath : vt_extra_rpath,
  )

  # Registry prober: connects as a plain client and verifies the
  # compositor advertises the staging protocols with their SPEC names
  # (wp_cursor_shape_manager_v1, xdg_activation_v1,
  # wp_fractional_scale_manager_v1, xdg_toplevel_icon_manager_v1,
  # wp_viewporter). Independent of any toolkit's own global-detection
  # bugs (foot <= 1.22 matched the wrong interface name for the
  # toplevel-icon manager — upstream fixed in 1.23).
  registry_probe = executable('vt-registry-probe',
    sources : 'registry-probe.c',
    dependencies : [dep_wayland_client],
    install : true,
    install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
    install_rpath : vt_extra_rpath,
  )

  # Frame-callback probe: animating client paced by wl_surface.frame
  # callbacks (the browser redraw model). Pins the commit→paint→done
  # cycle: a compositor that fires callbacks only on damage (or drops
  # the flush) freezes such clients after one frame.
  # GPU-buffer probe (below): full zwp_linux_dmabuf_v1 client (feedback,
  # params, create_immed, attach, frame-callback animation). Pins the
  # hardware-acceleration path end-to-end.
  dmabuf_xml = join_paths(wl_protocols_dir, 'unstable', 'linux-dmabuf',
                          'linux-dmabuf-unstable-v1.xml')
  have_dmabuf_xml = run_command('test', '-f', dmabuf_xml,
                                check: false).returncode() == 0
  if have_dmabuf_xml
    dmabuf_client_h = custom_target('linux-dmabuf-client-protocol.h',
      input: dmabuf_xml,
      output: 'linux-dmabuf-client-protocol.h',
      command: [prog_wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    dmabuf_probe = executable('vt-dmabuf-probe',
      sources : ['vt-dmabuf-probe.c', xdg_shell_client_h, dmabuf_client_h],
      include_directories : [include_directories('.')],
      dependencies : tests_deps + [dep_wayland_client],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
  endif

  # GLX probe (X11 games): full direct-context GLX pipeline against the
  # X11 session — pins "games launch" (display, visual, direct context,
  # mapping, swap) independently of any game engine.
  if dep_x11.found() and dep_gl.found()
    glx_probe = executable('vt-glx-probe',
      sources : 'vt-glx-probe.c',
      dependencies : [dep_x11, dep_gl],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
  endif

  frame_probe = executable('vt-frame-probe',
    sources : ['vt-frame-probe.c', xdg_shell_client_h],
    include_directories : [include_directories('.')],
    dependencies : tests_deps + [dep_wayland_client],
    install : true,
    install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
    install_rpath : vt_extra_rpath,
  )
endif


  # X11 browser-behavior probe: CSD window + text selection + right-click
  # context menu — pins the "menu opens, app does not freeze" contract
  # and the "no duplicate decoration around CSD" contract.
  if sub_dep_gtk4.found() and get_option('tests').enabled()
    gtk4_csd_probe = executable('gtk4-csd-menu-probe',
      sources : 'gtk4-csd-menu-probe.c',
      dependencies : [sub_dep_gtk4],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
  endif

# ------------------------------------------------------------------
# REAL-toolkit GTK4 probe: a plain GTK application (CSD headerbar,
# cairo content) the harnesses drive through the compositor's own
# input path. The synthetic vt-wayland-testclient acks configures
# blindly and ignores the configured size — GTK4 hides nothing, so
# this probe pins the COMPLETE xdg configure handshake (resize and
# maximize must actually apply), pointer scrolling, and the XWayland
# window-management paths against real-toolkit behavior.
# ------------------------------------------------------------------
if sub_dep_gtk4.found() and get_option('tests').enabled()
  gtk4_probe = executable('vt-gtk4-probe',
    sources : 'gtk4-probe.c',
    dependencies : [sub_dep_gtk4],
    install : true,
    install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
    install_rpath : vt_extra_rpath,
  )
endif

# ------------------------------------------------------------------
# Integration harnesses — boot the real session / compositor.
#   xvfb    : Xvfb + full vantage-session (wm+panel+desktop) + clients
#   wayland : vantage-wm as headless Wayland compositor + xdg-shell client
# Skipped automatically when the host lacks Xvfb / the needed binaries.
# ------------------------------------------------------------------
prog_xvfb = find_program('xvfb-run', required : false)
if not prog_xvfb.found()
  prog_xvfb = find_program('Xvfb', required : false)
endif

harness_xvfb = find_program('harness-xvfb.sh', required : false)
if harness_xvfb.found() and prog_xvfb.found() and \
   dep_x11.found() and get_option('tests').enabled()
  # integration harnesses boot real sessions — never run them in
  # parallel with each other (meson would otherwise overlap the Xvfb
  # session with the Wayland compositor and straggler accounting
  # becomes meaningless)
  test('harness-xvfb', harness_xvfb,
    args : [meson.project_build_root()],
    timeout : 180,
    suite : 'integration',
    is_parallel : false)
endif

harness_wayland = find_program('harness-wayland.sh', required : false)
if harness_wayland.found() and wl_client_ok and \
   dep_wayland_server.found() and get_option('tests').enabled()
  test('harness-wayland', harness_wayland,
    args : [meson.project_build_root()],
    timeout : 200,
    suite : 'integration',
    is_parallel : false)
endif

# REAL Wayland session self-test (tier 2): no forced headless, KMS
# required. Skips (exit 77) on machines without /dev/dri or when not run
# from a console — it NEVER fakes a pass. Run from a TTY on the target
# hardware:   ./build test   (or tests/harness-wayland-real.sh directly)
harness_wl_real = find_program('harness-wayland-real.sh', required : false)
if harness_wl_real.found() and wl_client_ok and \
   dep_wayland_server.found() and get_option('tests').enabled()
  test('harness-wayland-real', harness_wl_real,
    args : [meson.project_build_root()],
    timeout : 120,
    suite : 'integration',
    is_parallel : false)
endif

# Install the harnesses next to the docs so users can run them anywhere
install_data('harness-xvfb.sh', 'harness-wayland.sh', 'harness-wayland-real.sh', 'harness-cli.sh',
  install_dir : join_paths(vt_datadir, 'tests'))

# ------------------------------------------------------------------
# CLI / distribution-model harness: --help/--version/--wayland/--x11,
# invalid-flag handling, development-tree execution (repo-root
# symlinks) and installed-execution via a temporary staging prefix.
# ------------------------------------------------------------------
harness_cli = find_program('harness-cli.sh', required : false)
if harness_cli.found() and get_option('tests').enabled()
  test('harness-cli', harness_cli,
    args : [meson.project_build_root(), meson.project_source_root()],
    timeout : 180,
    suite : 'integration',
    is_parallel : false)
endif

  if dep_x11.found() and dep_xtst.found()
    x11_resize_probe = executable('vt-x11-resize-probe',
      sources : 'vt-x11-resize-probe.c',
      dependencies : [dep_x11, dep_xtst],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
  endif

  # generic XTest input injector (gestures against real toolkits)
  if dep_x11.found() and dep_xtst.found()
    xtest_drive = executable('vt-xtest-drive',
      sources : 'vt-xtest-drive.c',
      dependencies : [dep_x11, dep_xtst],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
    # window-tree dumper — locates probe windows / popovers for the
    # harness without external X tools (xwininfo/xdotool)
    wintree_tool = executable('vt-x11-wintree',
      sources : 'vt-x11-wintree.c',
      dependencies : [dep_x11],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
  endif

  # raw XI2 event-delivery probe (the GDK4 input path)
  if dep_x11.found() and dep_xi.found()
    xi2_probe = executable('vt-xi2-event-probe',
      sources : 'vt-xi2-event-probe.c',
      dependencies : [dep_x11, dep_xi],
      install : true,
      install_dir : join_paths(vt_libdir, 'vantage', 'tests'),
      install_rpath : vt_extra_rpath,
    )
  endif
