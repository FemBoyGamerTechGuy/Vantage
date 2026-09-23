src_panel = ['vt-panel.c', 'vt-panel-applets.c']

lib_vantage_panel = static_library('vantage-panel',
  sources : src_panel,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep, vantage_integrations_dep, vantage_wm_dep],
  pic : true,
)

vantage_panel_dep = declare_dependency(
  link_with : lib_vantage_panel,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep, vantage_integrations_dep, vantage_wm_dep],
)
