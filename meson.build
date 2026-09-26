project('vantage', 'c',
  version : '0.2.0',
  default_options : [
    'c_std=c17',
    'warning_level=2',
    'b_ndebug=false',
    'b_lto=true',
    'default_library=both',
  ],
  license : 'LicenseRef-Vantage-Proprietary',
  meson_version : '>= 0.59',
)

cc = meson.get_compiler('c')

# Project version (may be patched at distro time)
vt_version       = meson.project_version()
vt_api_version   = '0.1'
vt_prefix        = get_option('prefix')
vt_datadir       = join_paths(vt_prefix, get_option('datadir'), 'vantage')
vt_sysconfdir    = join_paths(get_option('sysconfdir'), 'vantage')
vt_libdir        = join_paths(vt_prefix, get_option('libdir'))
vt_includedir    = join_paths(vt_prefix, get_option('includedir'), 'vantage-' + vt_api_version)
vt_bindir        = join_paths(vt_prefix, get_option('bindir'))
vt_xdg_autostart = join_paths(vt_prefix, get_option('datadir'), 'xsessions')

# Optional extra RUNPATH baked into INSTALLED executables, for
# user-prefix builds where runtime libraries live outside the system
# paths (e.g. distro-in-a-sandbox with dev packages extracted into a
# private prefix). Configure with:
#   VT_EXTRA_RPATH=/path/to/libs meson setup build ...
# When unset this is an empty string and installed binaries get no
# extra rpath (the normal distro case: libraries live in system dirs).
vt_extra_rpath = run_command('printenv', 'VT_EXTRA_RPATH',
                             check : false).stdout().strip()

add_project_arguments([
  '-D_GNU_SOURCE',
  '-DVT_COMPILATION',
  '-DVT_VERSION="' + vt_version + '"',
  '-DVT_DATADIR="' + vt_datadir + '"',
  '-DVT_SYSCONFDIR="' + vt_sysconfdir + '"',
  '-DVT_LIBDIR="' + vt_libdir + '"',
  '-DVT_BINDIR="' + vt_bindir + '"',
  '-DVT_PREFIX="' + vt_prefix + '"',
], language : 'c')

# Common warning flags
common_cflags = [
  '-Wall', '-Wextra', '-Wpedantic',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wshadow',
  '-Wwrite-strings',
  '-Wconversion',
  '-Wno-error=conversion',
  '-Wno-sign-conversion',
  '-fvisibility=hidden',
]
add_project_arguments(common_cflags, language : 'c')

# Dependency detection -- everything optional is optional
dep_libm     = cc.find_library('m', required : false)
dep_librt    = cc.find_library('rt', required : false)
dep_libdl    = cc.find_library('dl', required : false)
dep_libpthread = cc.find_library('pthread', required : false)

# Core deps that are mandatory
dep_wayland_protocols = dependency('wayland-protocols', required : false)
dep_wayland_scanner   = dependency('wayland-scanner', required : false, native : true)
prog_wl_scanner = find_program('wayland-scanner', required : false)

# Backend deps -- all optional, runtime-detected
dep_wayland_client  = dependency('wayland-client',  required : false)
dep_wayland_server  = dependency('wayland-server',  required : false)
dep_x11             = dependency('x11',            required : false)
dep_xcb             = dependency('xcb',            required : false)
dep_xcb_randr       = dependency('xcb-randr',      required : false)
dep_xcb_xinput      = dependency('xcb-input',      required : false)
dep_xcb_util        = dependency('xcb-util',       required : false)
dep_xcb_ewmh        = dependency('xcb-ewmh',       required : false)
dep_xcb_icccm       = dependency('xcb-icccm',      required : false)
dep_xrender         = dependency('xrender',        required : false)
dep_xcomposite      = dependency('xcomposite',     required : false)
dep_xdamage         = dependency('xdamage',        required : false)
dep_xfixes          = dependency('xfixes',         required : false)
dep_xtst            = dependency('xtst',           required : false)
dep_xrandr          = dependency('xrandr',         required : false)
dep_xinerama        = dependency('xinerama',       required : false)
dep_xcursor         = dependency('xcursor',        required : false)
dep_xres            = dependency('xres',          required : false)
dep_xft             = dependency('xft',            required : false)
dep_freetype        = dependency('freetype2',     required : false)
dep_fontconfig      = dependency('fontconfig',    required : false)
dep_xi              = dependency('xi',             required : false)
dep_x11xcb          = dependency('x11-xcb',        required : false)
dep_xkbcommon       = dependency('xkbcommon',      required : false)
dep_xkbcommon_x11   = dependency('xkbcommon-x11',  required : false)

