TARGET = kmodule

GIT = git

ifdef DEBUG
EXTRA_CFLAGS += -DRAIDXOR_DEBUG
endif

ifdef TESTCASES
EXTRA_CFLAGS += -DRAIDXOR_RUN_TESTCASES
endif

all:
	$(MAKE) -C src EXTRA_CFLAGS="$(EXTRA_CFLAGS)" all

clean:
	$(MAKE) -C src EXTRA_CFLAGS="$(EXTRA_CFLAGS)" clean

dist:
	@echo Creating archive in $(TARGET).tar.bz2
	$(GIT) archive --format=tar --prefix=$(TARGET)/ HEAD | bzip2 > $(TARGET).tar.bz2
