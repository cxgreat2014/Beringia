# SPDX-License-Identifier: BSD-3-Clause
# Builds kcinject with OpenCorePkg's userspace build (User/Makefile), out of tree:
#   make -C BUILD_DIR -f tools/kc/Makefile OC_USER=.../OpenCorePkg UDK_PATH=.../audk
# scripts/kc/build.sh does this. Object list as OpenCorePkg Utilities/TestKextInject/Makefile.

KC_DIR  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

ifeq ($(OC_USER),)
$(error set OC_USER to an OpenCorePkg checkout)
endif

PROJECT = kcinject
PRODUCT = $(PROJECT)$(SUFFIX)
OBJS    = $(PROJECT).o \
	CommonPatches.o \
	CpuidPatches.o \
	KextPatcher.o \
	KxldState.o \
	PrelinkedKext.o \
	PrelinkedContext.o \
	MkextContext.o \
	Vtables.o \
	Link.o \
	KernelReader.o \
	KernelCollection.o \
	lzss.o \
	lzvn.o \
	adler32.o \
	compress.o \
	crc32.o \
	deflate.o \
	infback.o \
	inffast.o \
	inflate.o \
	inftrees.o \
	trees.o \
	uncompr.o \
	zlib_uefi.o \
	zutil.o
VPATH   = $(KC_DIR):$\
	$(OC_USER)/Library/OcAppleKernelLib:$\
	$(OC_USER)/Library/OcCompressionLib/lzss:$\
	$(OC_USER)/Library/OcCompressionLib/lzvn:$\
	$(OC_USER)/Library/OcCompressionLib/zlib
include $(OC_USER)/User/Makefile
include $(OC_USER)/User/SilenceZlibWarnings
