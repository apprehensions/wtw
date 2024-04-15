.POSIX:
.SUFFIXES:

PKG_CONFIG = pkg-config

PKGS = wlroots wayland-client fcft pixman-1
INCS = `$(PKG_CONFIG) --cflags $(PKGS)`
LIBS = `$(PKG_CONFIG) --libs $(PKGS)`

WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`

CPPFLAGS =
CFLAGS   = -pedantic -Wall $(INCS) $(CPPFLAGS)
LDFLAGS  = $(LIBS)

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $<

all: wtw

wtw: wtw.o wlr-layer-shell-unstable-v1.o xdg-shell-protocol.o
	$(CC) -o $@ wtw.o wlr-layer-shell-unstable-v1.o xdg-shell-protocol.o $(LDFLAGS) $(CFLAGS)
wtw.o: wlr-layer-shell-unstable-v1-protocol.h xdg-shell-client-protocol.h

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
wlr-layer-shell-unstable-v1.c:
	$(WAYLAND_SCANNER) private-code wlr-layer-shell-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) client-header wlr-layer-shell-unstable-v1.xml $@

clean:
	rm -f wtw xdg-shell-protocol.c wlr-layer-shell-unstable-v1.c *.o *-protocol.*

.PHONY: clean
