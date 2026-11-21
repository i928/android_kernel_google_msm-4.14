// SPDX-License-Identifier: GPL-2.0
/*
 * TEST ONLY (i928 -memcpy branch): C3 safety check + same-boot speed A/B.
 *
 * late_initcall: memcpy / __memcpy / memmove / __memmove (new optimized-routines code) against a
 * byte-loop reference, all lengths 0..1100 and selected large sizes, src/dst offsets 0..15,
 * memmove overlaps both directions; guard bytes around the destination must stay intact and the
 * return value must be dst. Any failure -> panic().
 *
 * Speed: echo <MiB per point> > /sys/module/memtest_i928/parameters/run   (pin with taskset)
 *   new vs old (old_memcpy_i928/old_memmove_i928 = the pre-C3 code), aligned and unaligned.
 *   Result: /sys/module/memtest_i928/parameters/result
 */
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/preempt.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#undef pr_fmt
#define pr_fmt(fmt) "memtest_i928: " fmt

typedef void *(copy_fn)(void *, const void *, size_t);
extern copy_fn __memcpy, __memmove, old_memcpy_i928, old_memmove_i928;

/* called through volatile pointers so the compiler can neither inline nor elide them */
static copy_fn *volatile f_memcpy = memcpy, *volatile f___memcpy = __memcpy;
static copy_fn *volatile f_memmove = memmove, *volatile f___memmove = __memmove;
static copy_fn *volatile f_old_memcpy = old_memcpy_i928, *volatile f_old_memmove = old_memmove_i928;

#define GUARD	64
#define BIG	(65536 + 64)
#define BUFSZ	(BIG + 2 * GUARD + 64)

static char mt_result[2048];
module_param_string(result, mt_result, sizeof(mt_result), 0444);

static void ref_move(volatile u8 *d, const volatile u8 *s, size_t n)
{
	size_t i;

	if (d <= s)
		for (i = 0; i < n; i++)
			d[i] = s[i];
	else
		for (i = n; i-- > 0;)
			d[i] = s[i];
}

static u8 *pat, *dbuf, *rbuf;
static unsigned long mt_checked, mt_bad;

static void mt_fail(const char *fn, size_t len, int so, int dof, const char *why)
{
	if (mt_bad++ < 8)
		pr_err("FAIL %s len=%zu src_off=%d dst_off=%d: %s\n", fn, len, so, dof, why);
}

/* non-overlapping copy: dst region in dbuf, src in pat */
static void mt_copy(const char *name, copy_fn *fn, size_t len, int so, int dof)
{
	u8 *d = dbuf + GUARD + dof, *r = rbuf + GUARD + dof;
	const u8 *s = pat + GUARD + so;
	void *ret;

	memset(dbuf, 0xa5, len + 2 * GUARD + 16);
	memset(rbuf, 0xa5, len + 2 * GUARD + 16);
	ref_move(r, s, len);
	ret = fn(d, s, len);
	mt_checked++;
	if (ret != d)
		mt_fail(name, len, so, dof, "return value");
	else if (memcmp(dbuf, rbuf, len + 2 * GUARD + 16))
		mt_fail(name, len, so, dof, "data/guard mismatch");
}

/* overlapping move inside one buffer: dst = src + delta */
static void mt_overlap(const char *name, copy_fn *fn, size_t len, int delta)
{
	u8 *base = dbuf + GUARD + 128, *rb = rbuf + GUARD + 128;
	size_t span = len + 2 * GUARD + 256 + 128;
	void *ret;

	memcpy(dbuf, pat, span);
	memcpy(rbuf, pat, span);
	ref_move(rb + delta, rb, len);
	ret = fn(base + delta, base, len);
	mt_checked++;
	if (ret != base + delta)
		mt_fail(name, len, 0, delta, "overlap return value");
	else if (memcmp(dbuf, rbuf, span))
		mt_fail(name, len, 0, delta, "overlap data/guard mismatch");
}

