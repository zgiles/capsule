# Version is passed in from the Makefile via:
#   rpmbuild --define "capsule_version X.Y.Z"
# Fall back to 1.0.0 if building the spec directly.
%{!?capsule_version: %global capsule_version 1.0.0}

Name:           capsule
Version:        %{capsule_version}
Release:        1%{?dist}
Summary:        Capability-based network namespace executor

License:        MIT
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  libcap-devel
Requires:       libcap
Requires:       iproute
Requires:       wireguard-tools
Requires(post): libcap
Requires(post): systemd

%description
capsule allows an unprivileged user to execute a program inside a
pre-existing Linux network namespace, without sudo, by using a binary
installed with the cap_sys_admin file capability.

The binary uses setns(2) to enter the network namespace, then immediately
drops all capabilities via libcap before exec, so the child process runs
fully unprivileged with the original caller's UID/GID and the unmodified
environment.


%prep
%setup -q


%build
make %{?_smp_mflags}


%install
make install DESTDIR=%{buildroot} PREFIX=/usr


%post
setcap 'cap_sys_admin,cap_dac_read_search+ep' %{_bindir}/capsule
%systemd_post capsule-netns@.service


%preun
%systemd_preun capsule-netns@.service
if [ $1 -eq 0 ]; then
    setcap -r %{_bindir}/capsule 2>/dev/null || true
fi


%postun
%systemd_postun_with_restart capsule-netns@.service


%files
%{_bindir}/capsule
%{_bindir}/capsule-netns-up
%{_bindir}/capsule-netns-down
%{_unitdir}/capsule-netns@.service
%{_unitdir}/capsule-namespaces.target
%{_prefix}/lib/systemd/system-generators/capsule-netns-generator
%dir %attr(0750,root,root) %{_sysconfdir}/capsule
%{_mandir}/man1/capsule.1*


%changelog
* Sun Feb 23 2026 capsule packager <capsule@example.com> - 1.0.0-1
- Initial release
