src_input = ['vt-input.c']

input_deps = [vantage_core_dep]
input_cargs = []

if dep_xkbcommon.found()
  input_deps += [dep_xkbcommon]
  input_cargs += ['-DVT_HAVE_XKBCOMMON']
endif

lib_vantage_input = static_library('vantage-input',
  sources : src_input,
  include_directories : vantage_includes,
  dependencies : input_deps,
  c_args : input_cargs,
  pic : true,
)

vantage_input_dep = declare_dependency(
  link_with : lib_vantage_input,
  include_directories : vantage_includes,
  dependencies : input_deps,
)
