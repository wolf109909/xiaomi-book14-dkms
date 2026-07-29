KDIR ?= /usr/lib/modules/$(shell uname -r)/build
CC = clang

obj-m += xiaomi-book14.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(CC) LD=ld.lld LLVM=1 modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
