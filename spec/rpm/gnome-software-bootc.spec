Name:           gnome-software-bootc
Version:        0.1
Release:        2%{?dist}
Summary:        GNOME Software plugin for bootc updates and system management

License:        GPL-2.0-or-later
URL:            https://github.com/ramonmsilvabr/gnome-software-plugin-bootc
Source0:        %{url}/archive/refs/heads/main.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson >= 0.59.0
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(gnome-software) >= 45.0
BuildRequires:  pkgconfig(glib-2.0) >= 2.74.0
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gobject-2.0)
BuildRequires:  polkit-devel

Requires:       gnome-software
Requires:       bootc
Requires:       polkit

%description
This package provides a GNOME Software plugin that enables system updates 
and background update checks for systems running bootc (bootable containers).

%prep
%autosetup -n gnome-software-plugin-bootc-main

%build
%meson
%meson_build

%install
%meson_install

# 1. Install helper script
install -d -m 0755 %{buildroot}%{_libexecdir}
install -m 0755 sys-utils/gs-bootc-helper %{buildroot}%{_libexecdir}/gs-bootc-helper

# 2. Install Polkit rules
install -d -m 0755 %{buildroot}%{_datadir}/polkit-1/rules.d
install -m 0644 sys-utils/99-bootc-check.rules %{buildroot}%{_datadir}/polkit-1/rules.d/99-bootc-check.rules

# 3. Install Polkit policy action
install -d -m 0755 %{buildroot}%{_datadir}/polkit-1/actions
install -m 0644 sys-utils/org.containers.bootc.policy %{buildroot}%{_datadir}/polkit-1/actions/org.containers.bootc.policy

%files
%license LICENSE
%doc README.md

# Dynamic library for GNOME Software plugin
%{_libdir}/gnome-software/plugins-*/libgs_plugin_bootc.so

# Executable helper script
%{_libexecdir}/gs-bootc-helper

# Polkit security files
%{_datadir}/polkit-1/rules.d/99-bootc-check.rules
%{_datadir}/polkit-1/actions/org.containers.bootc.policy

%changelog
* Sat Sep 26 2026 Developer <dev@example.com> - 0.1-1
- Initial RPM release with bootc helper script and Polkit rules