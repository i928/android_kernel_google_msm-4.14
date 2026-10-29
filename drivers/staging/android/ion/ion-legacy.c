// SPDX-License-Identifier: GPL-2.0
/*
 * Legacy (msm-4.9 and older) ION userspace ABI on top of the dma-buf
 * based ION of msm-4.14.
 *
 * Old vendor blobs (libQSEEComAPI, camera, audio, ...) still issue the
 * handle based ioctls: ALLOC returns a per-open handle, MAP/SHARE turn a
 * handle into a dma-buf fd, IMPORT turns an fd back into a handle, FREE
 * drops it, and the msm cache maintenance requests go through CUSTOM.
 * Each /dev/ion open gets an idr of handles; a handle holds one dma-buf
 * reference and a use count, as importing the same buffer twice returned
 * the same handle in the legacy ABI.
 */

#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ion.h"

typedef int ion_user_handle_t;

struct ion_legacy_allocation_data {
	size_t len;
	size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	ion_user_handle_t handle;
};

struct ion_legacy_fd_data {
	ion_user_handle_t handle;
	int fd;
};

struct ion_legacy_handle_data {
	ion_user_handle_t handle;
};

struct ion_legacy_custom_data {
	unsigned int cmd;
	unsigned long arg;
};

struct ion_legacy_flush_data {
	ion_user_handle_t handle;
	int fd;
	void __user *vaddr;
	unsigned int offset;
	unsigned int length;
};

#define ION_LEGACY_IOC_ALLOC	_IOWR('I', 0, struct ion_legacy_allocation_data)
#define ION_LEGACY_IOC_FREE	_IOWR('I', 1, struct ion_legacy_handle_data)
#define ION_LEGACY_IOC_MAP	_IOWR('I', 2, struct ion_legacy_fd_data)
#define ION_LEGACY_IOC_SHARE	_IOWR('I', 4, struct ion_legacy_fd_data)
#define ION_LEGACY_IOC_IMPORT	_IOWR('I', 5, struct ion_legacy_fd_data)
#define ION_LEGACY_IOC_CUSTOM	_IOWR('I', 6, struct ion_legacy_custom_data)
#define ION_LEGACY_IOC_SYNC	_IOWR('I', 7, struct ion_legacy_fd_data)

/* msm custom commands, passed in ion_legacy_custom_data.cmd */
#define ION_LEGACY_IOC_CLEAN_CACHES \
	_IOWR('M', 0, struct ion_legacy_flush_data)
#define ION_LEGACY_IOC_INV_CACHES \
	_IOWR('M', 1, struct ion_legacy_flush_data)
#define ION_LEGACY_IOC_CLEAN_INV_CACHES \
	_IOWR('M', 2, struct ion_legacy_flush_data)

#ifdef CONFIG_COMPAT
struct compat_ion_legacy_allocation_data {
	compat_size_t len;
	compat_size_t align;
	compat_uint_t heap_id_mask;
	compat_uint_t flags;
	compat_int_t handle;
};

struct compat_ion_legacy_custom_data {
	compat_uint_t cmd;
	compat_ulong_t arg;
};

struct compat_ion_legacy_flush_data {
	compat_int_t handle;
	compat_int_t fd;
	compat_uptr_t vaddr;
	compat_uint_t offset;
	compat_uint_t length;
};

#define COMPAT_ION_LEGACY_IOC_ALLOC \
	_IOWR('I', 0, struct compat_ion_legacy_allocation_data)
#define COMPAT_ION_LEGACY_IOC_CUSTOM \
	_IOWR('I', 6, struct compat_ion_legacy_custom_data)
#define COMPAT_ION_LEGACY_IOC_CLEAN_CACHES \
	_IOWR('M', 0, struct compat_ion_legacy_flush_data)
#define COMPAT_ION_LEGACY_IOC_INV_CACHES \
	_IOWR('M', 1, struct compat_ion_legacy_flush_data)
#define COMPAT_ION_LEGACY_IOC_CLEAN_INV_CACHES \
	_IOWR('M', 2, struct compat_ion_legacy_flush_data)
#endif

struct ion_legacy_client {
	struct mutex lock;
	struct idr handles;
};

struct ion_legacy_handle {
	struct dma_buf *dmabuf;
	unsigned int count;
};

int ion_legacy_open(struct inode *inode, struct file *filp)
{
	struct ion_legacy_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	mutex_init(&client->lock);
	idr_init(&client->handles);
	filp->private_data = client;
	return 0;
}

static int ion_legacy_release_one(int id, void *p, void *data)
{
	struct ion_legacy_handle *handle = p;

	dma_buf_put(handle->dmabuf);
	kfree(handle);
	return 0;
}

int ion_legacy_release(struct inode *inode, struct file *filp)
{
	struct ion_legacy_client *client = filp->private_data;

	idr_for_each(&client->handles, ion_legacy_release_one, NULL);
	idr_destroy(&client->handles);
	kfree(client);
	return 0;
}

