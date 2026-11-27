// SPDX-License-Identifier: APSL-2.0
/*
 * beringia_xnu.ko: loads an x86_64 XNU kernel into the carve-out beringia_host.ko lends it, builds its boot
 * environment and starts it on a hosted CPU (docs/architecture.md, section 8).
 *
 * Parameters:
 *   kernel=PATH       the kernel Mach-O (default /lib/firmware/beringia/kernel)
 *   boot_args=STRING  XNU's boot arguments
 *   ramdisk=PATH      an image copied into XNU's RAM for memdev
 *   disk=PATH         a host-file-backed block device for BeringiaDisk (unit 0)
 *   linux_cpu=N       the Linux CPU that runs hosted CPU 0 (default 1)
 *   fb=WxH            the boot framebuffer (default 1280x800, "" for none), readable as /dev/beringia-fb
 *
 * This module uses Linux only through beringia_host.ko's interface (host.h) and the module ABI.
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "loader.h"

static char *kernel = "/lib/firmware/beringia/kernel";
module_param(kernel, charp, 0444);
static char *ramdisk = "";
module_param(ramdisk, charp, 0444);
MODULE_PARM_DESC(ramdisk, "a memory-disk image (HFS+ root filesystem) for memdev pmd0 + rd=md0");
static char *disk = "";
module_param(disk, charp, 0444);
MODULE_PARM_DESC(disk, "a raw disk image for BeringiaDisk, 512-byte sectors, host block unit 0");
static char *boot_args = "-v debug=0x8 serial=3 -pmap_pcid_disable keepsyms=1 cpus=1 startup_debug=1";
module_param(boot_args, charp, 0444);
static int linux_cpu = 1;
module_param(linux_cpu, int, 0444);
/* Not readable through sysfs: it names the user's own key file. */
static char *osk = "";
module_param(osk, charp, 0);
MODULE_PARM_DESC(osk, "a file with the 64-character DSMOS key (OSK0 then OSK1) from your own Mac, for BeringiaSMC");
static char *fb = "1280x800";
module_param(fb, charp, 0444);
MODULE_PARM_DESC(fb, "the boot framebuffer, WIDTHxHEIGHT at 32 bpp (/dev/beringia-fb); \"\" or 0 for none");

struct ldr ldr;

/*
 * Bring-up: `echo 1 > /sys/module/beringia_xnu/parameters/dump` makes every hosted CPU print its threads (kernel
 * stacks); `echo WindowServer > ...` also prints the user-space backtraces of processes whose name contains that
 * string, `echo '*' > ...` those of every process.
 */
static int dump_set(const char *val, const struct kernel_param *kp)
{
	size_t n;
	int i;

	if (!ldr.shared)
		return -ENODEV;
	/* the kernel writes its header version on its first host call; without it, the struct ends before dump_filter */
	if (ldr.shared->kernel_abi != BERINGIA_ABI_VERSION) {
		bh_logf("xnu: the kernel was built with beringia ABI %u, this loader has %u: no dump",
		    ldr.shared->kernel_abi, BERINGIA_ABI_VERSION);
		return -EINVAL;
	}
	n = strcspn(val, "\n");
	if (n >= sizeof(ldr.shared->dump_filter))
		return -EINVAL;
	memset(ldr.shared->dump_filter, 0, sizeof(ldr.shared->dump_filter));
	memcpy(ldr.shared->dump_filter, val, n);
	for (i = 0; i < ldr.ncpus; i++) {
		__atomic_fetch_or(&ldr.shared->cpu[i].pending, BERINGIA_PENDING_DUMP, __ATOMIC_SEQ_CST);
		bh_kick(i);
	}
	return 0;
}

static const struct kernel_param_ops dump_ops = { .set = dump_set };
module_param_cb(dump, &dump_ops, NULL, 0200);
MODULE_PARM_DESC(dump, "process-name substring, \"*\", or peek:PID:ADDR:LEN: the hosted XNU prints threads and user stacks");

/*
 * An orderly end of the run: `echo 1` asks XNU to shut down (launchd gets SIGTERM and shuts the system down the way a
 * power key does, logd flushes and the disks are unmounted); `echo escalate` runs the BSD shutdown in the kernel
 * instead, for when launchd does not answer. Either way XNU reaches halt_all_cpus, which ends the hosted run.
 */
