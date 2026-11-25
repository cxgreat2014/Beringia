// SPDX-License-Identifier: APSL-2.0
/*
 * Loading an x86_64 XNU kernel the way Apple's booter does with KASLR: every segment at physical
 * (vmaddr - KERNEL_BASE + slide), the local relocations applied with the same slide (r_address relative to the first
 * writable segment, x86_64 Mach-O convention), and kaddr/kslide reported so XNU can check it (i386_vm_init).
 */
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include "loader.h"
#include "xnu_boot.h"

#define MH_MAGIC_64     0xfeedfacf
#define CPU_TYPE_X86_64 0x01000007
#define MH_EXECUTE      2
#define LC_SEGMENT_64   0x19
#define LC_SYMTAB       0x2
#define LC_DYSYMTAB     0xb
#define VM_PROT_WRITE   2

struct mh64 { u32 magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct lc { u32 cmd, cmdsize; };
struct seg64 { u32 cmd, cmdsize; char segname[16]; u64 vmaddr, vmsize, fileoff, filesize; u32 maxprot, initprot, nsects, flags; };
struct symtab_lc { u32 cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct dysymtab_lc { u32 cmd, cmdsize, v[18]; };
struct nlist64 { u32 strx; u8 type, sect; u16 desc; u64 value; };

static const char *const wanted[] = { "_vstart", "_low_eintstack", "_beringia_hook", "_beringia_shared" };

int ldr_macho_load(const u8 *f, u64 n)
{
	const struct mh64 *h = (const void *)f;
	const struct lc *l, *end;
	const struct symtab_lc *st = NULL;
	const struct dysymtab_lc *dy = NULL;
	u64 reloc_base = 0, slide = ldr.slide, lo = ~0ULL, hi = 0, *syms[4];
	void *hdr_dst = NULL;	/* the loaded copy of __TEXT (holds the Mach-O header) */
	u32 i;

	if (n < sizeof(*h) || h->magic != MH_MAGIC_64 || h->cputype != CPU_TYPE_X86_64 || h->filetype != MH_EXECUTE) {
		bh_log("xnu: not an x86_64 kernel Mach-O (a kernel collection is not supported yet)");
		return -ENOEXEC;
	}
	if (sizeof(*h) + h->sizeofcmds > n)
		return -ENOEXEC;
	l = (const void *)(h + 1);
	end = (const void *)((const u8 *)l + h->sizeofcmds);
	for (i = 0; i < h->ncmds && l < end; i++, l = (const void *)((const u8 *)l + l->cmdsize)) {
		if (l->cmdsize < sizeof(*l))
			return -ENOEXEC;
		if (l->cmd == LC_SEGMENT_64) {
			const struct seg64 *s = (const void *)l;
			u64 pa = s->vmaddr - XNU_KERNEL_BASE + slide;
			void *dst;

			if (!s->vmsize)
				continue;
			if (s->vmaddr < XNU_KERNEL_BASE || s->fileoff + s->filesize > n || s->filesize > s->vmsize) {
				bh_logf("xnu: bad segment %.16s", s->segname);
				return -ENOEXEC;
			}
			dst = bh_phys(pa, s->vmsize);
			if (!dst || pa + s->vmsize > ldr.low.pa + ldr.low.size) {
				bh_logf("xnu: segment %.16s at PA %#llx does not fit the low carve-out", s->segname, pa);
				return -ENOSPC;
			}
			memcpy(dst, f + s->fileoff, s->filesize);
			memset((u8 *)dst + s->filesize, 0, s->vmsize - s->filesize);
			if (!hdr_dst && s->fileoff == 0)
				hdr_dst = dst;	/* __TEXT carries the header at offset 0 */
			if (!reloc_base && (s->initprot & VM_PROT_WRITE))
				reloc_base = s->vmaddr;
			lo = min(lo, pa);
			hi = max(hi, pa + s->vmsize);
			bh_logf("xnu: %-16.16s VA %#llx PA %#llx size %#llx", s->segname, s->vmaddr + slide, pa, s->vmsize);
		} else if (l->cmd == LC_SYMTAB) {
			st = (const void *)l;
		} else if (l->cmd == LC_DYSYMTAB) {
			dy = (const void *)l;
		}
	}
	if (!st || !dy || !reloc_base)
		return -ENOEXEC;
	ldr.kaddr = lo;
	ldr.kend = hi;

	/*
	 * A booter also slides the addresses recorded *in* the Mach-O header (segment vmaddr, section addr): XNU reads
	 * __HIB's bounds and other segment extents back through getsegbynamefromheader(), so an unslid header would make
	 * the slide get applied twice (the double-map bounds check catches exactly this).
	 */
	if (hdr_dst) {
		struct lc *hl = (struct lc *)((u8 *)hdr_dst + sizeof(struct mh64));
		u32 hc;

		for (hc = 0; hc < h->ncmds; hc++, hl = (struct lc *)((u8 *)hl + hl->cmdsize)) {
			if (hl->cmd == LC_SEGMENT_64) {
				struct seg64 *s = (struct seg64 *)hl;
				u8 *sect = (u8 *)(s + 1);
				u32 si;

				s->vmaddr += slide;
				for (si = 0; si < s->nsects; si++, sect += 80)
					*(u64 *)(sect + 32) += slide;	/* section_64.addr */
			}
		}
	}

	/* local relocations: locreloff = v[16], nlocrel = v[17] */
	{
		u32 off = dy->v[16], cnt = dy->v[17], k, n64 = 0, n32 = 0;

		bh_logf("xnu: n=%llu locreloff=%u nlocrel=%u first=%08x %08x", n, off, cnt,
		    off + 8 < n ? *(u32 *)(f + off) : 0, off + 8 < n ? *(u32 *)(f + off + 4) : 0);
		if ((u64)off + (u64)cnt * 8 > n) {
			bh_logf("xnu: relocation data out of bounds: n=%llu off=%u cnt=%u", n, off, cnt);
			return -ENOEXEC;
		}
		for (k = 0; k < cnt; k++) {
			s32 addr = *(const s32 *)(f + off + 8 * k);
			u32 info = *(const u32 *)(f + off + 8 * k + 4);
			u32 len = (info >> 25) & 3, type = info >> 28, ext = (info >> 27) & 1;
			u64 va = reloc_base + (s64)addr;
			void *p = bh_phys(va - XNU_KERNEL_BASE + slide, 8);

			if (type != 0 || ext || !p) {
				bh_logf("xnu: unsupported relocation %u (type %u, extern %u) at %#llx", k, type, ext, va);
				return -ENOEXEC;
			}
			if (len == 3) {
				*(u64 *)p += slide;
				n64++;
			} else if (len == 2) {
				*(u32 *)p += (u32)slide;
				n32++;
			} else {
				bh_logf("xnu: relocation %u has bad length %u (info %#x)", k, len, info);
				return -ENOEXEC;
			}
		}
		bh_logf("xnu: %u local relocations (%u 64-bit, %u 32-bit), slide %#llx", cnt, n64, n32, slide);
	}

	/* symbols */
	syms[0] = &ldr.sym_vstart;
	syms[1] = &ldr.sym_stack;
	syms[2] = &ldr.sym_hook;
	syms[3] = &ldr.sym_shared;
	if ((u64)st->symoff + (u64)st->nsyms * sizeof(struct nlist64) > n || (u64)st->stroff + st->strsize > n)
		return -ENOEXEC;
	for (i = 0; i < st->nsyms; i++) {
		const struct nlist64 *s = (const struct nlist64 *)(f + st->symoff) + i;
		const char *name;
		int w;

		if (s->strx >= st->strsize || (s->type & 0xe0))	/* no debugging symbols */
			continue;
		name = (const char *)f + st->stroff + s->strx;
		for (w = 0; w < 4; w++)
			if (!*syms[w] && !strcmp(name, wanted[w]))
				*syms[w] = s->value + slide;
	}
	for (i = 0; i < 4; i++) {
		if (!*syms[i]) {
			bh_logf("xnu: symbol %s missing (kernel built without xnu/patches/hosted?)", wanted[i]);
			return -ENOENT;
		}
	}
	return 0;
}
