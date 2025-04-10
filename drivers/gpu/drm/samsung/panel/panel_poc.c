// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Samsung Electronics Co., Ltd.
 * Gwanghui Lee <gwanghui.lee@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/bug.h>
#include <linux/list.h>

#include "panel.h"
#include "panel_bl.h"
#include "panel_drv.h"
#include "panel_debug.h"
#include "panel_obj.h"
#include "panel_poc.h"
#ifdef CONFIG_USDM_POC_SPI
#include "panel_spi.h"
#endif

static u8 *poc_wr_img;
static u8 *poc_rd_img;

const char * const poc_op[MAX_POC_OP] = {
	[POC_OP_NONE] = "POC_OP_NONE",
	[POC_OP_ERASE] = "POC_OP_ERASE",
	[POC_OP_WRITE] = "POC_OP_WRITE",
	[POC_OP_READ] = "POC_OP_READ",
	[POC_OP_CANCEL] = "POC_OP_CANCEL",
	[POC_OP_CHECKSUM] = "POC_OP_CHECKSUM",
	[POC_OP_CHECKPOC] = "POC_OP_CHECKPOC",
	[POC_OP_SECTOR_ERASE] = "POC_OP_SECTOR_ERASE",
	[POC_OP_BACKUP] = "POC_OP_BACKUP",
	[POC_OP_IMG_WRITE] = "POC_OP_IMG_WRITE",
	[POC_OP_IMG_READ] = "POC_OP_IMG_READ",
	[POC_OP_IMG_READ_TEST] = "POC_OP_IMG_READ_TEST",
	[POC_OP_DIM_WRITE] = "POC_OP_DIM_WRITE",
	[POC_OP_DIM_READ] = "POC_OP_DIM_READ",
	[POC_OP_DIM_CHKSUM] = "POC_OP_DIM_CHKSUM",
	[POC_OP_DIM_VALID] = "POC_OP_DIM_VALID",
	[POC_OP_DIM_READ_TEST] = "POC_OP_DIM_READ_TEST",
	[POC_OP_SELF_TEST] = "POC_OP_SELF_TEST",
	[POC_OP_READ_TEST] = "POC_OP_READ_TEST",
	[POC_OP_WRITE_TEST] = "POC_OP_WRITE_TEST",
	[POC_OP_DIM_READ_FROM_FILE] = "POC_OP_DIM_READ_FROM_FILE",
	[POC_OP_MTP_READ] = "POC_OP_MTP_READ",
	[POC_OP_MCD_READ] = "POC_OP_MCD_READ",
#ifdef CONFIG_USDM_POC_SPI
	[POC_OP_SET_CONN_SRC] = "POC_OP_SET_CONN_SRC",
	[POC_OP_SET_SPI_SPEED] = "POC_OP_SET_SPI_SPEED",
	[POC_OP_READ_SPI_STATUS_REG] = "POC_OP_READ_SPI_STATUS_REG",
#endif
	[POC_OP_INITIALIZE] = "POC_OP_INITIALIZE",
	[POC_OP_UNINITIALIZE] = "POC_OP_UNINITIALIZE",
	[POC_OP_GM2_READ] = "POC_OP_GM2_READ",
};

const char * const str_partition_write_check[] = {
	[PARTITION_WRITE_CHECK_NONE] = "none",
	[PARTITION_WRITE_CHECK_NOK] = "nok",
	[PARTITION_WRITE_CHECK_OK] = "ok",
};

struct seqinfo *find_poc_seq_by_name(struct panel_poc_device *poc_dev, char *seqname)
{
	struct pnobj *pnobj;

	if (!poc_dev) {
		panel_err("poc_dev is null\n");
		return NULL;
	}

	if (list_empty(&poc_dev->seq_list)) {
		panel_err("poc_dev sequence is empty\n");
		return NULL;
	}

	pnobj = pnobj_find_by_name(&poc_dev->seq_list, seqname);
	if (!pnobj)
		return NULL;

	return pnobj_container_of(pnobj, struct seqinfo);
}

bool check_poc_seqtbl_exist(struct panel_poc_device *poc_dev, char *seqname)
{
	struct seqinfo *seq;

	seq = find_poc_seq_by_name(poc_dev, seqname);
	if (!seq)
		return false;

	if (!is_valid_sequence(seq))
		return false;

	return true;
}

int panel_do_poc_seqtbl_by_name_nolock(struct panel_poc_device *poc_dev, char *seqname)
{
	struct seqinfo *seq;

	if (!poc_dev) {
		panel_err("poc is null\n");
		return -EINVAL;
	}

	seq = find_poc_seq_by_name(poc_dev, seqname);
	if (!seq)
		return -EINVAL;

	return execute_sequence_nolock(to_panel_device(poc_dev), seq);
}

int panel_do_poc_seqtbl_by_name(struct panel_poc_device *poc_dev, char *seqname)
{
	int ret = 0;
	struct panel_device *panel;

	if (!poc_dev)
		return -EINVAL;

	panel = to_panel_device(poc_dev);
	panel_mutex_lock(&panel->op_lock);
	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, seqname);
	panel_mutex_unlock(&panel->op_lock);

	return ret;
}

static int poc_get_poc_chksum(struct panel_device *panel)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	int ret;

	if (sizeof(poc_info->poc_chksum) != PANEL_POC_CHKSUM_LEN) {
		panel_err("invalid poc control length\n");
		return -EINVAL;
	}

	ret = panel_resource_copy(panel, poc_info->poc_chksum, "poc_chksum");
	if (unlikely(ret < 0)) {
		panel_err("failed to copy resource(poc_chksum)\n");
		return ret;
	}

	panel_info("poc_chksum 0x%02X 0x%02X 0x%02X 0x%02X, result %d\n",
			poc_info->poc_chksum[0], poc_info->poc_chksum[1],
			poc_info->poc_chksum[2], poc_info->poc_chksum[3],
			poc_info->poc_chksum[4]);

	return 0;
}

#ifdef CONFIG_USDM_POC_SPI
int _spi_poc_erase(struct panel_device *panel, int addr, int len)
{
	struct panel_spi_dev *spi_dev = &panel->panel_spi_dev;
	struct spi_data_packet data_packet = { 0, };
	int ret = 0;

	if (unlikely(!SPI_IS_READY(spi_dev))) {
		panel_err("spi dev is not ready\n");
		return -EINVAL;
	}

	if (addr % POC_PAGE > 0 || addr < 0) {
		panel_err("failed to start erase. invalid addr\n");
		return -EINVAL;
	}

	if (len <= 0) {
		panel_err("failed to start erase. invalid len %d\n", len);
		return -EINVAL;
	}

	panel_info("++ addr 0x%06X(%d), 0x%X(%d) bytes\n", addr, addr, len, len);

	data_packet.addr = addr;
	data_packet.size = len;

	ret = spi_dev->ops->erase(spi_dev, &data_packet);
	if (ret != 0) {
		panel_err("failed to erase addr: 0x%06x size %d ret %d\n", data_packet.addr, data_packet.size, ret);
		ret = -EIO;
	}

	panel_info("-- ret %d\n", ret);
	return ret;
}
#endif

