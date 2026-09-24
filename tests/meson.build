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
  x11_testclient = executable('vt-x11-testclient',
    sources : 'vt-x11-testclient.c',
    dependencies : tests_deps + [dep_x11],
    install : true,
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
  test('harness-xvfb', harness_xvfb,
    args : [meson.build_root()],
    timeout : 90,
    suite : 'integration')
endif

harness_wayland = find_program('harness-wayland.sh', required : false)
if harness_wayland.found() and wl_client_ok and \
   dep_wayland_server.found() and get_option('tests').enabled()
  test('harness-wayland', harness_wayland,
    args : [meson.build_root()],
    timeout : 60,
    suite : 'integration')
endif

# Install the harnesses next to the docs so users can run them anywhere
install_data('harness-xvfb.sh', 'harness-wayland.sh',
  install_dir : join_paths(vt_datadir, 'tests'))
