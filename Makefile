CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -D_GNU_SOURCE \
          -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
LDFLAGS = -lcap -pie -Wl,-z,relro,-z,now

TARGET  = capsule
SRCS    = capsule.c
VERSION = 1.0.0

PREFIX     = /usr/local
BINDIR     = $(PREFIX)/bin
MANDIR     = $(PREFIX)/share/man/man1
SYSTEMDDIR   = /etc/systemd/system
GENERATORDIR = $(PREFIX)/lib/systemd/system-generators
NETNSCONF    = /etc/capsule

# Debian architecture name — prefer dpkg-architecture, fall back to uname -m
ARCH := $(shell dpkg --print-architecture 2>/dev/null || \
            uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/;s/armv7l/armhf/')

DEB_STAGE  = build/deb/capsule_$(VERSION)_$(ARCH)
DEB_OUT    = capsule_$(VERSION)_$(ARCH).deb

RPM_TOPDIR = $(CURDIR)/build/rpm
RPM_SOURCES = $(SRCS) capsule.1 Makefile README.md capsule.spec \
              capsule-netns@.service capsule-namespaces.target \
              capsule-netns-generator \
              scripts/capsule-netns-up scripts/capsule-netns-down

.PHONY: all setcap install uninstall clean deb rpm


# ── build ─────────────────────────────────────────────────────────────────────

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── install / uninstall ───────────────────────────────────────────────────────
#
# setcap is only run during a direct `make install` (DESTDIR is empty).
# Package builds (deb/rpm) set DESTDIR to a staging directory and handle
# the capability grant themselves via postinst / %post.

# Stamp the capability on the local build tree binary for quick testing.
# Run as root or with cap_setfcap:  sudo make setcap
# Then test without installing:     ./capsule /var/run/netns/vpn <cmd>
setcap: $(TARGET)
	setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep' ./$(TARGET)
	@echo "cap_sys_admin+ep set on ./$(TARGET)"

install: $(TARGET)
	install -D -m 0755 $(TARGET)                    $(DESTDIR)$(BINDIR)/$(TARGET)
	install -D -m 0644 capsule.1                    $(DESTDIR)$(MANDIR)/capsule.1
	install -D -m 0755 scripts/capsule-netns-up       $(DESTDIR)$(BINDIR)/capsule-netns-up
	install -D -m 0755 scripts/capsule-netns-down     $(DESTDIR)$(BINDIR)/capsule-netns-down
	install -D -m 0644 capsule-netns@.service         $(DESTDIR)$(SYSTEMDDIR)/capsule-netns@.service
	install -D -m 0644 capsule-namespaces.target      $(DESTDIR)$(SYSTEMDDIR)/capsule-namespaces.target
	install -D -m 0755 capsule-netns-generator        $(DESTDIR)$(GENERATORDIR)/capsule-netns-generator
	install -d -m 0750 $(DESTDIR)$(NETNSCONF)
ifeq ($(DESTDIR),)
	setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep' $(BINDIR)/$(TARGET)
	systemctl daemon-reload
endif

uninstall:
	rm -f  $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f  $(DESTDIR)$(BINDIR)/capsule-netns-up
	rm -f  $(DESTDIR)$(BINDIR)/capsule-netns-down
	rm -f  $(DESTDIR)$(MANDIR)/capsule.1
	rm -f  $(DESTDIR)$(SYSTEMDDIR)/capsule-netns@.service
	rm -f  $(DESTDIR)$(SYSTEMDDIR)/capsule-namespaces.target
	rm -f  $(DESTDIR)$(GENERATORDIR)/capsule-netns-generator
ifeq ($(DESTDIR),)
	systemctl daemon-reload
endif

# ── .deb ──────────────────────────────────────────────────────────────────────

deb: $(TARGET)
	rm -rf $(DEB_STAGE)
	install -D -m 0755 $(TARGET)                    $(DEB_STAGE)/usr/local/bin/$(TARGET)
	install -D -m 0644 capsule.1                    $(DEB_STAGE)/usr/local/share/man/man1/capsule.1
	install -D -m 0755 scripts/capsule-netns-up     $(DEB_STAGE)/usr/local/bin/capsule-netns-up
	install -D -m 0755 scripts/capsule-netns-down   $(DEB_STAGE)/usr/local/bin/capsule-netns-down
	install -D -m 0644 capsule-netns@.service         $(DEB_STAGE)/etc/systemd/system/capsule-netns@.service
	install -D -m 0644 capsule-namespaces.target      $(DEB_STAGE)/etc/systemd/system/capsule-namespaces.target
	install -D -m 0755 capsule-netns-generator        $(DEB_STAGE)/usr/local/lib/systemd/system-generators/capsule-netns-generator
	install -d -m 0750 $(DEB_STAGE)/etc/capsule
	install -d $(DEB_STAGE)/DEBIAN
	{ \
	  printf 'Package: capsule\n'; \
	  printf 'Version: $(VERSION)\n'; \
	  printf 'Architecture: $(ARCH)\n'; \
	  printf 'Maintainer: capsule packager\n'; \
	  printf 'Depends: libcap2, iproute2, wireguard-tools\n'; \
	  printf 'Section: utils\n'; \
	  printf 'Priority: optional\n'; \
	  printf 'Description: capability-based network namespace executor\n'; \
	  printf ' Allows an unprivileged user to execute a program inside a\n'; \
	  printf ' pre-existing Linux network namespace via cap_sys_admin file\n'; \
	  printf ' capability. The child runs with no capabilities.\n'; \
	} > $(DEB_STAGE)/DEBIAN/control
	{ \
	  printf '#!/bin/sh\nset -e\n'; \
	  printf 'case "$$1" in\n'; \
	  printf '    configure)\n'; \
	  printf '        setcap 'cap_sys_admin,cap_dac_read_search,cap_setpcap+ep' /usr/local/bin/capsule\n'; \
	  printf '        systemctl daemon-reload || true\n'; \
	  printf '        ;;\n'; \
	  printf 'esac\n'; \
	} > $(DEB_STAGE)/DEBIAN/postinst
	{ \
	  printf '#!/bin/sh\nset -e\n'; \
	  printf 'case "$$1" in\n'; \
	  printf '    remove|purge)\n'; \
	  printf '        setcap -r /usr/local/bin/capsule 2>/dev/null || true\n'; \
	  printf '        ;;\n'; \
	  printf 'esac\n'; \
	} > $(DEB_STAGE)/DEBIAN/prerm
	chmod 0755 $(DEB_STAGE)/DEBIAN/postinst $(DEB_STAGE)/DEBIAN/prerm
	# Requires dpkg-deb >= 1.19 (Ubuntu 18.04+); use fakeroot dpkg-deb --build on older systems
	dpkg-deb --root-owner-group --build $(DEB_STAGE) $(DEB_OUT)
	@echo "Built $(DEB_OUT)"

# ── .rpm ──────────────────────────────────────────────────────────────────────

rpm: $(RPM_SOURCES) capsule.spec
	mkdir -p $(RPM_TOPDIR)/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	tar czf $(RPM_TOPDIR)/SOURCES/capsule-$(VERSION).tar.gz \
	    --transform 's,^,capsule-$(VERSION)/,' \
	    $(RPM_SOURCES)
	rpmbuild -bb \
	    --define "_topdir $(RPM_TOPDIR)" \
	    --define "capsule_version $(VERSION)" \
	    capsule.spec
	find $(RPM_TOPDIR)/RPMS -name '*.rpm' -exec cp -v {} . \;
	@echo "Built RPM"

# ── clean ─────────────────────────────────────────────────────────────────────

clean:
	rm -f $(TARGET)
	rm -f $(DEB_OUT) *.rpm
	rm -rf build/