int _dsi_poc_erase(struct panel_device *panel, int addr, int len)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	int ret, sz_block = 0, erased_size = 0;
	char *erase_seq;

	if (addr % POC_PAGE > 0) {
		panel_err("failed to start erase. invalid addr\n");
		return -EINVAL;
	}

	if (len < 0 || addr + len > get_poc_partition_size(poc_dev, POC_IMG_PARTITION)) {
		panel_err("failed to start erase. range exceeded\n");
		return -EINVAL;
	}
	len = ALIGN(len, SZ_4K);

	panel_info("poc erase +++, 0x%x, %d\n", addr, len);

	panel_mutex_lock(&panel->op_lock);
	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_ERASE_ENTER_SEQ);
	if (unlikely(ret < 0)) {
		panel_err("failed to poc-erase-enter-seq\n");
		goto out_poc_erase;
	}
	while (len > erased_size) {
		if ((len >= erased_size + SZ_64K) &&
				check_poc_seqtbl_exist(poc_dev, POC_ERASE_64K_SEQ)) {
			erase_seq = POC_ERASE_64K_SEQ;
			sz_block = SZ_64K;
		} else if ((len >= erased_size + SZ_32K) &&
				check_poc_seqtbl_exist(poc_dev, POC_ERASE_32K_SEQ)) {
			erase_seq = POC_ERASE_32K_SEQ;
			sz_block = SZ_32K;
		} else {
			erase_seq = POC_ERASE_4K_SEQ;
			sz_block = SZ_4K;
		}

		poc_info->waddr = addr + erased_size;
		ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, erase_seq);
		if (unlikely(ret < 0)) {
			panel_err("failed to poc-erase-seq 0x%x\n", addr + erased_size);
			goto out_poc_erase;
		}
		panel_info("erased addr %06X, sz_block %06X\n",
				addr + erased_size, sz_block);

		if (atomic_read(&poc_dev->cancel)) {
			panel_err("stopped by user at erase 0x%x\n", erased_size);
			goto cancel_poc_erase;
		}
		erased_size += sz_block;
	}

	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_ERASE_EXIT_SEQ);
	if (unlikely(ret < 0)) {
		panel_err("failed to poc-erase-exit-seq\n");
		goto out_poc_erase;
	}

	panel_mutex_unlock(&panel->op_lock);
	panel_info("poc erase ---\n");
	return 0;

cancel_poc_erase:
	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_ERASE_EXIT_SEQ);
	if (unlikely(ret < 0))
		panel_err("failed to poc-erase-exit-seq\n");
	ret = -EIO;
	atomic_set(&poc_dev->cancel, 0);

out_poc_erase:
	panel_mutex_unlock(&panel->op_lock);
	return ret;
}

int poc_erase(struct panel_device *panel, int addr, int len)
{
#ifdef CONFIG_USDM_POC_SPI
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;

	if (poc_info->conn_src == POC_CONN_SRC_SPI)
		return _spi_poc_erase(panel, addr, len);
#endif
	return _dsi_poc_erase(panel, addr, len);
}

#ifdef CONFIG_USDM_POC_SPI
static int _spi_poc_read_data(struct panel_device *panel, u8 *buf, u32 addr, u32 len)
{
	struct panel_spi_dev *spi_dev = &panel->panel_spi_dev;
	struct spi_data_packet data_packet = { 0, };
	int i, ret = 0;
	int spi_buffer_size;
	u8 *spi_buffer;
	int read_len;

	if (unlikely(!SPI_IS_READY(spi_dev))) {
		panel_err("spi dev is not ready\n");
		return -EINVAL;
	}

	if (len <= 0) {
		panel_err("failed to start read. invalid len %d\n", len);
		return -EINVAL;
	}

	panel_info("++ addr 0x%06X(%d), 0x%X(%d) bytes\n", addr, addr, len, len);

	spi_buffer_size = spi_dev->ops->get_buf_size(spi_dev, PANEL_SPI_GET_READ_SIZE);
	if (spi_buffer_size < 1) {
		panel_err("got invalid buffer size %d\n", spi_buffer_size);
		return -EINVAL;
	}
	if (spi_buffer_size > SZ_4K) {
		panel_warn("buffer size set to %d->%d\n", spi_buffer_size, SZ_4K);
		spi_buffer_size = SZ_4K;
	}

	spi_buffer = (u8 *)devm_kzalloc(panel->dev, spi_buffer_size * sizeof(u8), GFP_KERNEL);
	if (!spi_buffer) {
		panel_err("memory allocation failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < len;) {
		memset(spi_buffer, 0, sizeof(spi_buffer_size));
		read_len = spi_buffer_size;
		if (read_len > len - i)
			read_len = len - i;

		data_packet.addr = addr + i;
		data_packet.buf = spi_buffer;
		data_packet.size = read_len;

		ret = spi_dev->ops->read(spi_dev, &data_packet);
		if (ret != 0) {
			panel_err("failed to read addr: 0x%06x size %d ret %d\n", data_packet.addr, data_packet.size, ret);
			ret = -EIO;
			goto error_exit;
		}

		memcpy(&buf[i], spi_buffer, read_len);
		panel_dbg("[%06X] 0x%02X, len %d\n", data_packet.addr, buf[i], data_packet.size);

		i += read_len;
	}
error_exit:
	panel_info("-- ret %d\n", ret);

	devm_kfree(panel->dev, spi_buffer);
	return ret;
}
#endif

static int _dsi_poc_read_data(struct panel_device *panel,
		u8 *buf, u32 addr, u32 len)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	int i, ret = 0;
	u32 poc_addr;

	panel_info("poc read addr 0x%06X, %d(0x%X) bytes +++\n", addr, len, len);

	ret = poc_get_poc_chksum(panel);
	if (unlikely(ret < 0)) {
		panel_err("failed to read poc cheksum seq\n");
		goto exit;
	}

	panel_mutex_lock(&panel->op_lock);
	poc_info->state = POC_STATE_RD_PROGRESS;

	if (poc_info->poc_chksum[0] != 0x00 || poc_info->poc_chksum[1] != 0x00) {
		ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_READ_PRE_ENTER_SEQ);
		if (unlikely(ret < 0)) {
			panel_err("failed to read poc-rd-pre-enter seq\n");
			goto out_poc_read;
		}
	}

	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_READ_ENTER_SEQ);
	if (unlikely(ret < 0)) {
		panel_err("failed to read poc-rd-enter seq\n");
		goto out_poc_read;
	}

	for (i = 0; i < len; i++) {
		if (atomic_read(&poc_dev->cancel)) {
			panel_err("stopped by user at %d bytes\n", i);
			goto cancel_poc_read;
		}

		poc_addr = addr + i;
		poc_info->raddr = poc_addr;
		ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_READ_DAT_SEQ);
		if (unlikely(ret < 0)) {
			panel_err("failed to read poc-rd-dat seq\n");
			goto out_poc_read;
		}

		ret = panel_resource_copy(panel, &buf[i], "poc_data");
		if (unlikely(ret < 0)) {
			panel_err("failed to copy resource(poc_data)\n");
			goto out_poc_read;
		}

		if ((i % 4096) == 0)
			panel_info("[%04d] addr %06X %02X\n", i, poc_addr, buf[i]);
	}

	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_READ_EXIT_SEQ);
	if (unlikely(ret < 0)) {
		panel_err("failed to read poc-rd-exit seq\n");
		goto out_poc_read;
	}

	panel_info("poc read addr 0x%06X, %d(0x%X) bytes ---\n", addr, len, len);
	poc_info->state = POC_STATE_RD_COMPLETE;
	panel_mutex_unlock(&panel->op_lock);

	return 0;

cancel_poc_read:
	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_READ_EXIT_SEQ);
	if (unlikely(ret < 0))
		panel_err("failed to read poc-rd-exit seq\n");
	ret = -EIO;
	atomic_set(&poc_dev->cancel, 0);

out_poc_read:
	poc_info->state = POC_STATE_RD_FAILED;
	panel_mutex_unlock(&panel->op_lock);
exit:
	return ret;
}

int poc_read_data(struct panel_device *panel, u8 *buf, u32 addr, u32 len)
{
#ifdef CONFIG_USDM_POC_SPI
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;

	if (poc_info->conn_src == POC_CONN_SRC_SPI)
		return _spi_poc_read_data(panel, buf, addr, len);
#endif
	return _dsi_poc_read_data(panel, buf, addr, len);
}

