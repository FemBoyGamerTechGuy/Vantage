src_wallpaper = ['vt-wallpaper.c', 'vt-image.c', 'vt-icons.c']

wallpaper_deps = [vantage_core_dep, vantage_renderer_dep]
wallpaper_cargs = []

if get_option('video-wallpaper').auto() and dep_libavcodec.found() and dep_libavformat.found() and dep_libavutil.found() and dep_libswscale.found()
  wallpaper_deps += [dep_libavcodec, dep_libavformat, dep_libavutil, dep_libswscale]
  wallpaper_cargs += ['-DVT_HAVE_FFMPEG']
endif

if dep_libpng.found()
  wallpaper_deps += [dep_libpng]
  wallpaper_cargs += ['-DVT_HAVE_PNG']
endif
if dep_libjpeg.found()
  wallpaper_deps += [dep_libjpeg]
  wallpaper_cargs += ['-DVT_HAVE_JPEG']
endif
if dep_gdkpixbuf.found()
  wallpaper_deps += [dep_gdkpixbuf]
  wallpaper_cargs += ['-DVT_HAVE_GDKPIXBUF']
endif

# SVG icon rendering at the exact display size (crisp theme icons)
if dep_librsvg.found() and dep_cairo.found()
  wallpaper_deps += [dep_librsvg, dep_cairo]
  wallpaper_cargs += ['-DVT_HAVE_LIBRSVG', '-DVT_HAVE_CAIRO']
endif

lib_vantage_wallpaper = static_library('vantage-wallpaper',
  sources : src_wallpaper,
  include_directories : vantage_includes,
  dependencies : wallpaper_deps,
  c_args : wallpaper_cargs,
  pic : true,
)

vantage_wallpaper_dep = declare_dependency(
  link_with : lib_vantage_wallpaper,
  include_directories : vantage_includes,
  dependencies : wallpaper_deps,
)
