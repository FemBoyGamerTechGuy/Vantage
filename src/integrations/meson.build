src_integ = []
integ_deps = [vantage_core_dep]
integ_cargs = []

# Always build audio/net/power/dbus files — backends are runtime-detected
# and gracefully degrade when the underlying library is not linked.
src_integ += ['vt-integ-audio.c', 'vt-integ-network.c',
              'vt-integ-power.c', 'vt-integ-dbus.c']

if get_option('dbus').enabled() or (get_option('dbus').auto() and dep_dbus.found())
  integ_deps += [dep_dbus]
  integ_cargs += ['-DVT_HAVE_DBUS']
endif
if get_option('pipewire').enabled() or (get_option('pipewire').auto() and dep_pipewire.found())
  integ_deps += [dep_pipewire]
  integ_cargs += ['-DVT_HAVE_PIPEWIRE']
endif
if get_option('pulseaudio').enabled() or (get_option('pulseaudio').auto() and dep_pulse.found())
  integ_deps += [dep_pulse]
  integ_cargs += ['-DVT_HAVE_PULSE']
endif
if get_option('alsa').enabled() or (get_option('alsa').auto() and dep_alsa.found())
  integ_deps += [dep_alsa]
  integ_cargs += ['-DVT_HAVE_ALSA']
endif
if get_option('networkmanager').enabled() or (get_option('networkmanager').auto() and dep_nm.found())
  integ_deps += [dep_nm]
  integ_cargs += ['-DVT_HAVE_NM']
endif

lib_vantage_integrations = static_library('vantage-integrations',
  sources : src_integ,
  include_directories : vantage_includes,
  dependencies : integ_deps,
  c_args : integ_cargs,
  pic : true,
)

vantage_integrations_dep = declare_dependency(
  link_with : lib_vantage_integrations,
  include_directories : vantage_includes,
  dependencies : integ_deps,
)