#ifdef CONFIG_USDM_POC_SPI
static int _spi_poc_write_data(struct panel_device *panel, u8 *data, u32 addr, u32 size)
{
	struct panel_spi_dev *spi_dev = &panel->panel_spi_dev;
	struct spi_data_packet data_packet = { 0, };
	int i, ret = 0;
	int spi_buffer_size;
	int copy_len;

	if (unlikely(!SPI_IS_READY(spi_dev))) {
		panel_err("spi dev is not ready\n");
		return -EINVAL;
	}

	if (addr % POC_PAGE > 0) {
		panel_err("failed to start write. invalid addr\n");
		return -EINVAL;
	}

	panel_info("++ addr 0x%06X(%d), 0x%X(%d) bytes\n", addr, addr, size, size);

	spi_buffer_size = spi_dev->ops->get_buf_size(spi_dev, PANEL_SPI_GET_WRITE_SIZE);
	for (i = 0; i < size;) {
		copy_len = size - i;
		if (copy_len > spi_buffer_size)
			copy_len = spi_buffer_size;

		data_packet.addr = addr + i;
		data_packet.buf = data + i;
		data_packet.size = copy_len;

		ret = spi_dev->ops->write(spi_dev, &data_packet);
		if (ret != 0) {
			panel_err("failed to write addr: 0x%06x size %d ret %d\n", data_packet.addr, data_packet.size, ret);
			ret = -EIO;
			break;
		}

		panel_dbg("[%06X] 0x%02X, len %d\n", data_packet.addr, data_packet.buf, data_packet.size);
		i += copy_len;
	}

	panel_info("-- ret %d\n", ret);
	return ret;
}
#endif

static int _dsi_poc_write_data(struct panel_device *panel, u8 *data, u32 addr, u32 size)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	int i, ret = 0;
	int copy_len;
	u32 poc_addr;
	bool write_stt_seq_exist;
	bool write_end_seq_exist;

	write_stt_seq_exist = check_poc_seqtbl_exist(poc_dev, POC_WRITE_STT_SEQ);
	write_end_seq_exist = check_poc_seqtbl_exist(poc_dev, POC_WRITE_END_SEQ);

	poc_info->wdata = (u8 *)devm_kzalloc(panel->dev, poc_info->wdata_len * sizeof(u8), GFP_KERNEL);
	if (!poc_info->wdata) {
		panel_err("memory allocation failed\n");
		return -ENOMEM;
	}

	panel_mutex_lock(&panel->op_lock);
	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_WRITE_ENTER_SEQ);
	if (unlikely(ret < 0)) {
		panel_err("failed to read poc-wr-enter-seq\n");
		goto out_poc_write;
	}

	for (i = 0; i < size;) {
		if (atomic_read(&poc_dev->cancel)) {
			panel_err("stopped by user at %d bytes\n", i);
			goto cancel_poc_write;
		}
		poc_addr = addr + i;
		poc_info->waddr = poc_addr;
		if (write_stt_seq_exist &&
			(i == 0 || (poc_addr & 0xFF) == 0)) {
			ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_WRITE_STT_SEQ);
			if (unlikely(ret < 0)) {
				panel_err("failed to write poc-wr-stt seq\n");
				goto out_poc_write;
			}
		}

		memset(poc_info->wdata, 0x00, poc_info->wdata_len);
		copy_len = size - i;
		if (copy_len > poc_info->wdata_len)
			copy_len = poc_info->wdata_len;
		memcpy(poc_info->wdata, data + i, copy_len);

		ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_WRITE_DAT_SEQ);
		if (unlikely(ret < 0)) {
			panel_err("failed to write poc-wr-img seq\n");
			goto out_poc_write;
		}

		if ((i % 4096) == 0)
			panel_info("addr %06X %02X\n", poc_addr, data[i]);
		if (write_end_seq_exist &&
			((poc_addr & 0xFF) == 0xFF || i == (size - 1))) {
			ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_WRITE_END_SEQ);
			if (unlikely(ret < 0)) {
				panel_err("failed to write poc-wr-exit seq\n");
				goto out_poc_write;
			}
		}
		i += copy_len;
	}

	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_WRITE_EXIT_SEQ);
	if (unlikely(ret < 0)) {
		panel_err("failed to write poc-wr-exit seq\n");
		goto out_poc_write;
	}

	panel_info("poc write addr 0x%06X, %d(0x%X) bytes\n", addr, size, size);
	panel_mutex_unlock(&panel->op_lock);
	if (poc_info->wdata)
		devm_kfree(panel->dev, poc_info->wdata);

	return 0;

cancel_poc_write:
	ret = panel_do_poc_seqtbl_by_name_nolock(poc_dev, POC_WRITE_EXIT_SEQ);
	if (unlikely(ret < 0))
		panel_err("failed to read poc-wr-exit seq\n");
	ret = -EIO;
	atomic_set(&poc_dev->cancel, 0);

out_poc_write:
	panel_mutex_unlock(&panel->op_lock);

	if (poc_info->wdata)
		devm_kfree(panel->dev, poc_info->wdata);

	return ret;
}

int poc_write_data(struct panel_device *panel, u8 *data, u32 addr, u32 size)
{
#ifdef CONFIG_USDM_POC_SPI
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;

	if (poc_info->conn_src == POC_CONN_SRC_SPI)
		return _spi_poc_write_data(panel, data, addr, size);
#endif
	return _dsi_poc_write_data(panel, data, addr, size);
}

int poc_memory_initialize(struct panel_device *panel)
{
	int ret = 0;
#ifdef CONFIG_USDM_POC_SPI
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_spi_dev *spi_dev = &panel->panel_spi_dev;

	if (poc_info->conn_src != POC_CONN_SRC_SPI)
		return ret;

	ret = spi_dev->ops->init(spi_dev);
	if (ret != 0) {
		panel_err("failed to initialize memory %d\n", ret);
		return -EIO;
	}
#endif
	return ret;
}

int poc_memory_uninitialize(struct panel_device *panel)
{
	int ret = 0;
#ifdef CONFIG_USDM_POC_SPI
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_spi_dev *spi_dev = &panel->panel_spi_dev;

	if (poc_info->conn_src != POC_CONN_SRC_SPI)
		return ret;

	ret = spi_dev->ops->exit(spi_dev);
	if (ret != 0) {
		panel_err("failed to uninitialize memory %d\n", ret);
		return -EIO;
	}
#endif
	return ret;

}

static int poc_get_octa_poc(struct panel_device *panel)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	u8 octa_id[PANEL_OCTA_ID_LEN] = { 0, };
	int ret;

	ret = panel_resource_copy(panel, octa_id, "octa_id");
	if (unlikely(ret < 0)) {
		panel_err("failed to copy resource(octa_id) (ret %d)\n", ret);
		return ret;
	}
	poc_info->poc = octa_id[1] & 0x0F;

	panel_info("poc %d\n", poc_info->poc);

	return 0;
}

static int poc_get_poc_ctrl(struct panel_device *panel)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	int ret;

	if (sizeof(poc_info->poc_ctrl) != PANEL_POC_CTRL_LEN) {
		panel_err("invalid poc control length\n");
		return -EINVAL;
	}

	panel_mutex_lock(&panel->op_lock);
	panel_set_key(panel, 3, true);
	ret = panel_resource_update_by_name(panel, "poc_ctrl");
	panel_set_key(panel, 3, false);
	panel_mutex_unlock(&panel->op_lock);
	if (unlikely(ret < 0)) {
		panel_err("failed to update resource(poc_ctrl)\n");
		return ret;
	}

	ret = panel_resource_copy(panel, poc_info->poc_ctrl, "poc_ctrl");
	if (unlikely(ret < 0)) {
		panel_err("failed to copy resource(poc_ctrl)\n");
		return ret;
	}

	panel_info("poc_ctrl 0x%02X 0x%02X 0x%02X 0x%02X\n",
			poc_info->poc_ctrl[0], poc_info->poc_ctrl[1],
			poc_info->poc_ctrl[2], poc_info->poc_ctrl[3]);

	return 0;
}

