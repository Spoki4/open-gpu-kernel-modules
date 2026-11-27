# vram-hintd: libc only for Hyprland, sway and niri; X11 with libxcb, xcb-res
# and xcb-randr when pkg-config finds them (X11=0 leaves it out).
#   make [O=builddir] [X11=0]
O ?= .
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
X11 ?= $(shell pkg-config --exists xcb xcb-res xcb-randr && echo 1 || echo 0)

ifeq ($(X11),1)
XCFLAGS := $(shell pkg-config --cflags xcb xcb-res xcb-randr) -DHAVE_X11=1
XLIBS := $(shell pkg-config --libs xcb xcb-res xcb-randr)
else
XCFLAGS := -DHAVE_X11=0
endif

$(O)/vram-hintd: vram-hintd.c
	$(CC) $(CFLAGS) $(XCFLAGS) -o $@ $< $(XLIBS) -lm

clean:
	rm -f $(O)/vram-hintd

.PHONY: clean
