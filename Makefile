TARGET = kmodule

GIT = git

all:
	@$(MAKE) -C src all

clean:
	@$(MAKE) -C src clean

dist:
	@echo Creating archive in $(TARGET).tar.bz2
	@$(GIT) archive --format=tar --prefix=$(TARGET)/ HEAD | bzip2 > $(TARGET).tar.bz2
