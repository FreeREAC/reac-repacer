# SPDX-License-Identifier: GPL-3.0-or-later
# reac-repacer — REAC Wi-Fi de-jitter re-pacer, Fedora build (no ubus).
%global debug_package %{nil}
Name:           reac-repacer
Version:        0.2.3
Release:        1%{?dist}
Summary:        REAC Wi-Fi de-jitter re-pacer (transparent buffer + re-clock)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/reac-repacer
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  systemd-rpm-macros
Requires:       iproute

%description
reac-repacer sits in the REAC signal path and removes Wi-Fi jitter: it buffers
the stream, re-clocks it from a free-running media clock, conceals gaps, and
re-locks to live sample-rate changes (44.1/48/96 kHz). It is a single
self-contained C program; this Fedora build omits the OpenWrt ubus management
interface (built without -DHAVE_UBUS). Needs iproute (tc/ip) at runtime.

%prep
%autosetup -n %{name}-%{version}

%build
# Production ring size (the source ships the larger test ring).
sed -i 's/^#define RING_BITS 14 .*/#define RING_BITS 11   \/* prod ring *\//' tools/reac_repacer.c
cc %{optflags} -std=c11 -pthread -o reac-repacer tools/reac_repacer.c -lm

%install
install -Dm0755 reac-repacer %{buildroot}%{_bindir}/reac-repacer
install -Dm0644 packaging/reac-repacer.service %{buildroot}%{_unitdir}/reac-repacer.service
install -Dm0644 packaging/reac-repacer.conf     %{buildroot}%{_sysconfdir}/reac-repacer/reac-repacer.conf
install -Dm0644 openwrt/reac-repacer/files/reac-repacer.8 %{buildroot}%{_mandir}/man8/reac-repacer.8

%files
%license LICENSE
%doc README.md
%{_bindir}/reac-repacer
%{_unitdir}/reac-repacer.service
%dir %{_sysconfdir}/reac-repacer
%config(noreplace) %{_sysconfdir}/reac-repacer/reac-repacer.conf
%{_mandir}/man8/reac-repacer.8*

%post
%systemd_post reac-repacer.service
%preun
%systemd_preun reac-repacer.service
%postun
%systemd_postun_with_restart reac-repacer.service

%changelog
* Mon Jun 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.3-1
- Fedora package: standalone REAC de-jitter re-pacer (no ubus).
