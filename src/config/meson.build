# libvantage-config
src_config = ['vt-config.c']

lib_vantage_config = static_library('vantage-config',
  sources : src_config,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
  pic : true,
)

vantage_config_dep = declare_dependency(
  link_with : lib_vantage_config,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
)
