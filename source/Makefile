ifeq ($(origin CC),default)
  CC := $(if $(shell command -v i686-linux-gnu-gcc 2>/dev/null),i686-linux-gnu-gcc,gcc)
endif
CFLAGS = -Wall -O2 -static $(if $(filter i686-linux-gnu-gcc,$(CC)),,-m32)
OUTDIR = ../build

_VERSION := $(shell sed -n 's/.*SCREENREMOTE_VERSION "\([^"]*\)".*/\1/p' screenremote.c)
BUILD_ID := $(shell date +%Y%m%d)-$(_VERSION)

TARGETS = $(OUTDIR)/screenremote

all: $(OUTDIR) $(TARGETS)

$(OUTDIR):
	mkdir -p $(OUTDIR)

vkbd_ko.h: ../vkbd_module/vkbd.c ../vkbd_module/Kbuild
	$(MAKE) -C ../vkbd_module -f Makefile.module

midi_inject_ko.h: ../midi_module/midi_inject.c ../midi_module/Kbuild
	$(MAKE) -C ../midi_module -f Makefile.module

midi_tcp_bin.h: midi_tcp.c
	$(CC) $(CFLAGS) -o midi_tcp_bin midi_tcp.c
	xxd -i midi_tcp_bin > $@
	rm -f midi_tcp_bin

$(OUTDIR)/screenremote: screenremote.c vkbd_ko.h midi_inject_ko.h midi_tcp_bin.h
	$(CC) $(CFLAGS) -DBUILD_ID=\"$(BUILD_ID)\" -o $@ screenremote.c

clean:
	rm -f $(TARGETS) vkbd_ko.h midi_inject_ko.h midi_tcp_bin.h midi_tcp_bin
	# Recurse into the kernel-module build dirs.  Without this, `make clean`
	# leaves the old .ko/.o in place; kbuild then skips recompiling when the
	# module source mtime isn't newer than the artifact (common right after a
	# git checkout/pull), silently re-embedding a STALE module.  That is how a
	# fix to midi_inject.c can fail to ship and a non-rooted unit keeps
	# freezing at boot on the old, unfixed hook.
	$(MAKE) -C ../vkbd_module -f Makefile.module clean
	$(MAKE) -C ../midi_module -f Makefile.module clean

.PHONY: all clean
