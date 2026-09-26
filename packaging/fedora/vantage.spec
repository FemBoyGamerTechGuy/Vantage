Name:       vantage
Version:    0.2.0
Release:    1%{?dist}
Summary:    Lightweight raw-C Linux desktop environment
License:    LicenseRef-Vantage-Proprietary
URL:        https://github.com/FemBoyGamerTechGuy/Vantage
Source0:    %{url}/archive/v%{version}.tar.gz

BuildRequires: meson, ninja-build, gcc, pkgconfig
BuildRequires: pkgconfig(xcb), pkgconfig(xcb-randr)
BuildRequires: pkgconfig(xkbcommon), pkgconfig(xkbcommon-x11)
BuildRequires: pkgconfig(wayland-client), pkgconfig(wayland-protocols)
BuildRequires: pkgconfig(egl), pkgconfig(gl), pkgconfig(glesv2)
BuildRequires: pkgconfig(gbm), pkgconfig(libdrm), pkgconfig(libudev)
BuildRequires: pkgconfig(json-c), pkgconfig(gdk-pixbuf-2.0)
BuildRequires: pkgconfig(libpng), pkgconfig(libjpeg)
BuildRequires: pkgconfig(libavcodec), pkgconfig(libavformat), pkgconfig(libavutil), pkgconfig(libswscale)
BuildRequires: pkgconfig(libpipewire-0.3), pkgconfig(libpulse), pkgconfig(alsa)
BuildRequires: pkgconfig(libnm), pkgconfig(upower-glib)
BuildRequires: pkgconfig(dbus-1), pkgconfig(libsystemd)

Requires: %{name}-core%{?_isa} = %{version}-%{release}

%description
Vantage is a lightweight Linux desktop environment written primarily in C,
inspired by XFCE but implemented from scratch. It supports native Wayland,
X11 (Xorg/XLibre) backends, hardware-accelerated rendering (NVIDIA/AMD/Intel),
live video wallpapers, cross-toolkit Qt6/GTK theming, and runs without D-Bus
or systemd if they are not available.

%package core
Summary: Core libraries for Vantage
%description core
Internal libraries used by Vantage.

%package devel
Summary: Development files for Vantage
Requires: %{name}-core%{?_isa} = %{version}-%{release}
%description devel
Headers and pkg-config files for plugin development.

%prep
%setup -q

%build
%meson -Ddocs=disabled
%meson_build

%install
%meson_install

%check
%meson_test

%files
%license LICENSE
%doc README.md
%{_bindir}/vantage-session
%{_bindir}/vantage-wm
%{_bindir}/vantage-panel
%{_bindir}/vantage-desktop
%{_bindir}/vantage-settings
%{_bindir}/vantage-config
%{_bindir}/vantage-renderer
%{_bindir}/vantage-theme
%{_bindir}/vantage-diagnostics
%{_datadir}/xsessions/vantage.desktop
%{_datadir}/vantage/

%files core
%{_libdir}/libvantage-*.so*
%{_sysconfdir}/vantage/

%files devel
%{_includedir}/vantage-0.1/

%changelog
* Mon Jan 01 2026 Vantage Project <vantage@example.org> - 0.1.0-1
- Initial RPM release
