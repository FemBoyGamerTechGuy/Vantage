src_compositor = ['vt-compositor.c']
comp_deps = [vantage_core_dep, vantage_renderer_dep, vantage_backends_dep]
comp_cargs = []

if dep_x11.found() and dep_xcomposite.found() and dep_xdamage.found() and \
   dep_xrender.found() and dep_xfixes.found()
  src_compositor += ['vt-compositor-x11.c']
  comp_deps += [dep_x11, dep_xcomposite, dep_xdamage, dep_xrender, dep_xfixes]
  comp_cargs += ['-DVT_HAVE_X11', '-DVT_HAVE_XCOMPOSITE',
                 '-DVT_HAVE_XDAMAGE', '-DVT_HAVE_XRENDER',
                 '-DVT_HAVE_XFIXES']
endif

lib_vantage_compositor = static_library('vantage-compositor',
  sources : src_compositor,
  include_directories : vantage_includes,
  dependencies : comp_deps,
  c_args : comp_cargs,
  pic : true,
)

vantage_compositor_dep = declare_dependency(
  link_with : lib_vantage_compositor,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep, vantage_backends_dep],
)
