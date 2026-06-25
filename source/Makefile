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

vkbd_ko.h:
	$(MAKE) -C ../vkbd_module -f Makefile.module

midi_inject_ko.h:
	$(MAKE) -C ../midi_module -f Makefile.module

midi_tcp_bin.h: midi_tcp.c
	$(CC) $(CFLAGS) -o midi_tcp_bin midi_tcp.c
	xxd -i midi_tcp_bin > $@
	rm -f midi_tcp_bin

$(OUTDIR)/screenremote: screenremote.c vkbd_ko.h midi_inject_ko.h midi_tcp_bin.h
	$(CC) $(CFLAGS) -DBUILD_ID=\"$(BUILD_ID)\" -o $@ screenremote.c

clean:
	rm -f $(TARGETS) vkbd_ko.h midi_inject_ko.h midi_tcp_bin.h midi_tcp_bin
