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
