// SPDX-License-Identifier: GPL-2.0
/*
 * TEMP b1c1 bring-up probe: draw into the bootloader splash buffer so
 * progress is visible without UART/pstore. Stripes mark boot stages;
 * text lines at the bottom show the current initcall and driver probe.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/font.h>
#include <linux/spinlock.h>
#include <stdarg.h>
#include <linux/notifier.h>

#define SPLASH_BASE	0x9d400000UL
#define FB_W		1440
#define FB_H		2960
#define FB_STRIDE	(FB_W * 4)
#define STRIPE_ROWS	120
#define TXT_SCALE	3
#define TXT_X0		48
#define TXT_Y0		2580
#define TXT_LINE_H	(16 * TXT_SCALE + 8)
#define TXT_COLS	((FB_W - 2 * TXT_X0) / (8 * TXT_SCALE))

static void __iomem *fb;
static DEFINE_RAW_SPINLOCK(fb_lock);

static void __iomem *fb_map(void)
{
	if (!fb)
		fb = ioremap_wc(SPLASH_BASE, FB_STRIDE * FB_H);
	return fb;
}

static void fb_fill_rows(int y, int rows, u32 val)
{
	int r, x;

	for (r = y; r < y + rows && r < FB_H; r++)
		for (x = 0; x < FB_W; x++)
			writel_relaxed(val, fb + r * FB_STRIDE + x * 4);
}

void b1c1_probe_band(int band)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&fb_lock, flags);
	if (fb_map())
		fb_fill_rows(band * 2 * STRIPE_ROWS, STRIPE_ROWS, 0xffffffff);
	wmb();
	raw_spin_unlock_irqrestore(&fb_lock, flags);
}

void b1c1_probe_text(int line, const char *fmt, ...)
{
	const unsigned char *glyphs = font_vga_8x16.data;
	char buf[TXT_COLS + 1];
	unsigned long flags;
	va_list args;
	int y = TXT_Y0 + line * TXT_LINE_H;
	int i, gy, gx, sy, sx;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	raw_spin_lock_irqsave(&fb_lock, flags);
	if (!fb_map())
		goto out;
	fb_fill_rows(y, 16 * TXT_SCALE, 0xff000000);
	for (i = 0; buf[i]; i++) {
		const unsigned char *g = glyphs + (unsigned char)buf[i] * 16;

		for (gy = 0; gy < 16; gy++)
			for (gx = 0; gx < 8; gx++) {
				u32 c = (g[gy] & (0x80 >> gx)) ?
					0xffffffff : 0xff000000;
				for (sy = 0; sy < TXT_SCALE; sy++)
					for (sx = 0; sx < TXT_SCALE; sx++)
						writel_relaxed(c, fb +
						  (y + gy * TXT_SCALE + sy) * FB_STRIDE +
						  (TXT_X0 + (i * 8 + gx) * TXT_SCALE + sx) * 4);
			}
	}
	wmb();
out:
	raw_spin_unlock_irqrestore(&fb_lock, flags);
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

static int b1c1_panic(struct notifier_block *nb, unsigned long ev, void *msg)
{
	b1c1_probe_text(5, "PANIC %s", (char *)msg);
	return NOTIFY_DONE;
}

static struct notifier_block b1c1_panic_nb = { .notifier_call = b1c1_panic };

static int __init b1c1_panic_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &b1c1_panic_nb);
	return 0;
}
early_initcall(b1c1_panic_init);
