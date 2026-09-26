src_wm = ['vt-wm.c']
wm_deps = [vantage_core_dep, vantage_backends_dep]
wm_cargs = []

if dep_x11.found()
  src_wm += ['vt-wm-x11.c']
  wm_deps += [dep_x11]
  wm_cargs += ['-DVT_HAVE_X11']
  if dep_xcursor.found()
    wm_deps += [dep_xcursor]
    wm_cargs += ['-DVT_HAVE_XCURSOR']
  endif
endif

lib_vantage_wm = static_library('vantage-wm',
  sources : src_wm,
  include_directories : vantage_includes,
  dependencies : wm_deps,
  c_args : wm_cargs,
  pic : true,
)

vantage_wm_dep = declare_dependency(
  link_with : lib_vantage_wm,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_backends_dep],
)
