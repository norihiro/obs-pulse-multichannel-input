Name: @PLUGIN_NAME@
Version: @VERSION@
Release: @RELEASE@%{?dist}
Summary: PulseAudio multi-channel audio source plugin for OBS Studio
License: GPLv2+

Source0: %{name}-%{version}.tar.bz2
BuildRequires: cmake, gcc, gcc-c++
BuildRequires: obs-studio-devel
BuildRequires: pulseaudio-libs-devel

%description
This plugin for OBS Studio provides a modified PulseAudio input.

%prep
%autosetup -p1

%build
%{cmake} -DLINUX_PORTABLE=OFF -DLINUX_RPATH=OFF
%{cmake_build}

%install
%{cmake_install}

%files
%{_libdir}/obs-plugins/%{name}.so
%{_datadir}/obs/obs-plugins/%{name}/
