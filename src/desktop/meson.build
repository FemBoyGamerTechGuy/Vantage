src_desktop = ['vt-desktop.c']

lib_vantage_desktop = static_library('vantage-desktop',
  sources : src_desktop,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep],
  pic : true,
)

vantage_desktop_dep = declare_dependency(
  link_with : lib_vantage_desktop,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep],
)
