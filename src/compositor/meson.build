src_compositor = ['vt-compositor.c']

lib_vantage_compositor = static_library('vantage-compositor',
  sources : src_compositor,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep, vantage_backends_dep],
  pic : true,
)

vantage_compositor_dep = declare_dependency(
  link_with : lib_vantage_compositor,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep, vantage_backends_dep],
)
