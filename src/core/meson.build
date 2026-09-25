# libvantage-core: foundational utilities used by every component
src_core = [
  'vt-core.c',
  'vt-paths.c',
]

lib_vantage_core = static_library('vantage-core',
  sources : src_core,
  include_directories : vantage_includes,
  dependencies : [dep_libm, dep_libpthread, dep_librt],
  install : false,
  pic : true,
)

vantage_core_dep = declare_dependency(
  link_with : lib_vantage_core,
  include_directories : vantage_includes,
  dependencies : [dep_libm, dep_libpthread, dep_librt],
)
