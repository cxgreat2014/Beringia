/* SPDX-License-Identifier: APSL-2.0 */
/*
 * What an x86_64 XNU expects from its booter: boot_args (pexpert/pexpert/i386/boot.h), the EFI memory map entries,
 * and the flattened device tree (pexpert/pexpert/device_tree.h). Copied in layout, checked by static assertions.
 */
#ifndef BERINGIA_XNU_BOOT_H
#define BERINGIA_XNU_BOOT_H

#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>

#define XNU_KERNEL_BASE         0xffffff8000000000ULL   /* KERNEL_BASE: image PA = VA - this (+ slide) */
#define XNU_KERNEL_BASE_OFFSET  0x100000ULL             /* KERNEL_BASE_OFFSET: __HIB's unslid physical address */

#define kBootArgsRevision1      1
#define kBootArgsVersion2       2
#define kBootArgsEfiMode64      64
#define kBootArgsFlagCSRActiveConfig (1 << 3)

#define kEfiReservedMemoryType  0
#define kEfiLoaderData          2
#define kEfiConventionalMemory  7

struct xnu_boot_video_v1 {
	u32 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct xnu_boot_video {
	u32 v_display, v_rowBytes, v_width, v_height, v_depth;
	u8 v_rotate, v_resv_byte[3];
	u32 v_resv[6];
	u64 v_baseAddr;
};

struct xnu_boot_args {
	u16 Revision;
	u16 Version;
	u8 efiMode;
	u8 debugMode;
	u16 flags;
	char CommandLine[1024];
	u32 MemoryMap;
	u32 MemoryMapSize;
	u32 MemoryMapDescriptorSize;
	u32 MemoryMapDescriptorVersion;
	struct xnu_boot_video_v1 VideoV1;
	u32 deviceTreeP;
	u32 deviceTreeLength;
	u32 kaddr;
	u32 ksize;
	u32 efiRuntimeServicesPageStart;
	u32 efiRuntimeServicesPageCount;
	u64 efiRuntimeServicesVirtualPageStart;
	u32 efiSystemTable;
	u32 kslide;
	u32 performanceDataStart;
	u32 performanceDataSize;
	u32 keyStoreDataStart;
	u32 keyStoreDataSize;
	u64 bootMemStart;
	u64 bootMemSize;
	u64 PhysicalMemorySize;
	u64 FSBFrequency;
	u64 pciConfigSpaceBaseAddress;
	u32 pciConfigSpaceStartBusNumber;
	u32 pciConfigSpaceEndBusNumber;
	u32 csrActiveConfig;
	u32 csrCapabilities;
	u32 boot_SMC_plimit;
	u16 bootProgressMeterStart;
	u16 bootProgressMeterEnd;
	struct xnu_boot_video Video;
	u32 apfsDataStart;
	u32 apfsDataSize;
	u64 KC_hdrs_vaddr;
	u64 arvRootHashStart, arvRootHashSize;
	u64 arvManifestStart, arvManifestSize;
	u64 bsARVRootHashStart, bsARVRootHashSize;
	u64 bsARVManifestStart, bsARVManifestSize;
	u32 __reserved4[692];
};

static_assert(offsetof(struct xnu_boot_args, MemoryMap) == 1032);
static_assert(offsetof(struct xnu_boot_args, deviceTreeP) == 1072);
static_assert(offsetof(struct xnu_boot_args, kslide) == 1108);
static_assert(offsetof(struct xnu_boot_args, PhysicalMemorySize) == 1144);
static_assert(offsetof(struct xnu_boot_args, Video) == 1192);
static_assert(offsetof(struct xnu_boot_args, KC_hdrs_vaddr) == 1256);
static_assert(sizeof(struct xnu_boot_args) == 4096);

struct xnu_efi_range {
	u32 Type;
	u32 Pad;
	u64 PhysicalStart;
	u64 VirtualStart;
	u64 NumberOfPages;
	u64 Attribute;
};
static_assert(sizeof(struct xnu_efi_range) == 40);

/* Flattened device tree: a node is {nProperties, nChildren}, then its properties, then its children. */
struct xnu_dt_node {
	u32 nProperties;
	u32 nChildren;
};

struct xnu_dt_prop {
	char name[32];
	u32 length;             /* the value follows, padded to 4 bytes */
};

#endif
