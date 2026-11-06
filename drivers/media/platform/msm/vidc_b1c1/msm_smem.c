/* Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/dma-iommu.h>
#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/iommu.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/msm_ion.h>
#include <linux/ion_kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "media/msm_vidc.h"
#include "msm_vidc_debug.h"
#include "msm_vidc_resources.h"

/*
 * msm-4.14 port: the msm-4.9 driver used the legacy ION client API
 * (ion_client / ion_handle). 4.14 ION is dma-buf only, so the "ion handle"
 * is now the dma_buf itself (holding its own reference) and allocations
 * come from ion_alloc() as in the 4.14 vidc. The 4.9 entry points and
 * their get/put pairing are unchanged for the rest of the driver.
 */
struct smem_client {
	int mem_type;
	void *clnt;
	struct msm_vidc_platform_resources *res;
	enum session_type session_type;
	bool tme_encode_mode;
};

static int msm_dma_get_device_address(struct smem_client *smem_client,
		struct dma_buf *buf, unsigned long align,
		dma_addr_t *iova, unsigned long *buffer_size,
		unsigned long flags, enum hal_buffer buffer_type,
		struct dma_mapping_info *mapping_info)
{
	int rc = 0;
	struct dma_buf_attachment *attach;
	struct sg_table *table = NULL;
	struct context_bank_info *cb = NULL;

	if (!iova || !buffer_size || !buf || !smem_client || !mapping_info) {
		dprintk(VIDC_ERR, "Invalid params: %pK, %pK, %pK, %pK\n",
				smem_client, buf, iova, buffer_size);
		return -EINVAL;
	}

	if (!is_iommu_present(smem_client->res)) {
		dprintk(VIDC_ERR, "iommu not present\n");
		return -EINVAL;
	}

	cb = msm_smem_get_context_bank(smem_client, flags & SMEM_SECURE,
			buffer_type);
	if (!cb) {
		dprintk(VIDC_ERR, "%s: Failed to get context bank device\n",
			__func__);
		return -EIO;
	}

	/* Check if the dmabuf size matches expected size */
	if (buf->size < *buffer_size) {
		dprintk(VIDC_ERR,
			"Size mismatch! Dmabuf size: %zu Expected Size: %lu",
			buf->size, *buffer_size);
		msm_vidc_res_handle_fatal_hw_error(smem_client->res, true);
		return -EINVAL;
	}

	attach = dma_buf_attach(buf, cb->dev);
	if (IS_ERR_OR_NULL(attach)) {
		rc = PTR_ERR(attach) ?: -ENOMEM;
		dprintk(VIDC_ERR, "Failed to attach dmabuf\n");
		return rc;
	}

	/* delayed unmap, like msm_dma_map_sg_lazy() on 4.9 */
	attach->dma_map_attrs = DMA_ATTR_DELAYED_UNMAP;
	if (smem_client->res->sys_cache_present)
		attach->dma_map_attrs |= DMA_ATTR_IOMMU_USE_UPSTREAM_HINT;

	table = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(table)) {
		rc = PTR_ERR(table) ?: -ENOMEM;
		dprintk(VIDC_ERR, "Failed to map table\n");
		goto detach;
	}

	if (!table->sgl) {
		dprintk(VIDC_ERR, "sgl is NULL\n");
		rc = -ENOMEM;
		goto unmap;
	}
	*iova = table->sgl->dma_address;
	*buffer_size = table->sgl->dma_length;

	mapping_info->dev = cb->dev;
	mapping_info->mapping = cb->mapping;
	mapping_info->table = table;
	mapping_info->attach = attach;
	mapping_info->buf = buf;

	trace_msm_smem_buffer_iommu_op_end("MAP", 0, 0,
		align, *iova, *buffer_size);
	return 0;

unmap:
	dma_buf_unmap_attachment(attach, table, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(buf, attach);
	return rc;
}