# Renderer deps -- optional
dep_egl             = dependency('egl',            required : false)
dep_glesv2          = dependency('glesv2',         required : false)
dep_gl              = dependency('gl',             required : false)
dep_opengl          = dependency('opengl',         required : false)
dep_gbm             = dependency('gbm',            required : false)
dep_libdrm          = dependency('libdrm',         required : false)
dep_udev            = dependency('libudev',       required : false)
dep_libseat         = dependency('libseat',        required : false)
dep_libinput        = dependency('libinput',       required : false)
dep_vulkan          = dependency('vulkan',         required : false)

# Wallpaper video pipeline -- optional
dep_libavcodec      = dependency('libavcodec',      required : false)
dep_libavformat     = dependency('libavformat',    required : false)
dep_libavutil       = dependency('libavutil',      required : false)
dep_libswscale      = dependency('libswscale',     required : false)
dep_libswresample   = dependency('libswresample', required : false)

# Image loading for wallpapers
dep_gdkpixbuf       = dependency('gdk-pixbuf-2.0', required : false)
dep_libpng          = dependency('libpng',         required : false)
dep_libjpeg         = dependency('libjpeg',         required : false)

# Audio
dep_alsa            = dependency('alsa',           required : false)
dep_pulse           = dependency('libpulse',      required : false)
dep_pipewire        = dependency('libpipewire-0.3', required : false)

# Network
dep_nm             = dependency('libnm',           required : false)

# Power / init system
dep_upower         = dependency('upower-glib',     required : false)
dep_systemd        = dependency('libsystemd',      required : false)
dep_elogind        = dependency('libelogind',     required : false)
dep_dbus           = dependency('dbus-1',          required : false)
dep_gio            = dependency('glib-2.0',       required : false)

# JSON parsing (for theme files)
dep_jsonc          = dependency('json-c',         required : false)

# Configuration summary
summary({
  'prefix'         : vt_prefix,
  'c_std'          : get_option('c_std'),
  'wayland-server' : dep_wayland_server.found(),
  'wayland-client' : dep_wayland_client.found(),
  'x11'            : dep_x11.found() or dep_xcb.found(),
  'egl'            : dep_egl.found(),
  'opengl'         : (dep_gl.found() or dep_glesv2.found() or dep_opengl.found()),
  'vulkan'         : dep_vulkan.found(),
  'gbm'            : dep_gbm.found(),
  'libdrm'         : dep_libdrm.found(),
  'libseat'        : dep_libseat.found(),
  'libinput'       : dep_libinput.found(),
  'libudev'        : dep_udev.found(),
  'ffmpeg (video wallpaper)' : dep_libavcodec.found(),
  'gdk-pixbuf'     : dep_gdkpixbuf.found(),
  'json-c'         : dep_jsonc.found(),
  'dbus-1'         : dep_dbus.found(),
  'libsystemd'     : dep_systemd.found(),
  'libelogind'     : dep_elogind.found(),
  'upower'         : dep_upower.found(),
  'pipewire'       : dep_pipewire.found(),
  'pulseaudio'     : dep_pulse.found(),
  'alsa'           : dep_alsa.found(),
  'NetworkManager' : dep_nm.found(),
}, section : 'Vantage build configuration')

# Subdirs
subdir('include')
subdir('src')
subdir('tests')
subdir('data')
subdir('docs')
subdir('packaging')
