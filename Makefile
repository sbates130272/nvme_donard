#
# By default, the build is done against the running linux kernel source.
# To build against a different kernel source tree, set SYSSRC:
#
#    make SYSSRC=/path/to/kernel/source

ifdef SYSSRC
 KERNEL_SOURCES	 = $(SYSSRC)
else
 KERNEL_UNAME	:= $(shell uname -r)
 KERNEL_SOURCES	 = /lib/modules/$(KERNEL_UNAME)/build
endif


NVIDIA_SRC := /usr/src/nvidia-331-331.62

default: modules
.PHONY: default
install: modules_install
	cp donard-nvme-blacklist.conf /etc/modprobe.d
	cp 80-donard.rules /etc/udev/rules.d
	update-initramfs -u

.PHONY: install


nvidia-syms:
	make -C $(KERNEL_SOURCES) SUBDIRS=$(NVIDIA_SRC) modules

.PHONY:


%::
	$(MAKE) -C $(KERNEL_SOURCES) \
        KBUILD_EXTRA_SYMBOLS=$(NVIDIA_SRC)/Module.symvers \
	    NVIDIA_SRC=$(NVIDIA_SRC) \
        M=$$PWD $@
