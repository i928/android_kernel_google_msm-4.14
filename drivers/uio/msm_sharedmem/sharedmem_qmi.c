// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * RFSA (remote filesystem access) QMI service, ported from msm-4.9 to the
 * 4.14 QMI API. The SDM845 modem asks this service for the address of the
 * rmtfs shared buffer; without it the modem crashes as soon as
 * rmt_storage allocates the buffer.
 */

#define DRIVER_NAME "msm_sharedmem"
#define pr_fmt(fmt) DRIVER_NAME ": %s: " fmt, __func__

#include <linux/err.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qmi.h>
#include "sharedmem_qmi.h"

#define RFSA_SERVICE_ID_V01			0x1C
#define RFSA_SERVICE_VERS_V01			0x01
#define RFSA_SERVICE_INSTANCE_NUM		1

#define QMI_RFSA_GET_BUFF_ADDR_REQ_MSG_V01	0x0023
#define QMI_RFSA_GET_BUFF_ADDR_RESP_MSG_V01	0x0023

#define RFSA_GET_BUFF_ADDR_REQ_MSG_MAX_LEN_V01	14
#define RFSA_GET_BUFF_ADDR_RESP_MSG_MAX_LEN_V01	18

struct rfsa_get_buff_addr_req_msg_v01 {
	u32 client_id;
	u32 size;
};

struct rfsa_get_buff_addr_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	u8 address_valid;
	u64 address;
};

static struct qmi_elem_info rfsa_get_buff_addr_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct rfsa_get_buff_addr_req_msg_v01,
					   client_id),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct rfsa_get_buff_addr_req_msg_v01,
					   size),
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static struct qmi_elem_info rfsa_get_buff_addr_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct rfsa_get_buff_addr_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct rfsa_get_buff_addr_resp_msg_v01,
					   address_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u64),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct rfsa_get_buff_addr_resp_msg_v01,
					   address),
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct shared_addr_entry {
	struct list_head node;
	u32 id;
	u64 address;
	u32 size;
};

static LIST_HEAD(sharedmem_list);
static DECLARE_RWSEM(sharedmem_list_lock);
static struct qmi_handle *sharedmem_qmi_svc_handle;

void sharedmem_qmi_add_entry(struct sharemem_qmi_entry *qmi_entry)
{
	struct shared_addr_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		pr_err("Alloc of new list entry failed\n");
		return;
	}

	entry->id = qmi_entry->client_id;
	entry->address = qmi_entry->address;
	entry->size = qmi_entry->size;

	down_write(&sharedmem_list_lock);
	list_add_tail(&entry->node, &sharedmem_list);
	up_write(&sharedmem_list_lock);
}

static int get_buffer_for_client(u32 id, u32 size, u64 *address)
{
	struct shared_addr_entry *entry;
	int result = -ENOENT;

	if (size == 0)
		return -ENOMEM;

	down_read(&sharedmem_list_lock);
	list_for_each_entry(entry, &sharedmem_list, node) {
		if (entry->id != id)
			continue;
		if (entry->size >= size) {
			*address = entry->address;
			result = 0;
		} else {
			pr_err("Shared mem req too large for id=%u\n", id);
			result = -ENOMEM;
		}
		break;
	}
	up_read(&sharedmem_list_lock);

	if (result == -ENOENT)
		pr_err("Unknown client id %u\n", id);

	return result;
}

static void sharedmem_qmi_get_buffer(struct qmi_handle *qmi,
				     struct sockaddr_qrtr *sq,
				     struct qmi_txn *txn, const void *decoded)
{
	const struct rfsa_get_buff_addr_req_msg_v01 *req = decoded;
	struct rfsa_get_buff_addr_resp_msg_v01 resp;
	u64 address = 0;
	int rc;

	memset(&resp, 0, sizeof(resp));

	rc = get_buffer_for_client(req->client_id, req->size, &address);
	if (!rc && address) {
		resp.address_valid = 1;
		resp.address = address;
		resp.resp.result = QMI_RESULT_SUCCESS_V01;
	} else {
		resp.resp.result = QMI_RESULT_FAILURE_V01;
		resp.resp.error = QMI_ERR_NO_MEMORY_V01;
	}
	pr_info("client 0x%x size %u -> %d addr 0x%llx\n",
		req->client_id, req->size, rc, address);

	rc = qmi_send_response(qmi, sq, txn,
			       QMI_RFSA_GET_BUFF_ADDR_RESP_MSG_V01,
			       RFSA_GET_BUFF_ADDR_RESP_MSG_MAX_LEN_V01,
			       rfsa_get_buff_addr_resp_msg_v01_ei, &resp);
	if (rc < 0)
		pr_err("send response failed %d\n", rc);
}

static struct qmi_msg_handler sharedmem_qmi_handlers[] = {
	{
		.type = QMI_REQUEST,
		.msg_id = QMI_RFSA_GET_BUFF_ADDR_REQ_MSG_V01,
		.ei = rfsa_get_buff_addr_req_msg_v01_ei,
		.decoded_size = sizeof(struct rfsa_get_buff_addr_req_msg_v01),
		.fn = sharedmem_qmi_get_buffer,
	},
	{}
};

int sharedmem_qmi_init(void)
{
	int rc;

	if (sharedmem_qmi_svc_handle)
		return 0;

	sharedmem_qmi_svc_handle = kzalloc(sizeof(*sharedmem_qmi_svc_handle),
					   GFP_KERNEL);
	if (!sharedmem_qmi_svc_handle)
		return -ENOMEM;

	rc = qmi_handle_init(sharedmem_qmi_svc_handle,
			     RFSA_GET_BUFF_ADDR_REQ_MSG_MAX_LEN_V01,
			     NULL, sharedmem_qmi_handlers);
	if (rc < 0) {
		pr_err("qmi_handle_init failed %d\n", rc);
		goto free;
	}

	rc = qmi_add_server(sharedmem_qmi_svc_handle, RFSA_SERVICE_ID_V01,
			    RFSA_SERVICE_VERS_V01, RFSA_SERVICE_INSTANCE_NUM);
	if (rc < 0) {
		pr_err("qmi_add_server failed %d\n", rc);
		qmi_handle_release(sharedmem_qmi_svc_handle);
		goto free;
	}

	pr_info("RFSA service registered\n");
	return 0;

free:
	kfree(sharedmem_qmi_svc_handle);
	sharedmem_qmi_svc_handle = NULL;
	return rc;
}

/* probe can run before qrtr is up; retry once the core is initialised */
static int __init sharedmem_qmi_late_init(void)
{
	return sharedmem_qmi_init();
}
late_initcall(sharedmem_qmi_late_init);
