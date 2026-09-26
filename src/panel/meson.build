src_panel = ['vt-panel.c', 'vt-panel-applets.c', 'vt-launcher-applet.c']
panel_deps = [vantage_core_dep, vantage_renderer_dep, vantage_ipc_dep,
              vantage_config_dep, vantage_integrations_dep,
              vantage_wallpaper_dep]
panel_cargs = []

if dep_x11.found() and dep_xft.found() and dep_xrender.found()
  panel_deps += [dep_x11, dep_xft, dep_xrender]
  panel_cargs += ['-DVT_HAVE_X11', '-DVT_HAVE_XRENDER']
endif

if dep_alsa.found()
  panel_deps += [dep_alsa]
  panel_cargs += ['-DVT_HAVE_ALSA']
endif

lib_vantage_panel = static_library('vantage-panel',
  sources : src_panel,
  include_directories : vantage_includes,
  dependencies : panel_deps,
  c_args : panel_cargs,
  pic : true,
)

vantage_panel_dep = declare_dependency(
  link_with : lib_vantage_panel,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep],
)
