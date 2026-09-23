src_theme = ['vt-theme.c']

theme_deps = [vantage_core_dep]
theme_cargs = []

if dep_jsonc.found()
  theme_deps += [dep_jsonc]
  theme_cargs += ['-DVT_HAVE_JSONC']
endif

lib_vantage_theme = static_library('vantage-theme',
  sources : src_theme,
  include_directories : vantage_includes,
  dependencies : theme_deps,
  c_args : theme_cargs,
  pic : true,
)

vantage_theme_dep = declare_dependency(
  link_with : lib_vantage_theme,
  include_directories : vantage_includes,
  dependencies : theme_deps,
)
