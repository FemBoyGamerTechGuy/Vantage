src_renderer = ['vt-renderer.c', 'vt-renderer-sw.c']

renderer_deps = [vantage_core_dep]
renderer_cargs = ['-DVT_COMPILATION']

if dep_egl.found() and (dep_glesv2.found() or dep_gl.found() or dep_opengl.found())
  src_renderer += 'vt-renderer-gl.c'
  renderer_deps += [dep_egl]
  renderer_cargs += ['-DVT_HAVE_EGL']
  if dep_glesv2.found()
    renderer_deps += [dep_glesv2]
    renderer_cargs += ['-DVT_HAVE_GLESV2']
  elif dep_gl.found()
    renderer_deps += [dep_gl]
    renderer_cargs += ['-DVT_HAVE_OPENGL']
  elif dep_opengl.found()
    renderer_deps += [dep_opengl]
    renderer_cargs += ['-DVT_HAVE_OPENGL']
  endif
endif

lib_vantage_renderer = static_library('vantage-renderer',
  sources : src_renderer,
  include_directories : vantage_includes,
  dependencies : renderer_deps,
  c_args : renderer_cargs,
  pic : true,
)

vantage_renderer_dep = declare_dependency(
  link_with : lib_vantage_renderer,
  include_directories : vantage_includes,
  dependencies : renderer_deps,
  sources : [],  # c_args propagated manually by clients via -d
)
