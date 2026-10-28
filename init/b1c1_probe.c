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
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/slab.h>

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
static bool log_mode;

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

void b1c1_probe_text(int line, const char *fmt, ...);

/*
 * The bootloader leaves the APSS watchdog running and it bites ~7 s after
 * kernel entry, before msm_watchdog probes. Stop it so a stuck probe shows
 * its timeout instead of resetting the board.
 */
static void wdog_stop(void)
{
	static bool done;
	void __iomem *wdt;

	if (done)
		return;
	done = true;
	wdt = ioremap(0x17980000, 0x1000);
	if (!wdt)
		return;
	__raw_writel(0, wdt + 0x08);
	iounmap(wdt);
	b1c1_probe_text(5, "WDOG off");
}

void b1c1_probe_band(int band)
{
	unsigned long flags;

	wdog_stop();
	raw_spin_lock_irqsave(&fb_lock, flags);
	if (!log_mode && fb_map())
		fb_fill_rows(band * 2 * STRIPE_ROWS, STRIPE_ROWS, 0xffffffff);
	wmb();
	raw_spin_unlock_irqrestore(&fb_lock, flags);
}

static void fb_draw_str(int x0, int y, int scale, const char *str)
{
	const unsigned char *glyphs = font_vga_8x16.data;
	int i, gy, gx, sy, sx;

	for (i = 0; str[i] && x0 + (i + 1) * 8 * scale <= FB_W; i++) {
		const unsigned char *g = glyphs + (unsigned char)str[i] * 16;

		for (gy = 0; gy < 16; gy++)
			for (gx = 0; gx < 8; gx++) {
				u32 c = (g[gy] & (0x80 >> gx)) ?
					0xffffffff : 0xff000000;
				for (sy = 0; sy < scale; sy++)
					for (sx = 0; sx < scale; sx++)
						writel_relaxed(c, fb +
						  (y + gy * scale + sy) * FB_STRIDE +
						  (x0 + (i * 8 + gx) * scale + sx) * 4);
			}
	}
}

void b1c1_probe_text(int line, const char *fmt, ...)
{
	char buf[TXT_COLS + 1];
	unsigned long flags;
	va_list args;
	int y = TXT_Y0 + line * TXT_LINE_H;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	raw_spin_lock_irqsave(&fb_lock, flags);
	if (log_mode || !fb_map())
		goto out;
	fb_fill_rows(y, 16 * TXT_SCALE, 0xff000000);
	fb_draw_str(TXT_X0, y, TXT_SCALE, buf);
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

/*
 * Screen dmesg: once boot has been going for a while, give the whole
 * panel to the tail of the kernel log, redrawn every few seconds.
 */
#define LOG_SCALE	2
#define LOG_LINE_H	(16 * LOG_SCALE + 1)
#define LOG_LINES	(FB_H / LOG_LINE_H)
#define LOG_COLS	(FB_W / (8 * LOG_SCALE))

static char log_ring[LOG_LINES][LOG_COLS + 1];
static struct kmsg_dumper log_dumper;

static void b1c1_log_draw(bool nolock)
{
	char line[256];
	unsigned long flags;
	size_t len;
	int n = 0, i, first;

	log_dumper.active = true;
	if (nolock)
		kmsg_dump_rewind_nolock(&log_dumper);
	else
		kmsg_dump_rewind(&log_dumper);
	while (nolock ?
	       kmsg_dump_get_line_nolock(&log_dumper, false, line,
					 sizeof(line) - 1, &len) :
	       kmsg_dump_get_line(&log_dumper, false, line,
				  sizeof(line) - 1, &len)) {
		line[len] = 0;
		strim(line);
		strlcpy(log_ring[n % LOG_LINES], line, LOG_COLS + 1);
		n++;
	}
	first = n > LOG_LINES ? n - LOG_LINES : 0;

	raw_spin_lock_irqsave(&fb_lock, flags);
	log_mode = true;
	if (fb_map()) {
		fb_fill_rows(0, FB_H, 0xff000000);
		for (i = first; i < n; i++)
			fb_draw_str(0, (i - first) * LOG_LINE_H, LOG_SCALE,
				    log_ring[i % LOG_LINES]);
		wmb();
	}
	raw_spin_unlock_irqrestore(&fb_lock, flags);
}

static int b1c1_log_thread(void *unused)
{
	msleep(1000);
	for (;;) {
		b1c1_log_draw(false);
		msleep(250);
		/* Warm reboot to fastboot (log is saved in /metadata) */
		if (time_after(jiffies, INITIAL_JIFFIES + 120 * HZ)) {
			pr_emerg("b1c1: 120 s, rebooting to bootloader\n");
			/* skip device_shutdown(): a driver hangs there */
			machine_restart("bootloader");
		}
	}
	return 0;
}

/*
 * Pixel 3 pstore is encrypted with a key only 4.9 userspace loads, so save
 * the whole log to the unencrypted /metadata partition every 5 s instead.
 */
#define SAVE_PATH	"/metadata/b1c1-kmsg.txt"
#define SAVE_BUF	(64 * 1024)
static struct kmsg_dumper save_dumper;

static void b1c1_save_log(char *buf)
{
	struct file *f;
	loff_t pos = 0;
	size_t len, used = 0;

	f = filp_open(SAVE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(f))
		return;
	kmsg_dump_rewind(&save_dumper);
	while (kmsg_dump_get_line(&save_dumper, false, buf + used,
				  SAVE_BUF - used, &len)) {
		used += len;
		if (SAVE_BUF - used < 1024) {
			kernel_write(f, buf, used, &pos);
			used = 0;
		}
	}
	if (used)
		kernel_write(f, buf, used, &pos);
	vfs_fsync(f, 0);
	filp_close(f, NULL);
}

static int b1c1_save_thread(void *unused)
{
	char *buf = kmalloc(SAVE_BUF, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;
	save_dumper.active = true;
	msleep(8000);
	for (;;) {
		b1c1_save_log(buf);
		msleep(5000);
	}
	return 0;
}

static int __init b1c1_log_init(void)
{
	kthread_run(b1c1_log_thread, NULL, "b1c1_log");
	kthread_run(b1c1_save_thread, NULL, "b1c1_save");
	return 0;
}
early_initcall(b1c1_log_init);

static int b1c1_panic(struct notifier_block *nb, unsigned long ev, void *msg)
{
	b1c1_log_draw(true);
	return NOTIFY_DONE;
}

static struct notifier_block b1c1_panic_nb = { .notifier_call = b1c1_panic };

/* Power-off/reboot: put the log on screen and hold it long enough to film */
static int b1c1_reboot(struct notifier_block *nb, unsigned long ev, void *cmd)
{
	pr_emerg("b1c1: %s %s\n", ev == SYS_POWER_OFF ? "POWER OFF" :
		 ev == SYS_HALT ? "HALT" : "REBOOT", cmd ? (char *)cmd : "");
	b1c1_log_draw(false);
	mdelay(5000);
	return NOTIFY_DONE;
}

static struct notifier_block b1c1_reboot_nb = {
	.notifier_call = b1c1_reboot,
	.priority = INT_MAX,
};

static int __init b1c1_panic_init(void)
{
	register_reboot_notifier(&b1c1_reboot_nb);
	atomic_notifier_chain_register(&panic_notifier_list, &b1c1_panic_nb);
	return 0;
}
early_initcall(b1c1_panic_init);
