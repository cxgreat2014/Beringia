// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/beringia-fb: a framebuffer the foreign kernel draws into (a part of a carve-out), readable by Linux user space.
 *
 * The loader publishes it (bh_fb_publish) once it has reserved the memory and described it to the foreign kernel; a
 * viewer or a screenshot tool reads the pixels with pread(2) and the geometry from the device's "geometry" attribute
 * (/sys/class/misc/beringia-fb/geometry: "WIDTH HEIGHT STRIDE FORMAT", FORMAT xrgb8888).
 *
 * The foreign kernel maps the memory write-combining or uncached; Linux's view of the carve-out is write-back. Before
 * copying, read() flushes the lines it is about to read, so it returns what is in memory, not a stale cached copy.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>
#include "internal.h"

static DEFINE_MUTEX(fb_lock);
static struct {
	void *va;
	u64 size;
	unsigned int width, height, stride;
	bool published;
} fb;

static ssize_t fb_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	ssize_t done = 0;
	u64 pos = *ppos;

	mutex_lock(&fb_lock);
	if (!fb.published) {
		mutex_unlock(&fb_lock);
		return -ENODEV;
	}
	if (pos >= fb.size) {
		mutex_unlock(&fb_lock);
		return 0;
	}
	len = min_t(u64, len, fb.size - pos);
	while (len) {
		size_t n = min_t(size_t, len, 64 << 10);

		clflush_cache_range(fb.va + pos, n);
		if (copy_to_user(buf + done, fb.va + pos, n)) {
			if (!done)
				done = -EFAULT;
			break;
		}
		pos += n;
		done += n;
		len -= n;
	}
	mutex_unlock(&fb_lock);
	if (done > 0)
		*ppos = pos;
	return done;
}

static const struct file_operations fb_fops = {
	.owner = THIS_MODULE,
	.read = fb_read,
	.llseek = default_llseek,
};

static ssize_t geometry_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;

	mutex_lock(&fb_lock);
	n = fb.published ? sysfs_emit(buf, "%u %u %u xrgb8888\n", fb.width, fb.height, fb.stride) : -ENODEV;
	mutex_unlock(&fb_lock);
	return n;
}
static DEVICE_ATTR_RO(geometry);

static struct attribute *fb_attrs[] = {
	&dev_attr_geometry.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fb);

static struct miscdevice fb_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "beringia-fb",
	.fops = &fb_fops,
	.groups = fb_groups,
	.mode = 0440,
};

int bh_fb_publish(bh_u64 pa, bh_u64 size, unsigned int width, unsigned int height, unsigned int stride)
{
	void *va = bh_phys(pa, size);
	int err;

	if (!va || !width || !height || stride < width * 4 || (u64)stride * height > size)
		return -EINVAL;
	mutex_lock(&fb_lock);
	if (fb.published) {
		mutex_unlock(&fb_lock);
		return -EBUSY;
	}
	fb.va = va;
	fb.size = size;
	fb.width = width;
	fb.height = height;
	fb.stride = stride;
	fb.published = true;
	mutex_unlock(&fb_lock);
	err = misc_register(&fb_dev);
	if (err) {
		mutex_lock(&fb_lock);
		fb.published = false;
		mutex_unlock(&fb_lock);
		return err;
	}
	bh_logf("framebuffer %ux%u (stride %u) at PA %#llx: /dev/beringia-fb", width, height, stride, pa);
	return 0;
}
EXPORT_SYMBOL(bh_fb_publish);

void bh_fb_unpublish(void)
{
	bool was;

	mutex_lock(&fb_lock);
	was = fb.published;
	fb.published = false;	/* readers that already hold the device now get -ENODEV */
	mutex_unlock(&fb_lock);
	if (was)
		misc_deregister(&fb_dev);
}
EXPORT_SYMBOL(bh_fb_unpublish);
