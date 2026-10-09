%global srcname linuxqq-wayland-fix

Name:           linuxqq-wayland-fix
Version:        %{_ver}
Release:        1%{?dist}
Summary:        Fix Linux QQ screen sharing, device audio and clipboard on Wayland
License:        MIT
URL:            https://github.com/SHORiN-KiWATA/%{srcname}
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  wayland-devel
Requires:       glib2
Recommends:     linuxqq
Recommends:     xdg-desktop-portal

%description
Fixes Linux QQ on Wayland: screen sharing does not work, shared device
audio is silent, and copy/paste between QQ and other apps is broken.
Open "QQ（Wayland修复版）" from the application menu.

%prep
%autosetup

%build
%make_build PREFIX=%{_prefix} LIBEXECDIR=%{_libdir}/%{srcname} VERSION=%{version} \
    CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%install
%make_install PREFIX=%{_prefix} LIBEXECDIR=%{_libdir}/%{srcname} VERSION=%{version}

%files
%license %{_datadir}/licenses/%{srcname}/LICENSE
%doc %{_docdir}/%{srcname}/README.md
%doc %{_docdir}/%{srcname}/docs
%{_bindir}/linuxqq-wayland-fix
%{_libdir}/%{srcname}/
%{_datadir}/applications/linuxqq-wayland-fix.desktop
%{_datadir}/applications/qq-screenshot-helper.desktop

%changelog
* Thu Oct 01 2026 Shorin <shorin@example.com> - %{version}-1
- See https://github.com/SHORiN-KiWATA/%{srcname}/releases
