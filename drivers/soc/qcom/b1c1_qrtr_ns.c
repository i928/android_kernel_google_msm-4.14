// SPDX-License-Identifier: GPL-2.0
/*
 * Start the QRTR name service for the Pixel 3 (b1c1).
 *
 * msm-4.9 used IPC router; on msm-4.14 the vendor QMI libraries pick
 * QRTR at runtime, but this tree has no in-kernel name service (upstream
 * net/qrtr/ns.c is 5.7+) and the Pixel 3 vendor image never starts
 * /vendor/bin/qrtr-ns. Without it every QMI service lookup hangs (sensors
 * HAL, then system_server's watchdog). Start it from the kernel once
 * /vendor is mounted.
 */
#define pr_fmt(fmt) "b1c1_qrtr_ns: " fmt

#include <linux/delay.h>
#include <linux/fs_struct.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/kthread.h>
#include <linux/path.h>
#include <linux/sched/task.h>

#define QRTR_NS_TRIES	400	/* 250 ms apart: 100 s */

/* the helper inherits kthreadd's ramdisk root; use init's instead */
static int qrtr_ns_umh_init(struct subprocess_info *info, struct cred *new)
{
	struct task_struct *init;
	struct path root;

	rcu_read_lock();
	init = find_task_by_vpid(1);
	if (init)
		get_task_struct(init);
	rcu_read_unlock();
	if (!init)
		return -ESRCH;

	get_fs_root(init->fs, &root);
	put_task_struct(init);
	set_fs_root(current->fs, &root);
	set_fs_pwd(current->fs, &root);
	path_put(&root);
	return 0;
}

static int qrtr_ns_start(void)
{
	static char *argv[] = { "/vendor/bin/qrtr-ns", "-f", NULL };
	static char *envp[] = { "PATH=/system/bin:/vendor/bin", NULL };
	struct subprocess_info *info;

	info = call_usermodehelper_setup(argv[0], argv, envp, GFP_KERNEL,
					 qrtr_ns_umh_init, NULL, NULL);
	if (!info)
		return -ENOMEM;
	return call_usermodehelper_exec(info, UMH_WAIT_EXEC);
}

static int qrtr_ns_thread(void *unused)
{
	int i, ret = -ENOENT;

	for (i = 0; i < QRTR_NS_TRIES; i++) {
		ret = qrtr_ns_start();
		if (!ret) {
			pr_info("started\n");
			return 0;
		}
		msleep(250);
	}
	pr_err("could not start /vendor/bin/qrtr-ns: %d\n", ret);
	return ret;
}

static int __init b1c1_qrtr_ns_init(void)
{
	return PTR_ERR_OR_ZERO(kthread_run(qrtr_ns_thread, NULL, "qrtr_ns"));
}
late_initcall(b1c1_qrtr_ns_init);
