// SPDX-License-Identifier: GPL-2.0
/*
 * TEMP b1c1 bring-up probe: paint white bands into the bootloader splash
 * buffer, one per boot stage, so progress is visible without UART/pstore.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <asm/cacheflush.h>

#define SPLASH_BASE	0x9d400000UL
#define BAND_BYTES	(1440UL * 4 * 120)	/* stripe; equal black gap follows */

void b1c1_probe_band(int band)
{
	phys_addr_t pa = SPLASH_BASE + band * 2 * BAND_BYTES;
	void __iomem *p;

	if (pfn_valid(__phys_to_pfn(pa))) {
		void *v = phys_to_virt(pa);

		memset(v, 0xff, BAND_BYTES);
		__flush_dcache_area(v, BAND_BYTES);
		mdelay(1000);
		return;
	}
	p = ioremap_wc(pa, BAND_BYTES);
	if (!p)
		return;
	memset_io(p, 0xff, BAND_BYTES);
	wmb();
	iounmap(p);
	mdelay(1000);
}

#define PROBE(level, n)						\
static int __init b1c1_probe_##level(void)			\
{								\
	b1c1_probe_band(n);					\
	return 0;						\
}								\
level##_initcall(b1c1_probe_##level)

PROBE(early, 1);
PROBE(core, 2);
PROBE(postcore, 3);
PROBE(arch, 4);
PROBE(subsys, 5);
PROBE(fs, 6);
PROBE(device, 7);
PROBE(late, 8);