static int shutdown_set(const char *val, const struct kernel_param *kp)
{
	unsigned int bit;
	int i;

	if (!ldr.shared)
		return -ENODEV;
	if (ldr.shared->kernel_abi != BERINGIA_ABI_VERSION)
		return -EINVAL;
	bit = (!strncmp(val, "esc", 3) || val[0] == '2') ? BERINGIA_PENDING_ESTOP : BERINGIA_PENDING_STOP;
	bh_logf("xnu: shutdown requested (%s)", bit == BERINGIA_PENDING_ESTOP ? "escalate" : "stop");
	for (i = 0; i < ldr.ncpus; i++) {
		__atomic_fetch_or(&ldr.shared->cpu[i].pending, bit, __ATOMIC_SEQ_CST);
		bh_kick(i);
	}
	return 0;
}

static const struct kernel_param_ops shutdown_ops = { .set = shutdown_set };
module_param_cb(shutdown, &shutdown_ops, NULL, 0200);
MODULE_PARM_DESC(shutdown, "1: orderly shutdown; escalate: the kernel's own shutdown path");

static int state_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", READ_ONCE(ldr.halted) ? "halted" : "running");
}

static const struct kernel_param_ops state_ops = { .get = state_get };
module_param_cb(state, &state_ops, NULL, 0444);
MODULE_PARM_DESC(state, "running, or halted once XNU ended the run itself");

static int find_low_range(void)
{
	int i;

	ldr.nranges = bh_mem_ranges(ldr.ranges, LDR_MAX_RANGES);
	for (i = 0; i < ldr.nranges; i++) {
		if (ldr.ranges[i].pa + ldr.ranges[i].size <= (4ULL << 30) && ldr.ranges[i].size >= (256ULL << 20) &&
		    !(ldr.ranges[i].pa & ((2ULL << 20) - 1))) {
			ldr.low = ldr.ranges[i];
			return 0;
		}
	}
	bh_log("xnu: no carve-out of 256 MiB or more below 4 GiB (beringia_host.ko carve=...)");
	return -ENOMEM;
}

/* Read the kernel file into the top of the low carve-out (scratch until XNU runs). */
static int read_kernel(u8 **image, u64 *size)
{
	void *f = bh_file_open(kernel, 0, size);
	u64 stage, off;
	u8 *dst;

	if (!f) {
		bh_logf("xnu: cannot open %s", kernel);
		return -ENOENT;
	}
	stage = (ldr.low.pa + ldr.low.size - *size) & ~((2ULL << 20) - 1);
	dst = bh_phys(stage, *size);
	if (!dst || *size > ldr.low.size / 4) {
		bh_file_close(f);
		return -EFBIG;
	}
	for (off = 0; off < *size;) {
		long r = bh_file_rw(f, 0, dst + off, min_t(u64, *size - off, 1 << 20), off);

		if (r <= 0) {
			bh_file_close(f);
			return r < 0 ? r : -EIO;
		}
		off += r;
	}
	bh_file_close(f);
	*image = dst;
	return 0;
}

/* Keep unit 0 open for the whole run. Prefer read/write, but a read-only image is useful for recovery boots and is
 * accurately reported through BERINGIA_CALL_BLOCK_INFO. */
static int open_disk(void)
{
	u64 size = 0;
	void *f;

	if (!disk[0])
		return 0;
	f = bh_file_open(disk, 1, &size);
	if (f) {
		ldr.disk_read_only = 0;
	} else {
		f = bh_file_open(disk, 0, &size);
		ldr.disk_read_only = 1;
	}
	if (!f) {
		bh_logf("xnu: cannot open disk %s", disk);
		return -ENOENT;
	}
	if (!size || (size & (BERINGIA_BLOCK_SIZE - 1))) {
		bh_logf("xnu: disk %s size %#llx is not a nonzero multiple of %u", disk, size,
		    BERINGIA_BLOCK_SIZE);
		bh_file_close(f);
		return -EINVAL;
	}
	ldr.disk_file = f;
	ldr.disk_size = size;
	bh_logf("xnu: disk %s, %#llx bytes, %s (block unit 0)", disk, size,
	    ldr.disk_read_only ? "read-only" : "writable");
	return 0;
}

/*
 * The DSMOS key (docs/setup/macos-assets.md): Apple's text, which every Intel Mac's SMC returns for the keys OSK0 and
 * OSK1. It is not part of Beringia; the user supplies it from their own Mac, and BeringiaSMC answers DSMOS with it.
 * It reaches XNU only as /efi/platform/beringia-osk, which BeringiaSMC takes and removes before user space runs.
 */