static int msm_dma_put_device_address(struct dma_mapping_info *mapping_info)
{
	if (!mapping_info || !mapping_info->dev || !mapping_info->table ||
		!mapping_info->buf || !mapping_info->attach) {
		dprintk(VIDC_WARN, "%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	trace_msm_smem_buffer_iommu_op_start("UNMAP", 0, 0, 0, 0, 0);
	dma_buf_unmap_attachment(mapping_info->attach,
		mapping_info->table, DMA_BIDIRECTIONAL);
	dma_buf_detach(mapping_info->buf, mapping_info->attach);
	trace_msm_smem_buffer_iommu_op_end("UNMAP", 0, 0, 0, 0, 0);

	mapping_info->dev = NULL;
	mapping_info->mapping = NULL;
	mapping_info->table = NULL;
	mapping_info->attach = NULL;
	mapping_info->buf = NULL;
	return 0;
}

void *msm_smem_get_dma_buf(int fd)
{
	struct dma_buf *dma_buf;

	dma_buf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dma_buf)) {
		dprintk(VIDC_ERR, "Failed to get dma_buf for %d, error %ld\n",
				fd, PTR_ERR(dma_buf));
		dma_buf = NULL;
	}

	return dma_buf;
}

void msm_smem_put_dma_buf(void *dma_buf)
{
	if (!dma_buf) {
		dprintk(VIDC_ERR, "%s: NULL dma_buf\n", __func__);
		return;
	}

	dma_buf_put((struct dma_buf *)dma_buf);
}

/* The "handle" is the dma_buf with an extra reference. */
void *msm_smem_get_handle(struct smem_client *client, void *dma_buf)
{
	if (!client || !dma_buf)
		return NULL;

	get_dma_buf((struct dma_buf *)dma_buf);
	return dma_buf;
}

void msm_smem_put_handle(struct smem_client *client, void *handle)
{
	if (!client || !handle) {
		dprintk(VIDC_ERR, "%s: Invalid params %pK %pK\n",
				__func__, client, handle);
		return;
	}
	dma_buf_put((struct dma_buf *)handle);
}

int msm_smem_map_dma_buf(struct msm_vidc_inst *inst, struct msm_smem *smem)
{
	int rc = 0;
	dma_addr_t iova = 0;
	u32 temp = 0;
	unsigned long buffer_size = 0;
	unsigned long align = SZ_4K;
	unsigned long ion_flags = 0;
	struct dma_buf *dma_buf;

	if (!inst || !inst->mem_client || !smem) {
		dprintk(VIDC_ERR, "%s: Invalid params: %pK %pK\n",
				__func__, inst, smem);
		return -EINVAL;
	}

	if (smem->refcount) {
		smem->refcount++;
		return 0;
	}

	dma_buf = msm_smem_get_dma_buf(smem->fd);
	if (!dma_buf)
		return -EINVAL;

	rc = dma_buf_get_flags(dma_buf, &ion_flags);
	if (rc) {
		dprintk(VIDC_ERR, "Failed to get dma buf flags: %d\n", rc);
		goto put;
	}

	if (ion_flags & ION_FLAG_CACHED)
		smem->flags |= SMEM_CACHED;

	if (ion_flags & ION_FLAG_SECURE)
		smem->flags |= SMEM_SECURE;

	buffer_size = smem->size;
	rc = msm_dma_get_device_address(inst->mem_client, dma_buf,
			align, &iova, &buffer_size, smem->flags,
			smem->buffer_type, &smem->mapping_info);
	if (rc) {
		dprintk(VIDC_ERR, "Failed to get device address: %d\n", rc);
		goto put;
	}
	temp = (u32)iova;
	if ((dma_addr_t)temp != iova) {
		dprintk(VIDC_ERR, "iova(%pa) truncated to %#x", &iova, temp);
		msm_dma_put_device_address(&smem->mapping_info);
		rc = -EINVAL;
		goto put;
	}

	get_dma_buf(dma_buf);
	smem->dma_buf = dma_buf;
	smem->handle = dma_buf;
	smem->device_addr = (u32)iova + smem->offset;
	smem->refcount++;
	return 0;

put:
	dma_buf_put(dma_buf);
	return rc;
}

