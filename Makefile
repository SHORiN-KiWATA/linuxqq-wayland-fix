# linuxqq-wayland-fix
#
#   make
#   make install DESTDIR=... PREFIX=/usr
#
# LIBEXECDIR 下放两个注入库；启动脚本在安装时写入它的绝对路径。

NAME       := linuxqq-wayland-fix
VERSION    ?= $(or $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//'),0.0.0)

PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/$(NAME)
DATADIR    ?= $(PREFIX)/share
DOCDIR     ?= $(DATADIR)/doc/$(NAME)

CC         ?= cc
CFLAGS     ?= -O2 -g
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

# 屏幕共享修复：只用 libpulse / libpipewire 的头文件，运行时不依赖它们；
# libX11 用于读 Xft.dpi 还原分数缩放（见 qq-wl-portal.c「7.」）
SS_CFLAGS  := $(shell $(PKG_CONFIG) --cflags gio-unix-2.0 libpulse libpipewire-0.3 x11)
SS_LIBS    := $(shell $(PKG_CONFIG) --libs gio-unix-2.0 x11)
# 剪贴板修复
CB_CFLAGS  := $(shell $(PKG_CONFIG) --cflags x11 wayland-client)
CB_LIBS    := $(shell $(PKG_CONFIG) --libs x11 wayland-client)

SS_LIB     := libqq-wl-portal.so
CB_LIB     := libqq-clipbridge.so
SH_LIB     := libqq-screenshot.so
CMD        := $(NAME)
CB_PROTOCOLS := ext-data-control-v1 wlr-data-control-unstable-v1
CB_GEN_H   := $(CB_PROTOCOLS:%=build/%-client-protocol.h)
CB_GEN_C   := $(CB_PROTOCOLS:%=build/%-protocol.c)

all: $(SS_LIB) $(CB_LIB) $(SH_LIB) $(CMD)

build/qq-wl-portal.o: src/qq-wl-portal.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra -Wno-nonnull-compare \
	    -DQQWL_VERSION='"$(VERSION)"' $(SS_CFLAGS) -c -o $@ $<

build/dlsym_trampoline.o: src/dlsym_trampoline.S
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(ASFLAGS) -fPIC -c -o $@ $<

$(SS_LIB): build/qq-wl-portal.o build/dlsym_trampoline.o
	$(CC) $(LDFLAGS) -shared -Wl,-z,defs -o $@ $^ $(SS_LIBS) -ldl

build/%-client-protocol.h: protocol/%.xml
	@mkdir -p build
	$(WAYLAND_SCANNER) client-header $< $@

build/%-protocol.c: protocol/%.xml
	@mkdir -p build
	$(WAYLAND_SCANNER) private-code $< $@

$(CB_LIB): src/qq-clipbridge.c $(CB_GEN_H) $(CB_GEN_C)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Ibuild $(CB_CFLAGS) \
	    $(LDFLAGS) -shared -Wl,-z,defs -o $@ src/qq-clipbridge.c $(CB_GEN_C) $(CB_LIBS) -lpthread -ldl

# 截图修复：同样用 X11 与 wayland-client（wlr-screencopy）
$(SH_LIB): src/qq-screenshot.c build/wlr-screencopy-unstable-v1-client-protocol.h build/wlr-screencopy-unstable-v1-protocol.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra -Ibuild $(CB_CFLAGS) \
	    $(LDFLAGS) -shared -Wl,-z,defs -o $@ src/qq-screenshot.c build/wlr-screencopy-unstable-v1-protocol.c $(CB_LIBS) -ldl

$(CMD): $(CMD).in
	sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@
	chmod +x $@

install: all
	install -Dm755 $(SS_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(SS_LIB)
	install -Dm755 $(CB_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(CB_LIB)
	install -Dm755 $(SH_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(SH_LIB)
	install -Dm755 $(CMD)            $(DESTDIR)$(BINDIR)/$(CMD)
	install -Dm644 $(CMD).desktop    $(DESTDIR)$(DATADIR)/applications/$(CMD).desktop
	install -Dm644 README.md         $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 LICENSE           $(DESTDIR)$(DATADIR)/licenses/$(NAME)/LICENSE

clean:
	rm -rf build src/*.o $(SS_LIB) $(CB_LIB) $(SH_LIB) $(CMD)

print-version:
	@echo $(VERSION)

.PHONY: all install clean print-version
