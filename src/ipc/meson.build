src_ipc = ['vt-ipc.c']

lib_vantage_ipc = static_library('vantage-ipc',
  sources : src_ipc,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
  pic : true,
)

vantage_ipc_dep = declare_dependency(
  link_with : lib_vantage_ipc,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
)
