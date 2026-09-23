src_session = ['vt-session.c']

lib_vantage_session = static_library('vantage-session',
  sources : src_session,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_config_dep],
  pic : true,
)

vantage_session_dep = declare_dependency(
  link_with : lib_vantage_session,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_config_dep],
)
