# SPDX-License-Identifier: APSL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) KBUILD_EXTRA_SYMBOLS=$(CURDIR)/../host/Module.symvers modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