static int read_osk(void)
{
	u8 buf[80];
	u64 size = 0;
	long n;
	void *f;
	int i;

	if (!osk[0])
		return 0;
	f = bh_file_open(osk, 0, &size);
	if (!f) {
		bh_logf("xnu: cannot open the DSMOS key file %s", osk);
		return -ENOENT;
	}
	n = bh_file_rw(f, 0, buf, sizeof(buf), 0);
	bh_file_close(f);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	for (i = 0; i < n && n == sizeof(ldr.osk); i++)
		if (buf[i] < 0x20 || buf[i] > 0x7e)
			n = -1;
	if (n != sizeof(ldr.osk)) {
		memzero_explicit(buf, sizeof(buf));
		bh_logf("xnu: %s: the DSMOS key must be 64 printable characters (OSK0 followed by OSK1)", osk);
		return -EINVAL;
	}
	memcpy(ldr.osk, buf, sizeof(ldr.osk));
	memzero_explicit(buf, sizeof(buf));
	ldr.have_osk = 1;
	bh_log("xnu: DSMOS key supplied, for BeringiaSMC");
	return 0;
}

static int parse_fb(void)
{
	unsigned int w, h;
	char end;

	if (!fb[0] || !strcmp(fb, "0"))
		return 0;
	if (sscanf(fb, "%ux%u%c", &w, &h, &end) != 2 || w < 640 || h < 480 || w > 4096 || h > 4096 || (w & 7)) {
		bh_logf("xnu: fb=%s: want WIDTHxHEIGHT from 640x480 to 4096x4096, the width a multiple of 8", fb);
		return -EINVAL;
	}
	ldr.fb_width = w;
	ldr.fb_height = h;
	return 0;
}

static void close_disk(void)
{
	if (ldr.disk_file) {
		bh_file_close(ldr.disk_file);
		ldr.disk_file = NULL;
	}
}

static int __init ldr_init(void)
{
	struct bh_world_start st;
	u8 *image;
	u64 size;
	int err;

	bh_logf("xnu: Beringia loader, ABI %d, kernel %s", BERINGIA_ABI_VERSION, kernel);
	err = find_low_range();
	if (err)
		return err;
	ldr.slide = ldr.low.pa;
	ldr.ncpus = 1;
	err = read_kernel(&image, &size);
	if (err)
		return err;
	err = ldr_macho_load(image, size);
	if (err)
		return err;
	ldr.ramdisk_path = ramdisk[0] ? ramdisk : NULL;
	err = parse_fb();
	if (err)
		return err;
	err = read_osk();
	if (err)
		return err;
	err = ldr_boot_build(boot_args);
	memzero_explicit(ldr.osk, sizeof(ldr.osk));	/* in XNU's device tree now; no copy stays in Linux */
	if (err)
		return err;
	err = open_disk();
	if (err)
		return err;
	/* not fatal: XNU still has its framebuffer, only Linux cannot look at it */
	if (ldr.fb_size && bh_fb_publish(ldr.fb_pa, ldr.fb_size, ldr.fb_width, ldr.fb_height, ldr.fb_stride))
		bh_log("xnu: could not publish /dev/beringia-fb");

	/* CPU 0 enters vstart(boot_args) as pstart would: on low_eintstack, interrupts off */
	st.cr3 = ldr.pml4_pa;
	st.rip = ldr.sym_vstart;
	st.rsp = ldr.sym_stack & ~15ULL;
	st.rdi = ldr.boot_args_pa;
	st.rsi = 0;
	bh_logf("xnu: entering vstart at %#llx (stack %#llx, CR3 %#llx)", st.rip, st.rsp, st.cr3);
	err = bh_cpu_start(0, linux_cpu, &st, ldr_hostcall);
	if (err) {
		bh_fb_unpublish();
		close_disk();
	}
	return err;
}

static void __exit ldr_exit(void)
{
	bh_cpu_stop_all();
	bh_fb_unpublish();
	close_disk();
	bh_log("xnu: unloaded");
}

module_init(ldr_init);
module_exit(ldr_exit);
MODULE_LICENSE("Proprietary");	/* APSL-2.0 */
MODULE_DESCRIPTION("Beringia: loads and runs a hosted x86_64 XNU");
