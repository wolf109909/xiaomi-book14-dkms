# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)

obj-m := xiaomi-book14.o

else

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

# Out-of-tree modules must be built with the toolchain the kernel was built
# with: Clang/LLVM for CachyOS-style kernels, GCC for most distro kernels.
ifeq ($(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo y),y)
KBUILD_TOOLCHAIN := LLVM=1
endif

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_TOOLCHAIN) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_TOOLCHAIN) clean

.PHONY: all clean

endif