static int __init memtest_check(void)
{
	static const size_t big[] = { 2047, 2048, 2049, 4095, 4096, 4097, 8191, 16384, 32769, 65535, 65536 };
	static const int offs[] = { 0, 1, 3, 7, 8, 15 };
	struct { const char *n; copy_fn *f; } fns[] = {
		{ "memcpy", f_memcpy }, { "__memcpy", f___memcpy },
		{ "memmove", f_memmove }, { "__memmove", f___memmove },
	};
	size_t len;
	int i, so, dof, k, delta;

	pat = vmalloc(BUFSZ);
	dbuf = vmalloc(BUFSZ);
	rbuf = vmalloc(BUFSZ);
	if (!pat || !dbuf || !rbuf)
		panic("memtest_i928: no memory");
	prandom_bytes(pat, BUFSZ);

	for (k = 0; k < ARRAY_SIZE(fns); k++) {
		for (len = 0; len <= 1100; len++) {
			int all = len <= 300;

			for (so = 0; so < 16; so++)
				for (dof = 0; dof < 16; dof++)
					if (all || ((so == 0 || so == 1 || so == 7 || so == 15) &&
						    (dof == 0 || dof == 3 || dof == 8 || dof == 15)))
						mt_copy(fns[k].n, fns[k].f, len, so, dof);
			if (!(len & 31))
				cond_resched();
		}
		for (i = 0; i < ARRAY_SIZE(big); i++)
			for (so = 0; so < ARRAY_SIZE(offs); so++)
				for (dof = 0; dof < ARRAY_SIZE(offs); dof++)
					mt_copy(fns[k].n, fns[k].f, big[i], offs[so], offs[dof]);
		cond_resched();
	}
	/* memmove overlaps, both directions */
	for (k = 2; k < 4; k++) {
		for (len = 0; len <= 600; len++) {
			for (delta = -100; delta <= 100; delta++)
				mt_overlap(fns[k].n, fns[k].f, len, delta);
			if (!(len & 15))
				cond_resched();
		}
		for (i = 0; i < ARRAY_SIZE(big) - 2; i++)
			for (delta = -128; delta <= 128; delta += 1 + (delta & 7))
				mt_overlap(fns[k].n, fns[k].f, big[i], delta);
	}
	if (mt_bad)
		panic("memtest_i928: %lu of %lu memcpy/memmove checks FAILED\n", mt_bad, mt_checked);
	pr_info("all %lu memcpy/memmove checks passed\n", mt_checked);
	return 0;
}
late_initcall(memtest_check);

static u64 mt_time(copy_fn *fn, u8 *d, const u8 *s, size_t len, unsigned long n)
{
	unsigned long i;
	u64 t;

	preempt_disable();
	t = ktime_get_ns();
	for (i = 0; i < n; i++)
		fn(d, s, len);
	t = ktime_get_ns() - t;
	preempt_enable();
	return t;
}

static int mt_run_set(const char *val, const struct kernel_param *kp)
{
	static const size_t sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536 };
	unsigned int mib, i, r, al, pos = 0;
	u8 *src, *dst;

	if (kstrtouint(val, 0, &mib) || !mib || mib > 4096)
		return -EINVAL;
	src = vmalloc(BIG + 128);
	dst = vmalloc(BIG + 128);
	if (!src || !dst) {
		vfree(src);
		vfree(dst);
		return -ENOMEM;
	}
	memset(src, 0x5a, BIG + 128);
	memset(dst, 0, BIG + 128);
	pos += scnprintf(mt_result + pos, sizeof(mt_result) - pos,
			 "cpu=%d %u MiB/point, best of 3, MB/s new/old (new/old)\nsize   aligned                 unaligned(src+1,dst+3)\n",
			 raw_smp_processor_id(), mib);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned long n = max_t(unsigned long, 1000, ((unsigned long)mib << 20) / sizes[i]);

		pos += scnprintf(mt_result + pos, sizeof(mt_result) - pos, "%-6zu", sizes[i]);
		for (al = 0; al < 2; al++) {
			u64 bn = ~0ULL, bo = ~0ULL, t;

			for (r = 0; r < 3; r++) {
				t = mt_time(f_memcpy, dst + 64 + 3 * al, src + 64 + al, sizes[i], n);
				bn = min(bn, t);
				t = mt_time(f_old_memcpy, dst + 64 + 3 * al, src + 64 + al, sizes[i], n);
				bo = min(bo, t);
				cond_resched();
			}
			pos += scnprintf(mt_result + pos, sizeof(mt_result) - pos, " %6llu/%-6llu (%3llu%%)  ",
					 (u64)n * sizes[i] * 1000 / bn, (u64)n * sizes[i] * 1000 / bo, bo * 100 / bn);
		}
		pos += scnprintf(mt_result + pos, sizeof(mt_result) - pos, "\n");
	}
	{	/* overlapping memmove, 4 KiB, dst = src + 64 (backward copy path) */
		unsigned long n = ((unsigned long)mib << 20) / 4096;
		u64 bn = ~0ULL, bo = ~0ULL;

		for (r = 0; r < 3; r++) {
			bn = min(bn, mt_time(f_memmove, src + 128, src + 64, 4096, n));
			bo = min(bo, mt_time(f_old_memmove, src + 128, src + 64, 4096, n));
		}
		pos += scnprintf(mt_result + pos, sizeof(mt_result) - pos,
				 "memmove 4096 overlap +64: %llu/%llu MB/s (%llu%%)\n",
				 (u64)n * 4096 * 1000 / bn, (u64)n * 4096 * 1000 / bo, bo * 100 / bn);
	}
	pr_info("\n%s", mt_result);
	vfree(src);
	vfree(dst);
	return 0;
}

static const struct kernel_param_ops mt_run_ops = {
	.set = mt_run_set,
};
module_param_cb(run, &mt_run_ops, NULL, 0200);
