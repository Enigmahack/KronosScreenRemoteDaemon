CC     = i686-linux-gnu-gcc
CFLAGS = -Wall -O2 -static
OUTDIR = ../build

_VERSION := $(shell sed -n 's/.*SCREENREMOTE_VERSION "\([^"]*\)".*/\1/p' screenremote.c)
BUILD_ID := $(shell date +%Y%m%d)-$(_VERSION)

TARGETS = $(OUTDIR)/screenremote 

all: $(OUTDIR) $(TARGETS)

$(OUTDIR):
	mkdir -p $(OUTDIR)

vkbd_ko.h:
	$(MAKE) -C ../vkbd_module -f Makefile.module

$(OUTDIR)/screenremote: screenremote.c vkbd_ko.h
	$(CC) $(CFLAGS) -DBUILD_ID=\"$(BUILD_ID)\" -o $@ screenremote.c

clean:
	rm -f $(TARGETS)
