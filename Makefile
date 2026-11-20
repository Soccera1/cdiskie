CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = gio-unix-2.0 yaml-0.1
DESKTOP ?= $(shell $(PKG_CONFIG) --exists gtk+-3.0 libnotify && echo 1 || echo 0)
ifeq ($(DESKTOP),1)
PACKAGES += gtk+-3.0 libnotify
CPPFLAGS += -DWITH_DESKTOP
INDICATOR ?= $(shell $(PKG_CONFIG) --exists ayatana-appindicator3-0.1 && echo 1 || echo 0)
ifeq ($(INDICATOR),1)
PACKAGES += ayatana-appindicator3-0.1
CPPFLAGS += -DWITH_INDICATOR
endif
endif

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -MMD -MP
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))
OBJECTS = build/main.o build/config.o build/device.o build/operations.o build/desktop.o
PROGRAMS = cdiskie cdiskie-mount cdiskie-umount cdiskie-info

.PHONY: all install clean
all: $(addprefix build/,$(PROGRAMS))

build/cdiskie: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/cdiskie-mount build/cdiskie-umount build/cdiskie-info: build/cdiskie
	ln -sf cdiskie $@

build/%.o: %.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build:
	mkdir -p $@

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 build/cdiskie "$(DESTDIR)$(BINDIR)/cdiskie"
	ln -sf cdiskie "$(DESTDIR)$(BINDIR)/cdiskie-mount"
	ln -sf cdiskie "$(DESTDIR)$(BINDIR)/cdiskie-umount"
	ln -sf cdiskie "$(DESTDIR)$(BINDIR)/cdiskie-info"

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
