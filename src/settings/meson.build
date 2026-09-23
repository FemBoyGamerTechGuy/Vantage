src_settings = ['vt-settings.c', 'vt-settings-modules.c']

lib_vantage_settings = static_library('vantage-settings',
  sources : src_settings,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_config_dep, vantage_theme_dep],
  pic : true,
)

vantage_settings_dep = declare_dependency(
  link_with : lib_vantage_settings,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_config_dep, vantage_theme_dep],
)
