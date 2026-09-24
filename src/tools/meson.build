# Tool binaries
tool_deps = [
  vantage_core_dep, vantage_config_dep, vantage_ipc_dep,
  vantage_renderer_dep, vantage_gpu_dep, vantage_backends_dep,
  vantage_compositor_dep, vantage_wm_dep, vantage_session_dep,
  vantage_panel_dep, vantage_desktop_dep, vantage_wallpaper_dep,
  vantage_theme_dep, vantage_settings_dep, vantage_input_dep,
  vantage_integrations_dep,
]

executable('vantage-session',
  sources : 'vantage-session.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-wm',
  sources : 'vantage-wm.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-panel',
  sources : 'vantage-panel.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-desktop',
  sources : 'vantage-desktop.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-settings',
  sources : 'vantage-settings.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-config',
  sources : 'vantage-config.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-renderer',
  sources : 'vantage-renderer.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-theme',
  sources : 'vantage-theme.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-diagnostics',
  sources : 'vantage-diagnostics.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)

executable('vantage-remote',
  sources : 'vantage-remote.c',
  dependencies : tool_deps,
  install : true,
  install_rpath : vt_extra_rpath,
)
