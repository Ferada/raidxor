ifneq ($(KERNELRELEASE),)

obj-m := test.o raidxor.o

raidxor-objs := module.o

else
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
endif
