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

  wayland_testclient = executable('vt-wayland-testclient',
    sources : ['vt-wayland-testclient.c', xdg_shell_client_h],
    include_directories : [vantage_includes, include_directories('.')],
    dependencies : tests_deps + [dep_wayland_client],
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
    timeout : 90,
    suite : 'integration',
    is_parallel : false)
endif

harness_wayland = find_program('harness-wayland.sh', required : false)
if harness_wayland.found() and wl_client_ok and \
   dep_wayland_server.found() and get_option('tests').enabled()
  test('harness-wayland', harness_wayland,
    args : [meson.project_build_root()],
    timeout : 60,
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