int msm_smem_unmap_dma_buf(struct msm_vidc_inst *inst, struct msm_smem *smem)
{
	int rc = 0;

	if (!inst || !smem) {
		dprintk(VIDC_ERR, "%s: Invalid params: %pK %pK\n",
				__func__, inst, smem);
		return -EINVAL;
	}

	if (smem->refcount) {
		smem->refcount--;
	} else {
		dprintk(VIDC_WARN,
			"unmap called while refcount is zero already\n");
		return -EINVAL;
	}

	if (smem->refcount)
		return 0;

	rc = msm_dma_put_device_address(&smem->mapping_info);
	if (rc) {
		dprintk(VIDC_ERR, "Failed to put device address: %d\n", rc);
		return rc;
	}

	dma_buf_put(smem->handle);
	dma_buf_put(smem->dma_buf);

	smem->device_addr = 0x0;
	smem->handle = NULL;
	smem->dma_buf = NULL;
	return 0;
}

static int get_secure_flag_for_buffer_type(
		struct smem_client *client, enum hal_buffer buffer_type)
{

	if (!client) {
		dprintk(VIDC_ERR, "%s - invalid params\n", __func__);
		return -EINVAL;
	}

	switch (buffer_type) {
	case HAL_BUFFER_INPUT:
		if (client->session_type == MSM_VIDC_ENCODER)
			return ION_FLAG_CP_PIXEL;
		else
			return ION_FLAG_CP_BITSTREAM;
	case HAL_BUFFER_OUTPUT:
	case HAL_BUFFER_OUTPUT2:
		if (client->session_type == MSM_VIDC_ENCODER)
			return ION_FLAG_CP_BITSTREAM;
		else
			return ION_FLAG_CP_PIXEL;
	case HAL_BUFFER_INTERNAL_SCRATCH:
		return ION_FLAG_CP_BITSTREAM;
	case HAL_BUFFER_INTERNAL_SCRATCH_1:
		return ION_FLAG_CP_NON_PIXEL;
	case HAL_BUFFER_INTERNAL_SCRATCH_2:
		return ION_FLAG_CP_PIXEL;
	case HAL_BUFFER_INTERNAL_PERSIST:
		return ION_FLAG_CP_BITSTREAM;
	case HAL_BUFFER_INTERNAL_PERSIST_1:
		return ION_FLAG_CP_NON_PIXEL;
	default:
		WARN(1, "No matching secure flag for buffer type : %x\n",
				buffer_type);
		return -EINVAL;
	}
}