static int poc_data_backup(struct panel_device *panel, u8 *buf, int size, char *filename)
{
	/* unsupported function */
	return -1;
}

static int read_poc_partition_magicnum(struct panel_poc_device *poc_dev, int index)
{
	struct panel_device *panel = to_panel_device(poc_dev);
	u32 value = 0;
	int ret;

	if (!IS_PANEL_ACTIVE(panel))
		return -EAGAIN;

	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	if (poc_dev->partition[index].magicnum_size != 0) {
		ret = poc_read_data(panel, (u8 *)&value,
				poc_dev->partition[index].magicnum_addr,
				poc_dev->partition[index].magicnum_size);

		if (unlikely(ret < 0)) {
			panel_err("failed to read poc data\n");
			return ret;
		}

		poc_dev->partition[index].magicnum_by_read = value;
		poc_dev->partition[index].write_check =
			(poc_dev->partition[index].magicnum ==
			 poc_dev->partition[index].magicnum_by_read) ?
			PARTITION_WRITE_CHECK_OK : PARTITION_WRITE_CHECK_NOK;
		poc_dev->partition[index].cache[PARTITION_REGION_MAGIC] = true;
	} else {
		panel_info("partition[%d] has no magicnum. write_check set to true\n", index);
		poc_dev->partition[index].write_check = PARTITION_WRITE_CHECK_OK;
		poc_dev->partition[index].cache[PARTITION_REGION_MAGIC] = true;
	}

	return 0;
}

static int read_poc_partition_chksum(struct panel_poc_device *poc_dev, int index)
{
	struct panel_device *panel = to_panel_device(poc_dev);
	int ret;

	if (!IS_PANEL_ACTIVE(panel))
		return -EAGAIN;

	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	if (poc_dev->partition[index].checksum_size != 0) {
		ret = poc_read_data(panel,
				&poc_rd_img[poc_dev->partition[index].checksum_addr],
				poc_dev->partition[index].checksum_addr,
				poc_dev->partition[index].checksum_size);

		if (unlikely(ret < 0)) {
			panel_err("failed to read poc data\n");
			return ret;
		}

		poc_dev->partition[index].chksum_by_read =
			ntohs(*(u16 *)&poc_rd_img[poc_dev->partition[index].checksum_addr]);
		poc_dev->partition[index].cache[PARTITION_REGION_CHKSUM] = true;
	}

	return 0;
}

static int read_poc_partition_data(struct panel_poc_device *poc_dev, int index)
{
	struct panel_device *panel = to_panel_device(poc_dev);
	u16 chksum = 0;
	int i, k, ret;

	if (!IS_PANEL_ACTIVE(panel))
		return -EAGAIN;

	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}


	for (k = 0; k < MAX_POC_PARTITION_DATA; k++) {
		if (poc_dev->partition[index].data[k].data_size != 0) {
			ret = poc_read_data(panel,
					&poc_rd_img[poc_dev->partition[index].data[k].data_addr],
					poc_dev->partition[index].data[k].data_addr,
					poc_dev->partition[index].data[k].data_size);

			if (unlikely(ret < 0)) {
				panel_err("failed to read poc data\n");
				return ret;
			}

			panel_info("partition %d data %d addr 0x%06x size %d\n", index, k,
				poc_dev->partition[index].data[k].data_addr, poc_dev->partition[index].data[k].data_size);
			for (i = 0; i < poc_dev->partition[index].data[k].data_size; i++)
				chksum += poc_rd_img[poc_dev->partition[index].data[k].data_addr + i];
		}
	}
	poc_dev->partition[index].chksum_by_calc = chksum;
	poc_dev->partition[index].cache[PARTITION_REGION_DATA] = true;

	return 0;
}

int read_poc_partition_region(struct panel_poc_device *poc_dev, int index, int region, bool force)
{
	int ret = 0;

	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	if (unlikely(region >= MAX_PARTITION_REGION)) {
		panel_err("invalid partition region %d\n", region);
		return -EINVAL;
	}

	if (force || poc_dev->partition[index].cache[region] == false) {
		if (region == PARTITION_REGION_MAGIC)
			ret = read_poc_partition_magicnum(poc_dev, index);
		else if (region == PARTITION_REGION_CHKSUM)
			ret = read_poc_partition_chksum(poc_dev, index);
		else if (region == PARTITION_REGION_DATA)
			ret = read_poc_partition_data(poc_dev, index);

		if (unlikely(ret < 0)) {
			panel_err("failed to read data (partition:%d, region:%d, ret:%d)\n",
					index, region, ret);
			return ret;
		}
	}

	return 0;
}

int read_poc_partition(struct panel_poc_device *poc_dev, int index)
{
	struct panel_device *panel = to_panel_device(poc_dev);
	int ret, region;

	if (!IS_PANEL_ACTIVE(panel))
		return -EAGAIN;

	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	poc_dev->partition[index].preload_done = false;
	poc_dev->partition[index].chksum_ok = false;
	poc_dev->partition[index].chksum_by_calc = 0;
	poc_dev->partition[index].chksum_by_read = 0;
	poc_dev->partition[index].write_check = PARTITION_WRITE_CHECK_NONE;
	memset(poc_dev->partition[index].cache, 0,
			sizeof(poc_dev->partition[index].cache));

	for (region = 0; region < MAX_PARTITION_REGION; region++) {
		ret = read_poc_partition_region(poc_dev, index, region, true);
		if (unlikely(ret < 0)) {
			panel_err("failed to read data (partition:%d, region:%d, ret:%d)\n",
					index, region, ret);
			goto err_read;
		}
	}

//	print_hex_dump(KERN_ERR, "read_poc_partition ", DUMP_PREFIX_ADDRESS, 16, 1, poc_rd_img + poc_dev->partition[index].addr, poc_dev->partition[index].size, false);

	poc_dev->partition[index].preload_done = true;
	poc_dev->partition[index].chksum_ok =
		(poc_dev->partition[index].chksum_by_calc ==
		 poc_dev->partition[index].chksum_by_read);

	panel_info("read partition[%d] chksum:%s(%04X,%04X), magic:%s(%X,%X)\n",
			index, poc_dev->partition[index].chksum_ok ? "OK" : "NOK",
			poc_dev->partition[index].chksum_by_calc,
			poc_dev->partition[index].chksum_by_read,
			str_partition_write_check[poc_dev->partition[index].write_check],
			poc_dev->partition[index].magicnum,
			poc_dev->partition[index].magicnum_by_read);

	return 0;

err_read:
	return ret;
}

int get_poc_partition_addr(struct panel_poc_device *poc_dev, int index)
{
	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	return poc_dev->partition[index].addr;
}

int get_poc_partition_size(struct panel_poc_device *poc_dev, int index)
{
	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	return poc_dev->partition[index].size;
}

int check_poc_partition_exists(struct panel_poc_device *poc_dev, int index)
{
	int ret;

	ret = read_poc_partition_region(poc_dev,
			index, PARTITION_REGION_MAGIC, false);
	if (unlikely(ret < 0)) {
		panel_err("failed to read magic (partition:%d, ret:%d)\n", index, ret);
		return ret;
	}

	return poc_dev->partition[index].write_check;
}
EXPORT_SYMBOL(check_poc_partition_exists);

int get_poc_partition_chksum(struct panel_poc_device *poc_dev, int index,
		u32 *chksum_ok, u32 *chksum_by_calc, u32 *chksum_by_read)
{
	int ret;

	ret = read_poc_partition_region(poc_dev,
			index, PARTITION_REGION_DATA, false);
	if (unlikely(ret < 0)) {
		panel_err("failed to read data (partition:%d, ret:%d)\n", index, ret);
		return ret;
	}

	ret = read_poc_partition_region(poc_dev,
			index, PARTITION_REGION_CHKSUM, false);
	if (unlikely(ret < 0)) {
		panel_err("failed to read chksum (partition:%d, ret:%d)\n", index, ret);
		return ret;
	}

	*chksum_ok = poc_dev->partition[index].chksum_ok;
	*chksum_by_read = poc_dev->partition[index].chksum_by_read;
	*chksum_by_calc = poc_dev->partition[index].chksum_by_calc;

	return 0;
}
EXPORT_SYMBOL(get_poc_partition_chksum);