/* Takes over the caller's reference to @dmabuf */
static int ion_legacy_handle_add(struct ion_legacy_client *client,
				 struct dma_buf *dmabuf)
{
	struct ion_legacy_handle *handle;
	int id;

	mutex_lock(&client->lock);
	idr_for_each_entry(&client->handles, handle, id) {
		if (handle->dmabuf == dmabuf) {
			handle->count++;
			mutex_unlock(&client->lock);
			dma_buf_put(dmabuf);
			return id;
		}
	}

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle) {
		mutex_unlock(&client->lock);
		dma_buf_put(dmabuf);
		return -ENOMEM;
	}
	handle->dmabuf = dmabuf;
	handle->count = 1;
	/* handle 0 is invalid in the legacy ABI */
	id = idr_alloc(&client->handles, handle, 1, 0, GFP_KERNEL);
	mutex_unlock(&client->lock);
	if (id < 0) {
		dma_buf_put(dmabuf);
		kfree(handle);
	}
	return id;
}

/* Returns a new reference to the handle's dma-buf */
static struct dma_buf *ion_legacy_handle_get(struct ion_legacy_client *client,
					     ion_user_handle_t id)
{
	struct ion_legacy_handle *handle;
	struct dma_buf *dmabuf = ERR_PTR(-EINVAL);

	mutex_lock(&client->lock);
	handle = idr_find(&client->handles, id);
	if (handle) {
		dmabuf = handle->dmabuf;
		get_dma_buf(dmabuf);
	}
	mutex_unlock(&client->lock);
	return dmabuf;
}

static int ion_legacy_handle_free(struct ion_legacy_client *client,
				  ion_user_handle_t id)
{
	struct ion_legacy_handle *handle;

	mutex_lock(&client->lock);
	handle = idr_find(&client->handles, id);
	if (!handle) {
		mutex_unlock(&client->lock);
		return -EINVAL;
	}
	if (--handle->count) {
		mutex_unlock(&client->lock);
		return 0;
	}
	idr_remove(&client->handles, id);
	mutex_unlock(&client->lock);
	dma_buf_put(handle->dmabuf);
	kfree(handle);
	return 0;
}

static int ion_legacy_alloc(struct ion_legacy_client *client, size_t len,
			    unsigned int heap_id_mask, unsigned int flags)
{
	struct dma_buf *dmabuf;

	dmabuf = ion_alloc_dmabuf(len, heap_id_mask, flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	return ion_legacy_handle_add(client, dmabuf);
}

static int ion_legacy_share(struct ion_legacy_client *client,
			    ion_user_handle_t id)
{
	struct dma_buf *dmabuf;
	int fd;

	dmabuf = ion_legacy_handle_get(client, id);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);
	return fd;
}

static int ion_legacy_import(struct ion_legacy_client *client, int fd)
{
	struct dma_buf *dmabuf;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	return ion_legacy_handle_add(client, dmabuf);
}

static struct dma_buf *ion_legacy_flush_buf(struct ion_legacy_client *client,
					    ion_user_handle_t id, int fd)
{
	/* as msm-4.9: a valid handle wins, else import the fd */
	if (id > 0)
		return ion_legacy_handle_get(client, id);
	return dma_buf_get(fd);
}