static int alloc_dma_mem(struct smem_client *client, size_t size, u32 align,
	u32 flags, enum hal_buffer buffer_type, struct msm_smem *mem,
	int map_kernel)
{
	struct dma_buf *dbuf;
	dma_addr_t iova = 0;
	unsigned long buffer_size = 0;
	unsigned long heap_mask = 0;
	int rc = 0;
	int ion_flags = 0;

	if (!client || !mem) {
		dprintk(VIDC_ERR, "%s: Invalid params: %pK %pK\n",
				__func__, client, mem);
		return -EINVAL;
	}

	align = ALIGN(align, SZ_4K);
	size = ALIGN(size, SZ_4K);

	if (is_iommu_present(client->res))
		heap_mask = ION_HEAP(ION_SYSTEM_HEAP_ID);
	else
		heap_mask = ION_HEAP(ION_ADSP_HEAP_ID);

	if (flags & SMEM_CACHED)
		ion_flags |= ION_FLAG_CACHED;

	if (flags & SMEM_SECURE) {
		int secure_flag =
			get_secure_flag_for_buffer_type(client, buffer_type);
		if (secure_flag < 0)
			return secure_flag;

		ion_flags |= ION_FLAG_SECURE | secure_flag;
		heap_mask = ION_HEAP(ION_SECURE_HEAP_ID);

		if (client->res->slave_side_cp) {
			heap_mask = ION_HEAP(ION_CP_MM_HEAP_ID);
			size = ALIGN(size, SZ_1M);
			align = ALIGN(size, SZ_1M);
		}
	}

	dbuf = ion_alloc(size, heap_mask, ion_flags);
	if (IS_ERR_OR_NULL(dbuf)) {
		dprintk(VIDC_ERR,
			"Failed to allocate shared memory = %pK, %zx, %d, %#x\n",
			client, size, align, flags);
		return -ENOMEM;
	}

	mem->dma_buf = dbuf;
	mem->handle = dbuf;
	mem->flags = flags;
	mem->buffer_type = buffer_type;
	mem->offset = 0;
	mem->size = size;
	mem->kvaddr = NULL;

	rc = msm_dma_get_device_address(client, dbuf, align, &iova,
			&buffer_size, flags, buffer_type, &mem->mapping_info);
	if (rc) {
		dprintk(VIDC_ERR, "Failed to get device address: %d\n", rc);
		goto fail_put;
	}
	mem->device_addr = (u32)iova;
	if ((dma_addr_t)mem->device_addr != iova) {
		dprintk(VIDC_ERR, "iova(%pa) truncated to %#x",
			&iova, mem->device_addr);
		rc = -EINVAL;
		goto fail_unmap;
	}

	if (map_kernel) {
		dma_buf_begin_cpu_access(dbuf, DMA_BIDIRECTIONAL);
		mem->kvaddr = dma_buf_vmap(dbuf);
		if (!mem->kvaddr) {
			dprintk(VIDC_ERR,
				"Failed to map shared mem in kernel\n");
			dma_buf_end_cpu_access(dbuf, DMA_BIDIRECTIONAL);
			rc = -EIO;
			goto fail_unmap;
		}
	}

	dprintk(VIDC_DBG,
		"%s: dma_buf = %pK, device_addr = %x, size = %d, kvaddr = %pK, buffer_type = %#x, flags = %#lx\n",
		__func__, mem->dma_buf, mem->device_addr, mem->size,
		mem->kvaddr, mem->buffer_type, mem->flags);
	return 0;

fail_unmap:
	msm_dma_put_device_address(&mem->mapping_info);
	mem->device_addr = 0;
fail_put:
	dma_buf_put(dbuf);
	mem->dma_buf = NULL;
	mem->handle = NULL;
	return rc;
}

static int free_dma_mem(struct smem_client *client, struct msm_smem *mem)
{
	if (!client || !mem) {
		dprintk(VIDC_ERR, "%s: Invalid params: %pK %pK\n",
				__func__, client, mem);
		return -EINVAL;
	}

	if (mem->device_addr) {
		msm_dma_put_device_address(&mem->mapping_info);
		mem->device_addr = 0x0;
	}

	if (mem->kvaddr) {
		dma_buf_vunmap(mem->dma_buf, mem->kvaddr);
		mem->kvaddr = NULL;
		dma_buf_end_cpu_access(mem->dma_buf, DMA_BIDIRECTIONAL);
	}

	if (mem->dma_buf) {
		dma_buf_put(mem->dma_buf);
		mem->dma_buf = NULL;
		mem->handle = NULL;
	}

	return 0;
}

int msm_smem_cache_operations(struct smem_client *client,
		void *handle, unsigned long offset, unsigned long size,
		enum smem_cache_ops cache_op)
{
	struct dma_buf *dbuf = handle;
	unsigned long flags = 0;
	int rc;

	if (!client || !dbuf) {
		dprintk(VIDC_ERR, "%s: Invalid params: %pK %pK\n",
			__func__, client, handle);
		return -EINVAL;
	}

	rc = dma_buf_get_flags(dbuf, &flags);
	if (rc) {
		dprintk(VIDC_ERR, "%s: dma_buf_get_flags failed, err %d\n",
			__func__, rc);
		return rc;
	}
	if (!(flags & ION_FLAG_CACHED))
		return 0;