int check_poc_partition_chksum(struct panel_poc_device *poc_dev, int index)
{
	int ret;
	int chksum_ok;
	int chksum_by_read;
	int chksum_by_calc;

	ret = get_poc_partition_chksum(poc_dev, index,
			&chksum_ok, &chksum_by_calc, &chksum_by_read);
	if (unlikely(ret < 0)) {
		panel_err("failed to get chksum (partition:%d, ret:%d)\n", index, ret);
		return ret;
	}

	return chksum_ok;
}

int cmp_poc_partition_data(struct panel_poc_device *poc_dev,
		int partition_index, int data_area_index, u8 *buf, u32 size)
{
	int ret = 0;

	if (unlikely(partition_index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", partition_index);
		return -EINVAL;
	}

	if (unlikely(data_area_index >= MAX_POC_PARTITION_DATA)) {
		panel_err("invalid partition data area index %d\n", data_area_index);
		return -EINVAL;
	}

	ret = read_poc_partition_region(poc_dev,
			partition_index, PARTITION_REGION_DATA, false);
	if (unlikely(ret < 0)) {
		panel_err("failed to read data (partition:%d, data_area %d, ret:%d)\n",
			partition_index, data_area_index, ret);
		return ret;
	}

	return (size != poc_dev->partition[partition_index].data[data_area_index].data_size) ||
			memcmp(&poc_rd_img[poc_dev->partition[partition_index].data[data_area_index].data_addr],
					buf, size);
}

int copy_poc_partition(struct panel_poc_device *poc_dev, u8 *dst,
		 int index, int offset, int size)
{
	if (unlikely(index >= poc_dev->nr_partition)) {
		panel_err("invalid partition index %d\n", index);
		return -EINVAL;
	}

	if (unlikely(offset + size > poc_dev->partition[index].size)) {
		panel_err("invalid offset %d size %d\n", offset, size);
		return -EINVAL;
	}

	if (!poc_dev->partition[index].preload_done) {
		panel_err("partition(%d) is not loaded\n", index);
		return -EINVAL;
	}

	if (!poc_dev->partition[index].chksum_ok) {
		panel_err("partition(%d) checksum error(calc:%04X read:%04X)\n",
				index, poc_dev->partition[index].chksum_by_calc,
				poc_dev->partition[index].chksum_by_read);
	}

	memcpy(dst, poc_rd_img + poc_dev->partition[index].addr + offset, size);
	return size;
}

int set_panel_poc(struct panel_poc_device *poc_dev, u32 cmd, void *arg)
{
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_device *panel = to_panel_device(poc_dev);
	int ret = 0;
	struct timespec64 cur_ts, last_ts, delta_ts;
	s64 elapsed_msec;
	int addr = -1, len = -1, index;
	int partition_size, partition_addr;

	if (cmd >= MAX_POC_OP) {
		panel_err("invalid poc_op %d\n", cmd);
		return -EINVAL;
	}

	panel_info("%s +\n", poc_op[cmd]);
	ktime_get_ts64(&last_ts);

	switch (cmd) {
	case POC_OP_ERASE:
		break;
	case POC_OP_WRITE:
		ret = poc_write_data(panel, &poc_info->wbuf[poc_info->wpos],
				poc_info->wpos, poc_info->wsize);
		if (unlikely(ret < 0)) {
			panel_err("failed to write poc-write-seq\n");
			return ret;
		}
		break;
	case POC_OP_READ:
		ret = poc_read_data(panel, &poc_info->rbuf[poc_info->rpos],
				poc_info->rpos, poc_info->rsize);
		if (unlikely(ret < 0)) {
			panel_err("failed to write poc-read-seq\n");
			return ret;
		}
		break;
	case POC_OP_CHECKSUM:
		ret = poc_get_poc_chksum(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to get poc checksum\n");
			return ret;
		}
		ret = poc_get_poc_ctrl(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to get poc ctrl\n");
			return ret;
		}
		break;
	case POC_OP_CHECKPOC:
		ret = poc_get_octa_poc(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to get_octa_poc\n");
			return ret;
		}
		break;
	case POC_OP_SECTOR_ERASE:
		ret = sscanf((char *)arg, "%*d %d %d", &addr, &len);
		if (unlikely(ret < 2)) {
			panel_err("failed to get poc erase params\n");
			return -EINVAL;
		}
		if (unlikely(addr < 0) || unlikely((addr % SZ_4K) > 0) || unlikely(len < 0)) {
			panel_err("invalid poc erase params\n");
			return -EINVAL;
		}
		partition_addr = get_poc_partition_addr(poc_dev, POC_IMG_PARTITION);
		if (partition_addr < 0 || (partition_addr % SZ_4K) > 0) {
			panel_err("invalid partition addr %d\n", partition_addr);
			return -EINVAL;
		}

		partition_size = get_poc_partition_size(poc_dev, POC_IMG_PARTITION);
		if (partition_size <= 0 || (partition_size < addr + len)) {
			panel_err("invalid partition size %d %d %d\n", partition_size, addr, len);
			return -EINVAL;
		}
		addr = partition_addr + addr;

		ret = poc_memory_initialize(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to initialize memory\n");
			return ret;
		}
#ifdef CONFIG_USDM_PANEL_DPUI
		poc_info->erase_trycount++;
#endif
		ret = poc_erase(panel, addr, len);
		if (unlikely(ret < 0)) {
			panel_err("failed to write poc-erase-seq\n");
#ifdef CONFIG_USDM_PANEL_DPUI
			poc_info->erase_failcount++;
#endif
			poc_info->erased = false;
		} else {
			poc_info->erased = true;
		}
		ret = poc_memory_uninitialize(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to uninitialize memory\n");
			return ret;
		}
		break;
	case POC_OP_IMG_READ:
		ret = read_poc_partition(poc_dev, POC_IMG_PARTITION);
		if (unlikely(ret < 0)) {
			panel_err("failed to read img partition\n");
			return ret;
		}
		ret = poc_data_backup(panel,
				poc_rd_img + poc_dev->partition[POC_IMG_PARTITION].addr,
				poc_dev->partition[POC_IMG_PARTITION].size, POC_IMG_PATH);
		if (unlikely(ret < 0)) {
			panel_err("failed to backup poc img\n");
			return ret;
		}
		break;
	case POC_OP_DIM_READ:
		if (arg == NULL)
			return -EINVAL;

		index = POC_DIM_PARTITION + *(int *)arg;
		if (index < POC_DIM_PARTITION || index > POC_DIM_PARTITION_END) {
			panel_err("invalid index of dim partition:%d\n", index);
			return -EINVAL;
		}

		ret = read_poc_partition(poc_dev, index);
		if (unlikely(ret < 0)) {
			panel_err("failed to read partition:%d\n", index);
			return ret;
		}
		break;
	case POC_OP_DIM_CHKSUM:
		break;
	case POC_OP_DIM_VALID:
		break;
	case POC_OP_MTP_READ:
		if (arg == NULL)
			return -EINVAL;

		index = POC_MTP_PARTITION + *(int *)arg;
		if (index < POC_MTP_PARTITION || index > POC_MTP_PARTITION_END) {
			panel_err("invalid index of mtp partition:%d\n", index);
			return -EINVAL;
		}

		ret = read_poc_partition(poc_dev, index);
		if (unlikely(ret < 0)) {
			panel_err("failed to read partition:%d\n", index);
			return ret;
		}
		break;
	case POC_OP_MCD_READ:
		ret = read_poc_partition(poc_dev, POC_MCD_PARTITION);
		if (unlikely(ret < 0)) {
			panel_err("failed to read MCD partition\n");
			return ret;
		}
		break;
	case POC_OP_DIM_READ_TEST:
		if (arg == NULL)
			return -EINVAL;

		index = POC_DIM_PARTITION + *(int *)arg;
		if (index < POC_DIM_PARTITION || index > POC_DIM_PARTITION_END) {
			panel_err("invalid index of mtp partition:%d\n", index);
			return -EINVAL;
		}

		ret = read_poc_partition(poc_dev, index);
		if (unlikely(ret < 0)) {
			panel_err("failed to read partition:%d\n", index);
			return ret;
		}

		ret = poc_data_backup(panel,
				poc_rd_img + poc_dev->partition[index].addr,
				poc_dev->partition[index].size, POC_DATA_PATH);
		if (unlikely(ret < 0)) {
			panel_err("failed to backup gamma flash\n");
			return ret;
		}
		break;
#ifdef CONFIG_USDM_POC_SPI
	case POC_OP_SET_CONN_SRC:
		ret = sscanf((char *)arg, "%*d %d", &addr);
		if (unlikely(ret < 1)) {
			panel_err("failed to get poc set conn params\n");
			return -EINVAL;
		}
		if (unlikely(addr < 0) || addr >= MAX_POC_CONN_SRC) {
			panel_err("invalid poc set conn params\n");
			return -EINVAL;
		}
		poc_info->conn_src = addr;
		break;
#if 0
	case POC_OP_READ_SPI_STATUS_REG:
		ret = _spi_poc_get_status(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to get status reg\n");
			return ret;
		}
		panel_info("spi_status_reg 0x%04X\n", ret);
		break;
#endif
#endif
	case POC_OP_INITIALIZE:
		ret = poc_memory_initialize(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to initialize memory\n");
			return ret;
		}
		break;
	case POC_OP_UNINITIALIZE:
		ret = poc_memory_uninitialize(panel);
		if (unlikely(ret < 0)) {
			panel_err("failed to uninitialize memory\n");
			return ret;
		}
		break;
	case POC_OP_GM2_READ:
		if (arg == NULL)
			return -EINVAL;
		index = *(int *)arg;
		if (index < POC_GM2_PARTITION || index > POC_GM2_PARTITION_END) {
			panel_err("invalid index of dim partition:%d\n", index);
			return -EINVAL;
		}
		ret = read_poc_partition(poc_dev, index);
		if (unlikely(ret < 0)) {
			panel_err("failed to read partition:%d\n", index);
			return ret;
		}
		break;
	case POC_OP_NONE:
		panel_info("none operation\n");
		break;
	default:
		panel_err("invalid poc op\n");
		break;
	}

	ktime_get_ts64(&cur_ts);
	delta_ts = timespec64_sub(cur_ts, last_ts);
	elapsed_msec = timespec64_to_ns(&delta_ts) / 1000000;
	panel_info("%s (elapsed %lld.%03lld sec) -\n", poc_op[cmd],
			elapsed_msec / 1000, elapsed_msec % 1000);

	return 0;
};
EXPORT_SYMBOL(set_panel_poc);

#ifdef CONFIG_USDM_PANEL_POC_FLASH
static long panel_poc_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct panel_poc_device *poc_dev = file->private_data;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_device *panel = to_panel_device(poc_dev);
	int ret;

	if (unlikely(!poc_dev->opened)) {
		panel_err("poc device not opened\n");
		return -EIO;
	}


	panel_wake_lock(panel);
	if (!IS_PANEL_ACTIVE(panel)) {
		panel_wake_unlock(panel);
		return -EAGAIN;
	}

	panel_mutex_lock(&panel->io_lock);
	switch (cmd) {
	case IOC_GET_POC_STATUS:
		if (copy_to_user((u32 __user *)arg, &poc_info->state,
					sizeof(poc_info->state))) {
			ret = -EFAULT;
			break;
		}
		break;
	case IOC_GET_POC_CHKSUM:
		ret = set_panel_poc(poc_dev, POC_OP_CHECKSUM, NULL);
		if (ret) {
			panel_err("error set_panel_poc\n");
			ret = -EFAULT;
			break;
		}
		if (copy_to_user((u8 __user *)arg, &poc_info->poc_chksum[4],
					sizeof(poc_info->poc_chksum[4]))) {
			ret = -EFAULT;
			break;
		}
		break;
	case IOC_GET_POC_CSDATA:
		ret = set_panel_poc(poc_dev, POC_OP_CHECKSUM, NULL);
		if (ret) {
			panel_err("error set_panel_poc\n");
			ret = -EFAULT;
			break;
		}
		if (copy_to_user((u8 __user *)arg, poc_info->poc_chksum,
					sizeof(poc_info->poc_chksum))) {
			ret = -EFAULT;
			break;
		}
		break;
	case IOC_GET_POC_ERASED:
		if (copy_to_user((u8 __user *)arg, &poc_info->erased,
					sizeof(poc_info->erased))) {
			ret = -EFAULT;
			break;
		}
		break;
	case IOC_GET_POC_FLASHED:
		ret = set_panel_poc(poc_dev, POC_OP_CHECKPOC, NULL);
		if (ret) {
			panel_err("error set_panel_poc\n");
			ret = -EFAULT;
			break;
		}
		if (copy_to_user((u8 __user *)arg, &poc_info->poc,
					sizeof(poc_info->poc))) {
			ret = -EFAULT;
			break;
		}
		break;
	case IOC_SET_POC_ERASE:
		ret = set_panel_poc(poc_dev, POC_OP_ERASE, NULL);
		if (ret) {
			panel_err("error set_panel_poc\n");
			ret = -EFAULT;
			break;
		}
		break;
	default:
		break;
	};
	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);

	return 0;
}

