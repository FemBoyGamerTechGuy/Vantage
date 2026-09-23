src_gpu = ['vt-gpu.c']

lib_vantage_gpu = static_library('vantage-gpu',
  sources : src_gpu,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
  pic : true,
)

vantage_gpu_dep = declare_dependency(
  link_with : lib_vantage_gpu,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
)