static int ion_legacy_cache_op(struct ion_legacy_client *client,
			       unsigned int cmd, ion_user_handle_t id, int fd)
{
	struct dma_buf *dmabuf;
	int ret = 0;

	dmabuf = ion_legacy_flush_buf(client, id, fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	switch (_IOC_NR(cmd)) {
	case 0:	/* clean: CPU writes out to memory */
		ret = dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
		break;
	case 1:	/* invalidate: drop stale CPU lines */
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		break;
	case 2:	/* clean and invalidate */
		ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		if (!ret)
			ret = dma_buf_begin_cpu_access(dmabuf,
						       DMA_BIDIRECTIONAL);
		break;
	default:
		ret = -ENOTTY;
	}
	dma_buf_put(dmabuf);
	return ret;
}

static int ion_legacy_sync(int fd)
{
	struct dma_buf *dmabuf;
	int ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_put(dmabuf);
	return ret;
}

static long ion_legacy_custom(struct ion_legacy_client *client,
			      unsigned int cmd, void __user *uarg)
{
	struct ion_legacy_flush_data flush;

	switch (cmd) {
	case ION_LEGACY_IOC_CLEAN_CACHES:
	case ION_LEGACY_IOC_INV_CACHES:
	case ION_LEGACY_IOC_CLEAN_INV_CACHES:
		if (copy_from_user(&flush, uarg, sizeof(flush)))
			return -EFAULT;
		return ion_legacy_cache_op(client, cmd, flush.handle,
					   flush.fd);
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
static long ion_legacy_compat_custom(struct ion_legacy_client *client,
				     unsigned int cmd, void __user *uarg)
{
	struct compat_ion_legacy_flush_data flush;

	switch (cmd) {
	case COMPAT_ION_LEGACY_IOC_CLEAN_CACHES:
	case COMPAT_ION_LEGACY_IOC_INV_CACHES:
	case COMPAT_ION_LEGACY_IOC_CLEAN_INV_CACHES:
		if (copy_from_user(&flush, uarg, sizeof(flush)))
			return -EFAULT;
		return ion_legacy_cache_op(client, cmd, flush.handle,
					   flush.fd);
	default:
		/* some 32-bit callers pass the 64-bit command numbers */
		return ion_legacy_custom(client, cmd, uarg);
	}
}
#endif

bool ion_legacy_cmd(unsigned int cmd)
{
	switch (cmd) {
	case ION_LEGACY_IOC_ALLOC:
	case ION_LEGACY_IOC_FREE:
	case ION_LEGACY_IOC_MAP:
	case ION_LEGACY_IOC_SHARE:
	case ION_LEGACY_IOC_IMPORT:
	case ION_LEGACY_IOC_CUSTOM:
	case ION_LEGACY_IOC_SYNC:
	case ION_LEGACY_IOC_CLEAN_CACHES:
	case ION_LEGACY_IOC_INV_CACHES:
	case ION_LEGACY_IOC_CLEAN_INV_CACHES:
#ifdef CONFIG_COMPAT
	case COMPAT_ION_LEGACY_IOC_ALLOC:
	case COMPAT_ION_LEGACY_IOC_CUSTOM:
	case COMPAT_ION_LEGACY_IOC_CLEAN_CACHES:
	case COMPAT_ION_LEGACY_IOC_INV_CACHES:
	case COMPAT_ION_LEGACY_IOC_CLEAN_INV_CACHES:
#endif
		return true;
	default:
		return false;
	}
}

long ion_legacy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ion_legacy_client *client = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret;

	if (in_compat_syscall())
		uarg = compat_ptr(arg);

	switch (cmd) {
	case ION_LEGACY_IOC_ALLOC:
	{
		struct ion_legacy_allocation_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		ret = ion_legacy_alloc(client, data.len, data.heap_id_mask,
				       data.flags);
		if (ret < 0)
			return ret;
		data.handle = ret;
		if (copy_to_user(uarg, &data, sizeof(data))) {
			ion_legacy_handle_free(client, data.handle);
			return -EFAULT;
		}
		return 0;
	}
	case ION_LEGACY_IOC_FREE:
	{
		struct ion_legacy_handle_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		return ion_legacy_handle_free(client, data.handle);
	}
	case ION_LEGACY_IOC_MAP:
	case ION_LEGACY_IOC_SHARE:
	{
		struct ion_legacy_fd_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		ret = ion_legacy_share(client, data.handle);
		if (ret < 0)
			return ret;
		data.fd = ret;
		if (copy_to_user(uarg, &data, sizeof(data)))
			return -EFAULT;
		return 0;
	}
	case ION_LEGACY_IOC_IMPORT:
	{
		struct ion_legacy_fd_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		ret = ion_legacy_import(client, data.fd);
		if (ret < 0)
			return ret;
		data.handle = ret;
		if (copy_to_user(uarg, &data, sizeof(data))) {
			ion_legacy_handle_free(client, data.handle);
			return -EFAULT;
		}
		return 0;
	}
	case ION_LEGACY_IOC_SYNC:
	{
		struct ion_legacy_fd_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		return ion_legacy_sync(data.fd);
	}
	case ION_LEGACY_IOC_CUSTOM:
	{
		struct ion_legacy_custom_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		return ion_legacy_custom(client, data.cmd,
					 (void __user *)data.arg);
	}
	/* msm-4.9 also took the cache requests directly on the ion fd */
	case ION_LEGACY_IOC_CLEAN_CACHES:
	case ION_LEGACY_IOC_INV_CACHES:
	case ION_LEGACY_IOC_CLEAN_INV_CACHES:
		return ion_legacy_custom(client, cmd, uarg);
#ifdef CONFIG_COMPAT
	case COMPAT_ION_LEGACY_IOC_CLEAN_CACHES:
	case COMPAT_ION_LEGACY_IOC_INV_CACHES:
	case COMPAT_ION_LEGACY_IOC_CLEAN_INV_CACHES:
		return ion_legacy_compat_custom(client, cmd, uarg);
	case COMPAT_ION_LEGACY_IOC_ALLOC:
	{
		struct compat_ion_legacy_allocation_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		ret = ion_legacy_alloc(client, data.len, data.heap_id_mask,
				       data.flags);
		if (ret < 0)
			return ret;
		data.handle = ret;
		if (copy_to_user(uarg, &data, sizeof(data))) {
			ion_legacy_handle_free(client, data.handle);
			return -EFAULT;
		}
		return 0;
	}
	case COMPAT_ION_LEGACY_IOC_CUSTOM:
	{
		struct compat_ion_legacy_custom_data data;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;
		return ion_legacy_compat_custom(client, data.cmd,
						compat_ptr(data.arg));
	}
#endif
	default:
		return -ENOTTY;
	}
}