static int panel_poc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *dev = file->private_data;
	struct panel_poc_device *poc_dev = container_of(dev, struct panel_poc_device, dev);
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_device *panel = to_panel_device(poc_dev);
	int ret;

	panel_info("was called\n");

	if (poc_dev->opened) {
		panel_err("already opend\n");
		return -EBUSY;
	}

	panel_wake_lock(panel);
	if (!IS_PANEL_ACTIVE(panel)) {
		panel_wake_unlock(panel);
		return -EAGAIN;
	}

	panel_mutex_lock(&panel->io_lock);

	ret = set_panel_poc(poc_dev, POC_OP_INITIALIZE, NULL);
	if (ret < 0)
		goto err_open;

	panel_mutex_lock(&panel->op_lock);
	poc_info->state = 0;
	panel_mutex_unlock(&panel->op_lock);
	memset(poc_info->poc_chksum, 0, sizeof(poc_info->poc_chksum));
	memset(poc_info->poc_ctrl, 0, sizeof(poc_info->poc_ctrl));

	poc_info->wbuf = poc_wr_img;
	poc_info->wpos = 0;
	poc_info->wsize = 0;

	poc_info->rbuf = poc_rd_img;
	poc_info->rpos = 0;
	poc_info->rsize = 0;

	file->private_data = poc_dev;
	poc_dev->opened = 1;
	atomic_set(&poc_dev->cancel, 0);
	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);

	return 0;
err_open:
	panel_err("failed to initialize %d\n", ret);
	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);
	return ret;
}

