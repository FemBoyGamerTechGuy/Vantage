src_desktop = ['vt-desktop.c']
desktop_deps = [vantage_core_dep, vantage_renderer_dep, vantage_wallpaper_dep,
                vantage_config_dep, vantage_ipc_dep]
desktop_cargs = []

if dep_x11.found() and dep_xft.found() and dep_xrender.found()
  desktop_deps += [dep_x11, dep_xft, dep_xrender]
  desktop_cargs += ['-DVT_HAVE_X11']
endif

lib_vantage_desktop = static_library('vantage-desktop',
  sources : src_desktop,
  include_directories : vantage_includes,
  dependencies : desktop_deps,
  c_args : desktop_cargs,
  pic : true,
)

vantage_desktop_dep = declare_dependency(
  link_with : lib_vantage_desktop,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep, vantage_renderer_dep],
)
