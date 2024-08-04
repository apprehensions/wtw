.POSIX:

VERSION = 0

PREFIX = /usr/local

PKG_CONFIG = pkg-config

PKGS = wayland-client fcft pixman-1
INCS = `$(PKG_CONFIG) --cflags $(PKGS)`
LIBS = `$(PKG_CONFIG) --libs $(PKGS)`

TWCPPFLAGS = -D_GNU_SOURCE -DVERSION=\"$(VERSION)\"
TWCFLAGS   = -pedantic -Wall $(INCS) $(TWCPPFLAGS) $(CPPFLAGS) $(CFLAGS)
LDLIBS     = $(LIBS)

all: wtw

.c.o:
	$(CC) -o $@ -c $(TWCFLAGS) -c $<

wtw.o: xdg-shell-protocol.h wlr-layer-shell-unstable-v1-protocol.h

wtw: wtw.o xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o
	$(CC) $(LDFLAGS) -o $@ wtw.o xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o $(LDLIBS)

WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code wlr-layer-shell-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) client-header wlr-layer-shell-unstable-v1.xml $@

clean:
	rm -f wtw *.o *-protocol.*

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f wtw $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/wtw

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wtw
	
.PHONY: all clean install uninstall