static int panel_poc_release(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct panel_poc_device *poc_dev = file->private_data;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_device *panel = to_panel_device(poc_dev);

	panel_info("was called\n");

	panel_wake_lock(panel);

	panel_mutex_lock(&panel->io_lock);

	panel_mutex_lock(&panel->op_lock);
	poc_info->state = 0;
	panel_mutex_unlock(&panel->op_lock);
	memset(poc_info->poc_chksum, 0, sizeof(poc_info->poc_chksum));
	memset(poc_info->poc_ctrl, 0, sizeof(poc_info->poc_ctrl));

	poc_info->wbuf = NULL;
	poc_info->wpos = 0;
	poc_info->wsize = 0;

	poc_info->rbuf = NULL;
	poc_info->rpos = 0;
	poc_info->rsize = 0;

	poc_dev->opened = 0;
	atomic_set(&poc_dev->cancel, 0);

	ret = set_panel_poc(poc_dev, POC_OP_UNINITIALIZE, NULL);
	if (ret < 0)
		panel_err("failed to uninitialize %d\n", ret);

	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);
	return ret;
}

static ssize_t panel_poc_read(struct file *file, char __user *buf, size_t count,
		loff_t *ppos)
{
	struct panel_poc_device *poc_dev = file->private_data;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_device *panel = to_panel_device(poc_dev);
	ssize_t res;
	int partition_addr, partition_size;

	panel_info("size:%d ppos:%d\n", (int)count, (int)*ppos);
	poc_info->read_trycount++;

	if (unlikely(!poc_dev->opened)) {
		panel_err("poc device not opened\n");
		poc_info->read_failcount++;
		return -EIO;
	}

	if (unlikely(!buf)) {
		panel_err("invalid read buffer\n");
		poc_info->read_failcount++;
		return -EINVAL;
	}

	partition_addr = get_poc_partition_addr(poc_dev, POC_IMG_PARTITION);
	if (partition_addr < 0 || (partition_addr % SZ_4K) > 0) {
		panel_err("invalid partition addr %d\n", partition_addr);
		return -EINVAL;
	}

	partition_size = get_poc_partition_size(poc_dev, POC_IMG_PARTITION);
	if (partition_size < 0) {
		poc_info->read_failcount++;
		return -EINVAL;
	}

	if (unlikely(*ppos < 0 || *ppos >= partition_size)) {
		panel_err("invalid read pos %d\n", (int)*ppos);
		poc_info->read_failcount++;
		return -EINVAL;
	}

	panel_wake_lock(panel);

	if (!IS_PANEL_ACTIVE(panel)) {
		poc_info->read_failcount++;
		panel_wake_unlock(panel);
		return -EAGAIN;
	}

	panel_mutex_lock(&panel->io_lock);
	poc_info->rbuf = poc_rd_img;
	poc_info->rpos = partition_addr + *ppos;
	if (count > partition_size - *ppos) {
		panel_warn("adjust count %d -> %d\n",
				(int)count, (int)(partition_size - *ppos));
		count = partition_size - *ppos;
	}
	poc_info->rsize = (u32)count;

	res = set_panel_poc(poc_dev, POC_OP_READ, NULL);
	if (res < 0)
		goto err_read;

	res = simple_read_from_buffer(buf, poc_info->rsize,
			ppos, poc_info->rbuf + partition_addr, partition_size);
	if (res < 0)
		goto err_read;

	panel_info("read %ld bytes (count %ld)\n", res, count);
	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);
	return res;

err_read:
	panel_mutex_unlock(&panel->io_lock);
	poc_info->read_failcount++;
	panel_wake_unlock(panel);
	return res;
}

static ssize_t panel_poc_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct panel_poc_device *poc_dev = file->private_data;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct panel_device *panel = to_panel_device(poc_dev);
	ssize_t res;
	int partition_addr, partition_size;

	panel_info("size:%d, ppos:%d\n", (int)count, (int)*ppos);
	poc_info->write_trycount++;

	if (unlikely(!poc_dev->opened)) {
		panel_err("poc device not opened\n");
		poc_info->write_failcount++;
		return -EIO;
	}

	if (unlikely(!buf)) {
		panel_err("invalid write buffer\n");
		poc_info->write_failcount++;
		return -EINVAL;
	}

	partition_addr = get_poc_partition_addr(poc_dev, POC_IMG_PARTITION);
	if (partition_addr < 0 || (partition_addr % SZ_4K) > 0) {
		panel_err("invalid partition addr %d\n", partition_addr);
		return -EINVAL;
	}

	partition_size = get_poc_partition_size(poc_dev, POC_IMG_PARTITION);
	if (partition_size < 0) {
		poc_info->write_failcount++;
		return -EINVAL;
	}

	if (unlikely(*ppos < 0 || *ppos >= partition_size)) {
		panel_err("invalid write size pos %d, size %d\n",
				(int)*ppos, (int)count);
		poc_info->write_failcount++;
		return -EINVAL;
	}

	panel_wake_lock(panel);

	if (!IS_PANEL_ACTIVE(panel)) {
		poc_info->write_failcount++;
		panel_wake_unlock(panel);
		return -EAGAIN;
	}

	panel_mutex_lock(&panel->io_lock);
	poc_info->wbuf = poc_wr_img;
	poc_info->wpos = partition_addr + *ppos;
	if (count > partition_size - *ppos) {
		panel_warn("adjust count %d -> %d\n",
				(int)count, (int)(partition_size - *ppos));
		count = partition_size - *ppos;
	}
	poc_info->wsize = (u32)count;

	res = simple_write_to_buffer(poc_info->wbuf + partition_addr, partition_size,
			ppos, buf, poc_info->wsize);
	if (res < 0)
		goto err_write;

	panel_info("write %ld bytes (count %ld)\n", res, count);

	res = set_panel_poc(poc_dev, POC_OP_WRITE, NULL);
	if (res < 0)
		goto err_write;
	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);

	return count;

err_write:
	poc_info->write_failcount++;
	panel_mutex_unlock(&panel->io_lock);
	panel_wake_unlock(panel);
	return res;
}

loff_t panel_poc_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	panel_info("s_maxbytes %d, i_size_read %d\n",
			(int)inode->i_sb->s_maxbytes, (int)i_size_read(inode));

	return generic_file_llseek_size(file, offset, whence,
					inode->i_sb->s_maxbytes,
					i_size_read(inode));
}

static const struct file_operations panel_poc_fops = {
	.owner = THIS_MODULE,
	.read = panel_poc_read,
	.write = panel_poc_write,
	.unlocked_ioctl = panel_poc_ioctl,
	.open = panel_poc_open,
	.release = panel_poc_release,
	.llseek	= generic_file_llseek,
};
#endif /* CONFIG_USDM_PANEL_POC_FLASH */

#ifdef CONFIG_USDM_PANEL_DPUI
#ifdef CONFIG_USDM_PANEL_POC_FLASH
#define EPOCEFS_IMGIDX (100)
enum {
	EPOCEFS_NOENT = 1,		/* No such file or directory */
	EPOCEFS_EMPTY = 2,		/* Empty file */
	EPOCEFS_READ = 3,		/* Read failed */
	MAX_EPOCEFS,
};

static int poc_dpui_callback(struct panel_poc_device *poc_dev)
{
	struct panel_poc_info *poc_info;

	poc_info = &poc_dev->poc_info;

	inc_dpui_u32_field(DPUI_KEY_PNPOC_ER_TRY, poc_info->erase_trycount);
	poc_info->erase_trycount = 0;
	inc_dpui_u32_field(DPUI_KEY_PNPOC_ER_FAIL, poc_info->erase_failcount);
	poc_info->erase_failcount = 0;

	inc_dpui_u32_field(DPUI_KEY_PNPOC_WR_TRY, poc_info->write_trycount);
	poc_info->write_trycount = 0;
	inc_dpui_u32_field(DPUI_KEY_PNPOC_WR_FAIL, poc_info->write_failcount);
	poc_info->write_failcount = 0;

	inc_dpui_u32_field(DPUI_KEY_PNPOC_RD_TRY, poc_info->read_trycount);
	poc_info->read_trycount = 0;
	inc_dpui_u32_field(DPUI_KEY_PNPOC_RD_FAIL, poc_info->read_failcount);
	poc_info->read_failcount = 0;

	return 0;
}
#else
static int poc_dpui_callback(struct panel_poc_device *poc_dev) { return 0; }
#endif /* CONFIG_USDM_PANEL_POC_FLASH */

