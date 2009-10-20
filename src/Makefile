ifneq ($(KERNELRELEASE),)

obj-m := raidxor.o

raidxor-objs := module.o

else
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C "$(KDIR)" SUBDIRS="$(PWD)" EXTRA_CFLAGS="$(EXTRA_CFLAGS)" modules

clean:
	$(MAKE) -C "$(KDIR)" SUBDIRS="$(PWD)" clean

endif
