src_wm = ['vt-wm.c']

lib_vantage_wm = static_library('vantage-wm',
  sources : src_wm,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_backends_dep],
  pic : true,
)

vantage_wm_dep = declare_dependency(
  link_with : lib_vantage_wm,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_backends_dep],
)