static int poc_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct panel_poc_device *poc_dev;
	struct dpui_info *dpui = data;

	if (dpui == NULL) {
		panel_err("dpui is null\n");
		return 0;
	}

	poc_dev = container_of(self, struct panel_poc_device, poc_notif);
	poc_dpui_callback(poc_dev);

	return 0;
}
#endif /* CONFIG_USDM_PANEL_DPUI */

int panel_poc_probe(struct panel_device *panel, struct panel_poc_data *poc_data)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct panel_poc_info *poc_info = &poc_dev->poc_info;
	struct pnobj *pnobj;
	int ret = 0, i, exists;
	static bool initialized;

	if (!poc_data) {
		panel_warn("poc_data is null\n");
		return -EINVAL;
	}

	if (!initialized) {
#ifdef CONFIG_USDM_PANEL_POC_FLASH
		poc_dev->dev.minor = MISC_DYNAMIC_MINOR;
		poc_dev->dev.name = "poc";
		poc_dev->dev.fops = &panel_poc_fops;
		poc_dev->dev.parent = NULL;

		ret = misc_register(&poc_dev->dev);
		if (ret) {
			panel_err("failed to register panel misc driver (ret %d)\n", ret);
			goto exit_probe;
		}
#endif
		INIT_LIST_HEAD(&poc_dev->seq_list);
		INIT_LIST_HEAD(&poc_dev->maptbl_list);
	}
	poc_info->version = poc_data->version;
#ifdef CONFIG_USDM_POC_SPI
	poc_info->conn_src = poc_data->conn_src;
#endif
	poc_info->wdata_len = poc_data->wdata_len;
	poc_dev->partition = poc_data->partition;
	poc_dev->nr_partition = poc_data->nr_partition;

	/* setup sequence list */
	for (i = 0; i < poc_data->nr_seqtbl; i++) {
		if (!is_valid_sequence(&poc_data->seqtbl[i]))
			continue;

		list_add_tail(get_pnobj_list(&poc_data->seqtbl[i].base),
				&poc_dev->seq_list);
	}

	/* setup maptbl list */
	for (i = 0; i < poc_data->nr_maptbl; i++)
		list_add_tail(get_pnobj_list(&poc_data->maptbl[i].base),
				&poc_dev->maptbl_list);

	/* initialize maptbl */
	list_for_each_entry(pnobj, &poc_dev->maptbl_list, list) {
		struct maptbl *m =
			pnobj_container_of(pnobj, struct maptbl);

		m->pdata = poc_dev;
		maptbl_init(m);
	}

	poc_info->erased = false;
	poc_info->poc = 1;	/* default enabled */
	poc_dev->opened = 0;

	for (i = 0; i < poc_dev->nr_partition; i++) {
		poc_dev->partition[i].preload_done = false;
		poc_dev->partition[i].chksum_ok = false;
		panel_info("%s addr:0x%x size:%d\n", poc_dev->partition[i].name,
				poc_dev->partition[i].addr, poc_dev->partition[i].size);
		if (poc_info->total_size <
				poc_dev->partition[i].addr + poc_dev->partition[i].size)
			poc_info->total_size =
				poc_dev->partition[i].addr + poc_dev->partition[i].size;
	}

	if (poc_wr_img)
		devm_kfree(panel->dev, poc_wr_img);
	poc_wr_img = (u8 *)devm_kzalloc(panel->dev,
			poc_info->total_size * sizeof(u8), GFP_KERNEL);

	if (poc_rd_img)
		devm_kfree(panel->dev, poc_rd_img);
	poc_rd_img = (u8 *)devm_kzalloc(panel->dev,
			poc_info->total_size * sizeof(u8), GFP_KERNEL);

#ifdef CONFIG_USDM_PANEL_DPUI
	poc_info->total_trycount = -1;
	poc_info->total_failcount = -1;

	if (!initialized) {
		poc_dev->poc_notif.notifier_call = poc_notifier_callback;
		ret = dpui_logging_register(&poc_dev->poc_notif, DPUI_TYPE_PANEL);
		if (ret) {
			panel_err("failed to register dpui notifier callback\n");
			goto exit_probe;
		}
	}
#endif

	for (i = 0; i < poc_dev->nr_partition; i++) {
		if (poc_dev->partition[i].need_preload) {
			exists = check_poc_partition_exists(poc_dev, i);
			if (exists < 0) {
				panel_err("failed to check partition(%d)\n", i);
				ret = exists;
				goto exit_probe;
			}

			if (!exists) {
				panel_warn("partition(%d) not exist\n", i);
				continue;
			}

			ret = read_poc_partition(poc_dev, i);
			if (unlikely(ret < 0)) {
				panel_err("failed to read partition(%d)\n", i);
				goto exit_probe;
			}
		}
	}
	initialized = true;

	panel_info("total_size:%d registered successfully\n",
			poc_info->total_size);

exit_probe:
	return ret;
}

int panel_poc_remove(struct panel_device *panel)
{
	struct panel_poc_device *poc_dev = &panel->poc_dev;
	struct pnobj *pos, *next;

	list_for_each_entry_safe(pos, next, &poc_dev->seq_list, list)
		list_del(&pos->list);

	list_for_each_entry_safe(pos, next, &poc_dev->maptbl_list, list)
		list_del(&pos->list);

	return 0;
}

void copy_poc_wr_addr_maptbl(struct maptbl *tbl, u8 *dst)
{
	struct panel_poc_device *poc_dev;
	struct panel_poc_info *poc_info;

	if (!tbl || !dst)
		return;

	poc_dev = (struct panel_poc_device *)tbl->pdata;
	if (unlikely(!poc_dev))
		return;

	poc_info = &poc_dev->poc_info;

	dst[0] = (poc_info->waddr & 0xFF0000) >> 16;
	dst[1] = (poc_info->waddr & 0x00FF00) >> 8;
	dst[2] = (poc_info->waddr & 0x0000FF);
}

void copy_poc_wr_data_maptbl(struct maptbl *tbl, u8 *dst)
{
	struct panel_poc_device *poc_dev;
	struct panel_poc_info *poc_info;

	if (!tbl || !dst)
		return;

	poc_dev = (struct panel_poc_device *)tbl->pdata;
	if (unlikely(!poc_dev))
		return;

	poc_info = &poc_dev->poc_info;
	memcpy(dst, poc_info->wdata, poc_info->wdata_len);
}

void copy_poc_rd_addr_maptbl(struct maptbl *tbl, u8 *dst)
{
	struct panel_poc_device *poc_dev;
	struct panel_poc_info *poc_info;

	if (!tbl || !dst)
		return;

	poc_dev = (struct panel_poc_device *)tbl->pdata;
	if (unlikely(!poc_dev))
		return;

	poc_info = &poc_dev->poc_info;

	dst[0] = (poc_info->raddr & 0xFF0000) >> 16;
	dst[1] = (poc_info->raddr & 0x00FF00) >> 8;
	dst[2] = (poc_info->raddr & 0x0000FF);
}

void copy_poc_er_addr_maptbl(struct maptbl *tbl, u8 *dst)
{
	struct panel_poc_device *poc_dev;
	struct panel_poc_info *poc_info;

	if (!tbl || !dst)
		return;

	poc_dev = (struct panel_poc_device *)tbl->pdata;
	if (unlikely(!poc_dev))
		return;

	poc_info = &poc_dev->poc_info;

	dst[0] = (poc_info->waddr & 0xFF0000) >> 16;
	dst[1] = (poc_info->waddr & 0x00FF00) >> 8;
	dst[2] = (poc_info->waddr & 0x0000FF);
}
