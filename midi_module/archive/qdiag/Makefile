KDIR  ?= /home/build/linux-kronos
PWD   := $(shell pwd)
PATCH := ../../../patch_init_offset.py

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	python3 $(PATCH) midi_qdiag.ko

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