	switch (cache_op) {
	case SMEM_CACHE_CLEAN:
	case SMEM_CACHE_CLEAN_INVALIDATE:
		rc = dma_buf_begin_cpu_access_partial(dbuf, DMA_TO_DEVICE,
				offset, size);
		if (rc)
			break;
		rc = dma_buf_end_cpu_access_partial(dbuf, DMA_TO_DEVICE,
				offset, size);
		break;
	case SMEM_CACHE_INVALIDATE:
		rc = dma_buf_begin_cpu_access_partial(dbuf, DMA_TO_DEVICE,
				offset, size);
		if (rc)
			break;
		rc = dma_buf_end_cpu_access_partial(dbuf, DMA_FROM_DEVICE,
				offset, size);
		break;
	default:
		dprintk(VIDC_ERR, "%s: cache (%d) operation not supported\n",
			__func__, cache_op);
		rc = -EINVAL;
		break;
	}
	if (rc)
		dprintk(VIDC_ERR, "%s: Failed cache operations: %d\n",
			__func__, rc);
	return rc;
}

void *msm_smem_new_client(enum smem_type mtype,
		void *platform_resources, enum session_type stype)
{
	struct smem_client *client;

	if (mtype != SMEM_ION) {
		dprintk(VIDC_ERR, "Mem type not supported\n");
		return NULL;
	}

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;

	client->mem_type = mtype;
	client->clnt = client;	/* non-NULL: no ION client on 4.14 */
	client->res = platform_resources;
	client->session_type = stype;
	return client;
}

void msm_smem_set_tme_encode_mode(struct smem_client *client, bool enable)
{
	if (!client)
		return;
	client->tme_encode_mode = enable;
}

int msm_smem_alloc(struct smem_client *client, size_t size,
		u32 align, u32 flags, enum hal_buffer buffer_type,
		int map_kernel, struct msm_smem *smem)
{
	int rc;

	if (!client || !smem || !size) {
		dprintk(VIDC_ERR, "%s: Invalid params %pK %pK %d\n",
				__func__, client, smem, (u32)size);
		return -EINVAL;
	}

	rc = alloc_dma_mem(client, size, align, flags, buffer_type,
			smem, map_kernel);
	if (rc)
		dprintk(VIDC_ERR, "Failed to allocate memory\n");
	return rc;
}

int msm_smem_free(void *clt, struct msm_smem *smem)
{
	int rc;

	if (!clt || !smem) {
		dprintk(VIDC_ERR, "Invalid  client/handle passed\n");
		return -EINVAL;
	}

	rc = free_dma_mem(clt, smem);
	if (rc)
		dprintk(VIDC_ERR, "Failed to free memory\n");
	return rc;
}

void msm_smem_delete_client(void *clt)
{
	if (!clt) {
		dprintk(VIDC_ERR, "Invalid  client passed\n");
		return;
	}
	kfree(clt);
}

struct context_bank_info *msm_smem_get_context_bank(void *clt,
			bool is_secure, enum hal_buffer buffer_type)
{
	struct smem_client *client = clt;
	struct context_bank_info *cb = NULL, *match = NULL;

	if (!clt) {
		dprintk(VIDC_ERR, "%s: invalid params\n", __func__);
		return NULL;
	}

	/*
	 * HAL_BUFFER_INPUT is directly mapped to bitstream CB in DT
	 * as the buffer type structure was initially designed
	 * just for decoder. For Encoder, input should be mapped to
	 * pixel CB. So swap the buffer types just in this local scope.
	 */
	if (is_secure && client->session_type == MSM_VIDC_ENCODER) {
		if (buffer_type == HAL_BUFFER_INPUT)
			buffer_type = HAL_BUFFER_OUTPUT;
		else if (buffer_type == HAL_BUFFER_OUTPUT &&
			!client->tme_encode_mode)
			buffer_type = HAL_BUFFER_INPUT;
	}

	list_for_each_entry(cb, &client->res->context_banks, list) {
		if (cb->is_secure == is_secure &&
				cb->buffer_type & buffer_type) {
			match = cb;
			break;
		}
	}
	if (!match)
		dprintk(VIDC_ERR,
			"%s: cb not found for buffer_type %x, is_secure %d\n",
			__func__, buffer_type, is_secure);

	return match;
}
