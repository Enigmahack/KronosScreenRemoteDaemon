ifeq ($(origin CC),default)
  CC := $(if $(shell command -v i686-linux-gnu-gcc 2>/dev/null),i686-linux-gnu-gcc,gcc)
endif
CFLAGS = -Wall -O2 -static $(if $(filter i686-linux-gnu-gcc,$(CC)),,-m32)
OUTDIR = ../build

# miniz (MIT, see miniz.LICENSE) provides the zlib-format deflate encoder used
# by the v3 RGB565 stream on Nautilus. Built as a separate object with only
# the compressor enabled - no stdio/archive/time code, and no zlib-compatible
# names so it can never collide with a system zlib.
MINIZ_DEFS = -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS \
             -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES

_VERSION := $(shell sed -n 's/.*SCREENREMOTE_VERSION "\([^"]*\)".*/\1/p' screenremote.c)
BUILD_ID := $(shell date +%Y%m%d)-$(_VERSION)

TARGETS = $(OUTDIR)/screenremote

all: $(OUTDIR) $(TARGETS)

$(OUTDIR):
	mkdir -p $(OUTDIR)

vkbd_ko.h: ../vkbd_module/vkbd.c ../vkbd_module/Kbuild
	$(MAKE) -C ../vkbd_module -f Makefile.module

midi_bridge_ko.h: ../midi_module/midi_bridge.c ../midi_module/Kbuild
	$(MAKE) -C ../midi_module -f Makefile.module

nks4_inject_ko.h: ../nks4_inject_module/nks4_inject.c ../nks4_inject_module/Kbuild
	$(MAKE) -C ../nks4_inject_module -f Makefile.module

eva_mode_ko.h: ../eva_mode_module/eva_mode.c ../eva_mode_module/Kbuild
	$(MAKE) -C ../eva_mode_module -f Makefile.module

mode_page_hook_ko.h: ../mode_page_hook_module/mode_page_hook.c ../mode_page_hook_module/Kbuild
	$(MAKE) -C ../mode_page_hook_module -f Makefile.module

midi_tcp_bin.h: midi_tcp.c
	$(CC) $(CFLAGS) -o midi_tcp_bin midi_tcp.c
	xxd -i midi_tcp_bin > $@
	rm -f midi_tcp_bin

miniz.o: miniz.c miniz.h
	$(CC) $(filter-out -Wall,$(CFLAGS)) $(MINIZ_DEFS) -c -o $@ miniz.c

$(OUTDIR)/screenremote: screenremote.c miniz.o vkbd_ko.h midi_bridge_ko.h nks4_inject_ko.h eva_mode_ko.h mode_page_hook_ko.h midi_tcp_bin.h
	$(CC) $(CFLAGS) $(MINIZ_DEFS) -DBUILD_ID=\"$(BUILD_ID)\" -o $@ screenremote.c miniz.o

clean:
	rm -f $(TARGETS) miniz.o vkbd_ko.h midi_bridge_ko.h nks4_inject_ko.h eva_mode_ko.h mode_page_hook_ko.h midi_tcp_bin.h midi_tcp_bin
	# Recurse into the kernel-module build dirs.  Without this, `make clean`
	# leaves the old .ko/.o in place; kbuild then skips recompiling when the
	# module source mtime isn't newer than the artifact (common right after a
	# git checkout/pull), silently re-embedding a STALE module.  That is how a
	# fix to midi_bridge.c can fail to ship and a unit keeps running the old,
	# unfixed module.
	$(MAKE) -C ../vkbd_module -f Makefile.module clean
	$(MAKE) -C ../midi_module -f Makefile.module clean
	$(MAKE) -C ../nks4_inject_module -f Makefile.module clean
	$(MAKE) -C ../eva_mode_module -f Makefile.module clean
	$(MAKE) -C ../mode_page_hook_module -f Makefile.module clean

.PHONY: all clean
