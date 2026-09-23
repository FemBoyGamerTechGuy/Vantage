src_backends = ['vt-backend.c', 'vt-backend-x11.c', 'vt-backend-wayland.c',
                'vt-backend-xlibre.c']

backend_deps = [vantage_core_dep]
backend_cargs = []

if dep_xcb.found()
  src_backends += []  # already in the list
  backend_deps += [dep_xcb]
  backend_cargs += ['-DVT_HAVE_XCB']
  if dep_xcb_randr.found()
    backend_deps += [dep_xcb_randr]
    backend_cargs += ['-DVT_HAVE_XCB_RANDR']
  endif
endif

if dep_wayland_client.found()
  backend_deps += [dep_wayland_client]
  backend_cargs += ['-DVT_HAVE_WAYLAND']
endif

lib_vantage_backends = static_library('vantage-backends',
  sources : src_backends,
  include_directories : vantage_includes,
  dependencies : backend_deps,
  c_args : backend_cargs,
  pic : true,
)

vantage_backends_dep = declare_dependency(
  link_with : lib_vantage_backends,
  include_directories : vantage_includes,
  dependencies : backend_deps,
)
