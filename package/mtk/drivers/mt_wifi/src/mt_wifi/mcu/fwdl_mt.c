/*
 * Copyright (c) [2020], MediaTek Inc. All rights reserved.
 *
 * This software/firmware and related documentation ("MediaTek Software") are
 * protected under relevant copyright laws.
 * The information contained herein is confidential and proprietary to
 * MediaTek Inc. and/or its licensors.
 * Except as otherwise provided in the applicable licensing terms with
 * MediaTek Inc. and/or its licensors, any reproduction, modification, use or
 * disclosure of MediaTek Software, and information contained herein, in whole
 * or in part, shall be strictly prohibited.
*/
/*
 ***************************************************************************
 ***************************************************************************

	Module Name:
	fwdl.c
*/

#ifdef COMPOS_WIN
#include "MtConfig.h"
#if defined(EVENT_TRACING)
#include "fwdl_mt.tmh"
#endif

struct MCU_CTRL;

#elif defined(COMPOS_TESTMODE_WIN)
#include "config.h"
#else
#include "rt_config.h"
#endif

#define UINT16_MAX	0xFFFF
#define UINT32_MAX	0XFFFFFFFF



static VOID img_get_8bit(UINT8 *dest, UINT8 **src, UINT32 cnt)
{
	UINT32 i;

	for (i = 0; i < cnt; i++) {
		dest[i] = *(*src + i);
	}

	*src += cnt;
}

static VOID img_get_16bit(UINT16 *dest, UINT8 **src, UINT32 cnt)
{
	UINT32 i;

	for (i = 0; i < cnt; i++) {
		dest[i] = (UINT16)((*(*src + (i * 4) + 1) <<  8) |
						   (*(*src + (i * 4))     <<  0));
	}

	*src += cnt * 2;
}

static VOID img_get_32bit(UINT32 *dest, UINT8 **src, UINT32 cnt)
{
	UINT32 i;

	for (i = 0; i < cnt; i++) {
		dest[i] = (UINT32)((*(*src + (i * 4) + 3) << 24) |
						   (*(*src + (i * 4) + 2) << 16) |
						   (*(*src + (i * 4) + 1) <<  8) |
						   (*(*src + (i * 4))     <<  0));
	}

	*src += cnt * 4;
}

static INT32 MtCmdFwScatters(RTMP_ADAPTER *ad, UINT8 *image, UINT32 image_len)
{
	UINT32 sent_len;
	UINT32 cur_len = 0, count = 0;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(ad->hdev_ctrl);
	int ret = 0;

	while (1) {
		UINT32 sent_len_max = MT_UPLOAD_FW_UNIT - cap->cmd_header_len;

		sent_len = (image_len - cur_len) >=  sent_len_max ?
				   sent_len_max : (image_len - cur_len);

		if (sent_len > 0) {
			ret = MtCmdFwScatter(ad, image + cur_len, sent_len, count);
			count++;

			if (ret)
				goto error;

			cur_len += sent_len;
		} else
			break;
	}

error:
	MTWF_DBG(NULL, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_DEBUG,
			 "%s:(ret = %d)\n", __func__, ret);
	return ret;
}

static NDIS_STATUS load_code(struct _RTMP_ADAPTER *pAd, UINT32 method, struct img_source *src)
{
	NDIS_STATUS ret = NDIS_STATUS_FAILURE;

	if ((ret != NDIS_STATUS_SUCCESS) && (method & BIT(BIN_METHOD))) {
		if (src->bin_name)
			os_load_code_from_bin(pAd, &src->img_ptr, src->bin_name, &src->img_len);

		if (src->img_ptr) {
			src->applied_method = BIN_METHOD;
			ret =  NDIS_STATUS_SUCCESS;
		} else {
			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "Can't alloc memory for firmware bin\n");
			ret = NDIS_STATUS_RESOURCES;
			return ret;
		}
	}

	if ((ret != NDIS_STATUS_SUCCESS) && (method & BIT(HEADER_METHOD))) {
		if (src->header_ptr && src->header_len) {
			src->img_ptr = src->header_ptr;
			src->img_len = src->header_len;
			src->applied_method = HEADER_METHOD;
			ret = NDIS_STATUS_SUCCESS;
		} else
			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR,
				"load code method: cap (bitmap) = 0x%x, applied = %d fail, not support header\n", method, src->applied_method);
	}

	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
		"load code method: cap (bitmap) = 0x%x, applied = %d\n", method, src->applied_method);

	return ret;
}

static NDIS_STATUS alloc_patch_target(struct _RTMP_ADAPTER *pAd, struct patch_dl_target *target, UINT32 count)
{
	target->num_of_region = count;
	return os_alloc_mem(pAd, (UCHAR **)&target->patch_region, count * sizeof(struct patch_dl_buf));
}

static VOID free_patch_target(struct _RTMP_ADAPTER *pAd, struct patch_dl_target *target)
{
	os_free_mem(target->patch_region);
	target->patch_region = NULL;
	target->num_of_region = 0;
}

static NDIS_STATUS alloc_fw_target(struct _RTMP_ADAPTER *pAd, struct fw_dl_target *target, UINT32 count)
{
	target->num_of_region = count;
	return os_alloc_mem(pAd, (UCHAR **)&target->fw_region, count * sizeof(struct fw_dl_buf));
}

static VOID free_fw_target(struct _RTMP_ADAPTER *pAd, struct fw_dl_target *target)
{
	os_free_mem(target->fw_region);
	target->fw_region = NULL;
	target->num_of_region = 0;
}

static VOID show_patch_info_cpu(struct patch_info *patch_info)
{
	FWDL_PRINT_CHAR(patch_info->built_date, 16, "\tBuilt date: ");
	FWDL_PRINT_CHAR(patch_info->platform, 4, "\tPlatform: ");
	FWDL_PRINT_HEX(patch_info->hw_sw_version, 4, "\tHW/SW version: ");
	FWDL_PRINT_HEX(patch_info->patch_version, 4, "\tPatch version: ");
}

VOID show_patch_info(struct _RTMP_ADAPTER *pAd)
{
	RTMP_CHIP_CAP *chip_cap = hc_get_chip_cap(pAd->hdev_ctrl);
	UINT32 need_load, i;
	struct patch_info *patch_info;

	need_load = chip_cap->need_load_patch;

	for (i = 0; i < MAX_CPU; i++) {
		if (need_load & BIT(i)) {
			patch_info = &pAd->MCUCtrl.fwdl_ctrl.patch_profile[i].patch_info;

			MTWF_PRINT("CPU %d patch info\n", i);

			show_patch_info_cpu(patch_info);
		}
	}
}

static VOID show_fw_info_cpu(struct fw_info *fw_info)
{
	FWDL_PRINT_HEX(&fw_info->chip_id, 1, "\tChip ID: ");
	FWDL_PRINT_HEX(&fw_info->eco_ver, 1, "\tEco version: ");
	FWDL_PRINT_HEX(&fw_info->num_of_region, 1, "\tRegion number: ");
	FWDL_PRINT_HEX(&fw_info->format_ver, 1, "\tFormat version: ");
	FWDL_PRINT_HEX(&fw_info->format_flag, 1, "\tFormat flag: ");
	FWDL_PRINT_CHAR(fw_info->ram_ver, 10, "\tRam version: ");
	FWDL_PRINT_CHAR(fw_info->ram_built_date, 15, "\tBuilt date: ");
	MTWF_PRINT("\tCommon crc: 0x%x\n", fw_info->crc);
}

VOID show_fw_info(struct _RTMP_ADAPTER *pAd)
{
	RTMP_CHIP_CAP *chip_cap = hc_get_chip_cap(pAd->hdev_ctrl);
	UINT32 need_load, i;
	struct fw_info *fw_info;

	need_load = chip_cap->need_load_fw;

	for (i = 0; i < MAX_CPU; i++) {
		if (need_load & BIT(i)) {
			fw_info = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[i].fw_info;

			MTWF_PRINT("CPU %d fw info\n", i);

			show_fw_info_cpu(fw_info);
		}
	}
}

static VOID show_release_info_cpu(UCHAR *ptr)
{
	UINT8 tag_id = 0;
	UINT8 padding_length = 0;
	UINT16 tag_length = 0;
	UINT16 total_length = 0;
	MTWF_PRINT("\tRelease info: ");

	/* bypass separator */
	ptr += 16;

	/* parsing header tag */
	img_get_16bit(&tag_length, &ptr, 1);
	img_get_8bit(&padding_length, &ptr, 1);
	img_get_8bit(&tag_id, &ptr, 1);

	MTWF_PRINT("header tag = %d, total length = %d\n", tag_id, tag_length);

	if (tag_id != 0 || tag_length == 0)
		return;
	else
		total_length = tag_length;

	/* parsing individual tag */
	while (total_length > 0 && total_length < UINT16_MAX) {
		img_get_16bit(&tag_length, &ptr, 1);
		img_get_8bit(&padding_length, &ptr, 1);
		img_get_8bit(&tag_id, &ptr, 1);

		MTWF_PRINT("\ttag %d, padding length = %d, tag length = %d\n", tag_id, padding_length, tag_length);
		if (tag_length > 0 && tag_length < UINT16_MAX) {
			FWDL_PRINT_CHAR(ptr, tag_length, "\tpayload: ");
		}
		ptr += (tag_length + padding_length);
		if (total_length < (4 + tag_length + padding_length))
			break;
		total_length -= (4 + tag_length + padding_length);
	}

	return;
}

static NDIS_STATUS parse_patch_v1(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct patch_dl_target *target)
{
	NDIS_STATUS ret;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);
	struct img_source *src;
	struct patch_info *patch_info;
	UINT8 *img_ptr;
	UINT32 num_of_region, i;

	if (cpu >= MAX_CPU) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "target cpu incorrect!!!\n");
		ret = NDIS_STATUS_INVALID_DATA;
		goto out;
	}

	src = &pAd->MCUCtrl.fwdl_ctrl.patch_profile[cpu].source;
	patch_info = &pAd->MCUCtrl.fwdl_ctrl.patch_profile[cpu].patch_info;

	ret = load_code(pAd, cap->load_patch_method, src);
	if (ret)
		goto out;

	/* parse patch info */
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing patch header\n");

	img_ptr = src->img_ptr;

	img_get_8bit(patch_info->built_date, &img_ptr, 16);
	img_get_8bit(patch_info->platform, &img_ptr, 4);
	img_get_8bit(patch_info->hw_sw_version, &img_ptr, 4);
	img_get_8bit(patch_info->patch_version, &img_ptr, 4);

	show_patch_info_cpu(patch_info);

	/* fix to one region for this rom patch format */
	num_of_region = 1;

	ret = alloc_patch_target(pAd, target, num_of_region);
	if (ret)
		goto out;

	for (i = 0; i < num_of_region; i++) {
		struct patch_dl_buf *region;

		region = &target->patch_region[i];

		region->img_dest_addr = cap->rom_patch_offset;
		region->img_size = src->img_len - PATCH_V1_INFO_SIZE;
		region->img_ptr = src->img_ptr + PATCH_V1_INFO_SIZE;

		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
			"\tTarget address: 0x%x, length: 0x%x\n", region->img_dest_addr, region->img_size);
	}

	return NDIS_STATUS_SUCCESS;

out:
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "parse patch fail\n");

	return ret;
}

static NDIS_STATUS parse_patch_v2(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct patch_dl_target *target)
{
	NDIS_STATUS ret;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);
	struct img_source *src;
	struct patch_info *patch_info;
	UINT8 *img_ptr;
	UINT32 num_of_region, i;
	struct patch_glo_desc *glo_desc;
	struct patch_sec_map *sec_map;

	if (cpu >= MAX_CPU) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "target cpu incorrect!!!\n");
		ret = NDIS_STATUS_INVALID_DATA;
		goto out;
	}

	src = &pAd->MCUCtrl.fwdl_ctrl.patch_profile[cpu].source;
	patch_info = &pAd->MCUCtrl.fwdl_ctrl.patch_profile[cpu].patch_info;

	ret = load_code(pAd, cap->load_patch_method, src);
	if (ret)
		goto out;

	/* parse patch info */
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing patch header\n");

	img_ptr = src->img_ptr;

	/* patch header */
	img_get_8bit(patch_info->built_date, &img_ptr, 16);
	img_get_8bit(patch_info->platform, &img_ptr, 4);
	img_get_8bit(patch_info->hw_sw_version, &img_ptr, 4);
	img_get_8bit(patch_info->patch_version, &img_ptr, 4);
	img_ptr += 4; /* bypass crc & reserved field */

	show_patch_info_cpu(patch_info);

	/* global descriptor */
	glo_desc = (struct patch_glo_desc *)img_ptr;
	num_of_region = be2cpu32(glo_desc->section_num);
	img_ptr += sizeof(struct patch_glo_desc);

	if (num_of_region == 0 || num_of_region >= UINT32_MAX) {
		MTWF_DBG(NULL, DBG_CAT_CFG, DBG_SUBCAT_ALL, DBG_LVL_ERROR,
			"num_of_region invalid\n");
		return NDIS_STATUS_FAILURE;
	}
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
				"\tSection num: 0x%x, subsys: 0x%x\n", num_of_region, be2cpu32(glo_desc->subsys));
	ret = alloc_patch_target(pAd, target, num_of_region);
	if (ret)
		goto out;
	/* section map */
	for (i = 0; i < num_of_region; i++) {
		struct patch_dl_buf *region;
		UINT32 section_type;

		region = &target->patch_region[i];
		sec_map = (struct patch_sec_map *)img_ptr;
		img_ptr += sizeof(struct patch_sec_map);

		section_type = be2cpu32(sec_map->section_type);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
				 "\tSection %d: type = 0x%x, offset = 0x%x, size = 0x%x\n", i, section_type,
				 be2cpu32(sec_map->section_offset), be2cpu32(sec_map->section_size));

		if ((section_type & PATCH_SEC_TYPE_MASK) == PATCH_SEC_TYPE_BIN_INFO) {
			region->img_dest_addr = be2cpu32(sec_map->bin_info_spec.dl_addr);
			region->img_size = be2cpu32(sec_map->bin_info_spec.dl_size);
			region->img_ptr = src->img_ptr + be2cpu32(sec_map->section_offset);

			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
				"\tTarget address: 0x%x, length: 0x%x\n", region->img_dest_addr, region->img_size);
		} else {
			region->img_ptr = NULL;
			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tNot binary\n");
		}
	}

	return NDIS_STATUS_SUCCESS;

out:
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "parse patch fail\n");

	return ret;
}

static NDIS_STATUS load_patch_v1(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct patch_dl_target *target)
{
	NDIS_STATUS ret;
	UINT32 num_of_region, i;
	struct fwdl_ctrl *fwdl_ctrl;

	ret = NDIS_STATUS_SUCCESS;
	fwdl_ctrl = &pAd->MCUCtrl.fwdl_ctrl;
	num_of_region = target->num_of_region;

	fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;

	/* 1. get semaphore */
	ret = MtCmdPatchSemGet(pAd, GET_PATCH_SEM);
	if (ret)
		goto out;

	switch (fwdl_ctrl->sem_status) {
	case PATCH_NOT_DL_SEM_FAIL:
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "patch is not ready && get semaphore fail\n");
		ret = NDIS_STATUS_FAILURE;
		goto out;
		break;
	case PATCH_IS_DL:
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "patch is ready, continue to ILM/DLM DL\n");
		ret = NDIS_STATUS_SUCCESS;
		goto out;
		break;
	case PATCH_NOT_DL_SEM_SUCCESS:
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "patch is not ready && get semaphore success\n");
		break;
	default:
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "get semaphore invalid status(%d)\n", fwdl_ctrl->sem_status);
		ret = NDIS_STATUS_FAILURE;
		goto out;
		break;
	}
	for (i = 0; i < num_of_region; i++) {
		struct patch_dl_buf *region;

		region = &target->patch_region[i];

		if (region->img_ptr == NULL)
			continue;
		/* 2. config PDA */
		fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;
		ret = MtCmdAddressLenReq(pAd, region->img_dest_addr, region->img_size, MODE_TARGET_ADDR_LEN_NEED_RSP);

		if (ret)
			goto out;

		/* 3. image scatter */
		fwdl_ctrl->stage = FWDL_STAGE_SCATTER;

		ret = MtCmdFwScatters(pAd, region->img_ptr, region->img_size);

		if (ret)
			goto out;
	}

	/* 4. patch start */
	fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;

	ret = MtCmdPatchFinishReq(pAd);
	if (ret)
		goto out;

	/* 5. release semaphore */
	ret = MtCmdPatchSemGet(pAd, REL_PATCH_SEM);
	if (ret)
		goto out;

	if (fwdl_ctrl->sem_status == SEM_RELEASE) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "release patch semaphore\n");
	} else {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "release patch semaphore invalid status(%d)\n", fwdl_ctrl->sem_status);
		ret = NDIS_STATUS_FAILURE;
	}

out:
	free_patch_target(pAd, target);

	if (ret) {
		fwdl_ctrl->stage = FWDL_STAGE_FW_NOT_DL;
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "load patch fail\n");
	}

	return ret;
}

static NDIS_STATUS parse_fw_v1(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct fw_dl_target *target)
{
	NDIS_STATUS ret;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);
	struct img_source *src;
	struct fw_info *fw_info;
	UINT8 *img_ptr;
	UINT32 num_of_region, i;

	if (cpu >= MAX_CPU) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "target cpu incorrect!!!\n");
		ret = NDIS_STATUS_INVALID_DATA;
		goto out;
	}

	src = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[cpu].source;
	fw_info = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[cpu].fw_info;

	ret = load_code(pAd, cap->load_fw_method, src);
	if (ret)
		goto out;

	/* parse fw info */
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing CPU %d fw tailer\n", cpu);

	img_ptr = src->img_ptr + src->img_len - 29;

	img_get_8bit(fw_info->ram_ver, &img_ptr, 10);
	img_get_8bit(fw_info->ram_built_date, &img_ptr, 15);

	show_fw_info_cpu(fw_info);

	/* fix to 1 because they're all von-neumann architecture */
	num_of_region = 1;

	ret = alloc_fw_target(pAd, target, num_of_region);
	if (ret)
		goto out;

	for (i = 0; i < num_of_region; i++) {
		UINT32 dl_len = 0;
		struct fw_dl_buf *region;

		img_ptr = src->img_ptr + src->img_len - 4;

		img_get_32bit(&dl_len, &img_ptr, 1);
		dl_len += 4; /* including 4 byte inverse-crc */

		region = &target->fw_region[i];
#define FW_CODE_START_ADDRESS1 0x100000
		region->img_dest_addr = FW_CODE_START_ADDRESS1; /* hard code because header don't have it */
		region->img_size = dl_len;
		region->img_ptr = src->img_ptr;
		region->feature_set = 0; /* no feature set field for this fw format */
		region->feature_set |= FW_FEATURE_OVERRIDE_RAM_ADDR; /* hard code this field to override ram starting address */
	}

	return NDIS_STATUS_SUCCESS;

out:
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "parse fw fail\n");

	return ret;
}

static NDIS_STATUS parse_fw_v2(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct fw_dl_target *target)
{
	NDIS_STATUS ret;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);
	struct img_source *src;
	struct fw_info *fw_info;
	UINT8 *img_ptr;
	UINT32 num_of_region, i, offset;

	if (cpu >= MAX_CPU) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "target cpu incorrect!!!\n");
		ret = NDIS_STATUS_INVALID_DATA;
		goto out;
	}

	src = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[cpu].source;
	fw_info = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[cpu].fw_info;
	offset = 0;

	ret = load_code(pAd, cap->load_fw_method, src);
	if (ret)
		goto out;

	/* parse fw info */
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing CPU %d fw tailer\n", cpu);

	img_ptr = src->img_ptr + src->img_len - FW_V2_INFO_SIZE;

	img_ptr += 4; /* bypass destination address field */
	img_get_8bit(&fw_info->chip_id, &img_ptr, 1);
	img_ptr += 1; /* bypass feature set field */
	img_get_8bit(&fw_info->eco_ver, &img_ptr, 1);
	img_get_8bit(fw_info->ram_ver, &img_ptr, 10);
	img_get_8bit(fw_info->ram_built_date, &img_ptr, 15);

	show_fw_info_cpu(fw_info);

	if (cpu == WA_CPU)
		num_of_region = 1; /* for 7615 CR4, which is von-neumann architecture */
	else
		num_of_region = 2; /* fix to 2 because they're all harvard architecture */

	ret = alloc_fw_target(pAd, target, num_of_region);
	if (ret)
		goto out;

	/* first region first parsing */
	for (i = 0; i < num_of_region; i++) {
		UINT32 dl_addr = 0;
		UINT32 dl_len = 0;
		UINT8 dl_feature_set;
		struct fw_dl_buf *region;

		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing tailer region %d\n", i);

		img_ptr = src->img_ptr + src->img_len - (num_of_region - i) * FW_V2_INFO_SIZE;

		img_get_32bit(&dl_addr, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tTarget address: 0x%x\n", dl_addr);

		img_ptr += 1; /* bypass chip id field */

		img_get_8bit(&dl_feature_set, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_CAT_ALL, DBG_LVL_INFO, "\tFeature set: 0x%02x\n", dl_feature_set);

		img_ptr += 26; /* jump to size field */

		img_get_32bit(&dl_len, &img_ptr, 1);
		dl_len += 4; /* including 4 byte inverse-crc */
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tDownload size: %d\n", dl_len);

		region = &target->fw_region[i];
		region->img_dest_addr = dl_addr;
		region->img_size = dl_len;
		region->img_ptr = src->img_ptr + offset;
		offset += dl_len;
		region->feature_set = dl_feature_set;
		if (IS_MT7615(pAd) && (cpu == WM_CPU) && (i == 0)) { /* for 7615 N9 ILM */
			region->feature_set |= FW_FEATURE_OVERRIDE_RAM_ADDR; /* hard code this field to override ram starting address */
		}
	}

	return NDIS_STATUS_SUCCESS;

out:
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "parse fw fail\n");

	return ret;
}

static NDIS_STATUS parse_fw_v3(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct fw_dl_target *target)
{
	NDIS_STATUS ret;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);
	struct img_source *src;
	UINT8 *img_ptr;
	struct fw_info *fw_info;
	UINT32 num_of_region, i, offset;

	if (cpu >= MAX_CPU) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "target cpu incorrect!!!\n");
		ret = NDIS_STATUS_INVALID_DATA;
		goto out;
	}

	src = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[cpu].source;
	fw_info = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[cpu].fw_info;
	offset = 0;

	ret = load_code(pAd, cap->load_fw_method, src);
	if (ret)
		goto out;

	/* parse fw info */
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing CPU %d fw tailer\n", cpu);

	img_ptr = src->img_ptr + src->img_len - FW_V3_COMMON_TAILER_SIZE;

	img_get_8bit(&fw_info->chip_id, &img_ptr, 1);
	img_get_8bit(&fw_info->eco_ver, &img_ptr, 1);
	img_get_8bit(&fw_info->num_of_region, &img_ptr, 1);
	img_get_8bit(&fw_info->format_ver, &img_ptr, 1);
	img_get_8bit(&fw_info->format_flag, &img_ptr, 1);
	img_ptr += 2; /* bypass reserved field */
	img_get_8bit(fw_info->ram_ver, &img_ptr, 10);
	img_get_8bit(fw_info->ram_built_date, &img_ptr, 15);
	img_get_32bit(&fw_info->crc, &img_ptr, 1);

	show_fw_info_cpu(fw_info);

	num_of_region = fw_info->num_of_region;

	ret = alloc_fw_target(pAd, target, num_of_region);
	if (ret)
		goto out;

	/* first region first parsing */
	for (i = 0; i < num_of_region; i++) {
		struct fw_dl_buf *region;
		UINT32 dl_addr = 0;
		UINT32 dl_len = 0;
		UINT32 decomp_crc = 0;
		UINT32 decomp_len = 0;
		UINT32 decomp_block_size = 0;
		UINT8 dl_feature_set;

		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Parsing tailer region %d\n", i);

		img_ptr = src->img_ptr + src->img_len - FW_V3_COMMON_TAILER_SIZE - (num_of_region - i) * FW_V3_REGION_TAILER_SIZE;

		img_get_32bit(&decomp_crc, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tDecomp crc: 0x%x\n", decomp_crc);

		img_get_32bit(&decomp_len, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tDecomp size: 0x%x\n", decomp_len);

		img_get_32bit(&decomp_block_size, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tDecomp block size: 0x%x\n", decomp_block_size);

		img_ptr += 4; /* bypass reserved field */

		img_get_32bit(&dl_addr, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tTarget address: 0x%x\n", dl_addr);

		img_get_32bit(&dl_len, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "\tDownload size: %d\n", dl_len);

		img_get_8bit(&dl_feature_set, &img_ptr, 1);
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_CAT_ALL, DBG_LVL_INFO, "\tFeature set: 0x%02x\n", dl_feature_set);

		region = &target->fw_region[i];
		region->img_dest_addr = dl_addr;
		region->img_size = dl_len;
		region->feature_set = dl_feature_set;
		region->decomp_crc = decomp_crc;
		region->decomp_img_size = decomp_len;
		region->decomp_block_size = decomp_block_size;
		region->img_ptr = src->img_ptr + offset;
		offset += dl_len;
	}

	if (fw_info->format_ver == FW_V3_FORMAT_VER_CONNAC_V1) {
		if (fw_info->format_flag & FW_V3_FORMAT_FLAG_RELEASE_INFO)
			show_release_info_cpu(src->img_ptr + offset);
	} else {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "unknown format version %d\n", fw_info->format_ver);
	}

	return NDIS_STATUS_SUCCESS;

out:
	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "parse fw fail\n");

	return ret;
}

#ifdef WIFI_RAM_EMI_SUPPORT
static NDIS_STATUS load_emi_fw(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct fw_dl_target *target)
{
	NDIS_STATUS ret = NDIS_STATUS_SUCCESS;
	UINT32 i, emi_addr_offset = 0;
	struct MCU_CTRL *mcu_ctrl = &pAd->MCUCtrl;
	struct fw_dl_buf *region = NULL;
	RTMP_CHIP_CAP *pChipCap = hc_get_chip_cap(pAd->hdev_ctrl);
	volatile UINT8 __iomem *vir_addr = NULL, *target_vir_addr = NULL;
	BOOLEAN bEMIPayloadFound = FALSE;

	if (pChipCap->emi_phy_addr == 0 || pChipCap->emi_phy_addr_size == 0) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR,
				"emi physical address or size is zero\n");
		return NDIS_STATUS_FAILURE;
	}

	MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
			"emi_phy_addr = 0x%08x, emi_phy_addr_size = 0x%08x\n",
			pChipCap->emi_phy_addr, pChipCap->emi_phy_addr_size);

	vir_addr = ioremap(pChipCap->emi_phy_addr, pChipCap->emi_phy_addr_size);
	if (vir_addr != 0) {
		/* parsing and find download emi payload */
		for (i = 0; i < target->num_of_region; i++) {
			region = &target->fw_region[i];

			if (region->feature_set & FW_FEATURE_DL_TO_EMI) {
				emi_addr_offset = (region->img_dest_addr - pChipCap->mcu_emi_addr_base);

				if ((emi_addr_offset + region->img_size) <= pChipCap->emi_phy_addr_size) {
					target_vir_addr = vir_addr + emi_addr_offset;
					memmove(target_vir_addr, region->img_ptr, region->img_size);
					MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO,
							"emi_addr_offset = 0x%08x, img_dest_addr = 0x%08x, img_size = 0x%08x\n",
							emi_addr_offset, region->img_dest_addr, region->img_size);
				} else {
					ret = NDIS_STATUS_FAILURE;
					MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "Over EMI reserved space size!\n");
					goto error;
				}
				bEMIPayloadFound = TRUE;
			}
		}

		if (bEMIPayloadFound)
			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "wifi ram emi download successfully!\n");
		else
			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "No wifi ram emi payload found!\n");
	} else {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "ioremap fail!\n");
		return NDIS_STATUS_FAILURE;
	}

error:
	if (vir_addr != 0)
		iounmap(vir_addr);

	return ret;
}
#endif /* WIFI_RAM_EMI_SUPPORT */

static NDIS_STATUS load_fw_v1(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct fw_dl_target *target)
{
	NDIS_STATUS ret;
	UINT32 i, override, override_addr;
	struct fwdl_ctrl *fwdl_ctrl;
	struct MCU_CTRL *mcu_ctrl;

	ret = NDIS_STATUS_SUCCESS;
	override = 0;
	override_addr = 0;
	fwdl_ctrl = &pAd->MCUCtrl.fwdl_ctrl;
	mcu_ctrl = &pAd->MCUCtrl;

	/* first parsing first download */
	for (i = 0; i < target->num_of_region; i++) {
		struct fw_dl_buf *region;

		region = &target->fw_region[i];
#ifdef WIFI_RAM_EMI_SUPPORT
		if (region->feature_set & FW_FEATURE_DL_TO_EMI)
			continue;
#endif /* WIFI_RAM_EMI_SUPPORT */

		if (region->feature_set & FW_FEATURE_OVERRIDE_RAM_ADDR) {
			override |= FW_START_OVERRIDE_START_ADDRESS;
			override_addr = region->img_dest_addr;
		}

		fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;

		/* 1. config PDA */
		ret = MtCmdAddressLenReq(pAd, region->img_dest_addr, region->img_size,
								((region->feature_set & FW_FEATURE_SET_ENCRY) ? MODE_ENABLE_ENCRY : 0) |
								(MODE_SET_KEY(GET_FW_FEATURE_SET_KEY(region->feature_set))) |
								((region->feature_set & FW_FEATURE_SET_ENCRY) ? MODE_RESET_SEC_IV : 0) |
								((region->feature_set & FW_FEATURE_ENCRY_MODE) ? MODE_ENCRY_MODE_SEL : 0) |
								((cpu == WA_CPU) ? MODE_WORKING_PDA_OPTION : 0) |
								MODE_TARGET_ADDR_LEN_NEED_RSP);
		if (ret)
			goto out;

		/* 2. image scatter */
		fwdl_ctrl->stage = FWDL_STAGE_SCATTER;

		ret = MtCmdFwScatters(pAd, region->img_ptr, region->img_size);
		if (ret)
			goto out;
	}

	/* 3. fw start negotiation */
	fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;

	ret = MtCmdFwStartReq(pAd, override | ((cpu == WA_CPU) ? FW_START_WORKING_PDA_OPTION : 0), override_addr);

out:
	free_fw_target(pAd, target);

	if (ret) {
		fwdl_ctrl->stage = FWDL_STAGE_FW_NOT_DL;
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "load fw fail\n");
	}

	return ret;
}

static NDIS_STATUS load_fw_v2_compressimg(struct _RTMP_ADAPTER *pAd, enum target_cpu cpu, struct fw_dl_target *target)
{
	NDIS_STATUS ret;
	UINT32 i, override, override_addr;
	UINT32 compress_region_num = 0;
	UINT32 block_idx = 0;
	UINT32 block_dest_addr;
	UINT32 remain_chunk_size = 0;
	UINT8 *img_ptr_pos = NULL;
	struct fwdl_ctrl *fwdl_ctrl;
	struct MCU_CTRL *mcu_ctrl;
	INIT_CMD_WIFI_START_WITH_DECOMPRESSION decompress_info;
	UINT8 do_compressed_dl = 0;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);

	ret = NDIS_STATUS_SUCCESS;
	override = 0;
	override_addr = 0;
	fwdl_ctrl = &pAd->MCUCtrl.fwdl_ctrl;
	mcu_ctrl = &pAd->MCUCtrl;

	/* first parsing first download */
	for (i = 0; i < target->num_of_region; i++) {
		struct fw_dl_buf *region;

		region = &target->fw_region[i];
#ifdef WIFI_RAM_EMI_SUPPORT
		if (region->feature_set & FW_FEATURE_DL_TO_EMI)
			continue;
#endif /* WIFI_RAM_EMI_SUPPORT */

		img_ptr_pos = region->img_ptr;
		remain_chunk_size = region->img_size;
		if (region->feature_set & FW_FEATURE_OVERRIDE_RAM_ADDR) {
			override |= FW_START_OVERRIDE_START_ADDRESS;
			override_addr = region->img_dest_addr;
		}
		if (region->feature_set & FW_FEATURE_COMPRESS_IMG) { /* Compressed image Process */
			do_compressed_dl = 1;
			block_idx = 0;
			compress_region_num += 1;
			MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "REGION[%d] COMPRESSED IMAGE DOWNLOAD\n", i);
			while (remain_chunk_size > 0) {
				UINT32 payload_size_per_chunk = 0;

				fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;
				img_get_32bit(&payload_size_per_chunk, &img_ptr_pos, 1);
				if (remain_chunk_size < 4)
					break;
				remain_chunk_size -= 4;
				/* 1. config PDA */
				block_dest_addr = region->img_dest_addr + block_idx * region->decomp_block_size;
				ret = MtCmdAddressLenReq(pAd, block_dest_addr, payload_size_per_chunk,
					((region->feature_set & FW_FEATURE_SET_ENCRY) ? MODE_ENABLE_ENCRY : 0) |
					(MODE_SET_KEY(GET_FW_FEATURE_SET_KEY(region->feature_set))) |
					((region->feature_set & FW_FEATURE_SET_ENCRY) ? MODE_RESET_SEC_IV : 0) |
					((cpu == WA_CPU) ? MODE_WORKING_PDA_OPTION : 0) |
					MODE_TARGET_ADDR_LEN_NEED_RSP);
				if (ret)
					goto out;
				/* 2. image scatter */
				fwdl_ctrl->stage = FWDL_STAGE_SCATTER;
				ret = MtCmdFwScatters(pAd, img_ptr_pos, payload_size_per_chunk);
				if (ret)
					goto out;
				if (remain_chunk_size < payload_size_per_chunk)
					break;
				remain_chunk_size -= payload_size_per_chunk;
				img_ptr_pos += payload_size_per_chunk;
				block_idx++;
			}
			decompress_info.aucDecompRegion[i].u4RegionAddress = region->img_dest_addr;
			decompress_info.aucDecompRegion[i].u4Regionlength = region->decomp_img_size;
			decompress_info.aucDecompRegion[i].u4RegionCRC = region->decomp_crc;
			decompress_info.u4BlockSize = region->decomp_block_size;
		} else { /* Uncompressed Image Process*/
			fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;
			/* 1. config PDA */
			ret = MtCmdAddressLenReq(pAd, region->img_dest_addr, region->img_size,
						((region->feature_set & FW_FEATURE_SET_ENCRY) ? MODE_ENABLE_ENCRY : 0) |
						(MODE_SET_KEY(GET_FW_FEATURE_SET_KEY(region->feature_set))) |
						((region->feature_set & FW_FEATURE_SET_ENCRY) ? MODE_RESET_SEC_IV : 0) |
						((cpu == WA_CPU) ? MODE_WORKING_PDA_OPTION : 0) |
						MODE_TARGET_ADDR_LEN_NEED_RSP);
			if (ret)
				goto out;
			/* 2. image scatter */
			fwdl_ctrl->stage = FWDL_STAGE_SCATTER;

			ret = MtCmdFwScatters(pAd, region->img_ptr, region->img_size);
			if (ret)
				goto out;
		}
	} /* num_of_region */
	fwdl_ctrl->stage = FWDL_STAGE_CMD_EVENT;
	if (do_compressed_dl) {
		/*if the override[5] is not set, the ROM CODE will use default WIFI ILM as start address */
		decompress_info.u4Address = override_addr;
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_INFO, "Start CMD Jump Address 0x%x\n", decompress_info.u4Address);
		decompress_info.u4DecompressTmpAddress = cap->decompress_tmp_addr;
		override |= ((cpu == WA_CPU) ? FW_START_WORKING_PDA_OPTION : 0);
		override |= FW_CHANGE_DECOMPRESSION_TMP_ADDRESS;
		decompress_info.u4Override = override;
		decompress_info.u4RegionNumber = compress_region_num;
		/* 3. fw start negotiation */
		ret = MtCmdFwDecompressStart(pAd, &decompress_info);
	} else {
		/* 3. fw start negotiation */
		ret = MtCmdFwStartReq(pAd, override | ((cpu == WA_CPU) ? FW_START_WORKING_PDA_OPTION : 0), override_addr);
	}
out:
	free_fw_target(pAd, target);

	if (ret) {
		fwdl_ctrl->stage = FWDL_STAGE_FW_NOT_DL;
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "load fw fail\n");
	}

	return ret;
}

static VOID free_patch_buf(RTMP_ADAPTER *pAd)
{
	UINT32 i, loaded;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);

	loaded = cap->need_load_patch;

	for (i = 0; i < MAX_CPU; i++) {
		if (loaded & BIT(i)) {
			struct img_source *src;

			src = &pAd->MCUCtrl.fwdl_ctrl.patch_profile[i].source;
			if ((src->applied_method == BIN_METHOD) && (src->img_ptr)) {
#ifdef MT7981
				vfree(src->img_ptr);
#else
				os_free_mem(src->img_ptr);
#endif
				src->img_ptr = NULL;
			}
		}
	}
}

static VOID free_fw_buf(RTMP_ADAPTER *pAd)
{
	UINT32 i, loaded;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);

	loaded = cap->need_load_fw;

	for (i = 0; i < MAX_CPU; i++) {
		if (loaded & BIT(i)) {
			struct img_source *src;

			src = &pAd->MCUCtrl.fwdl_ctrl.fw_profile[i].source;
			if ((src->applied_method == BIN_METHOD) && (src->img_ptr)) {
#ifdef MT7981
				vfree(src->img_ptr);
#else
				os_free_mem(src->img_ptr);
#endif
				src->img_ptr = NULL;
			}
		}
	}
}


/*
 * setup fw wifi task to be in the right state for a specific fwdl stage
 */
static NDIS_STATUS ctrl_fw_state_v2(struct _RTMP_ADAPTER *pAd, enum fwdl_stage target_stage)
{
	NDIS_STATUS ret;
	UINT32 fw_sync, loop;
	RTMP_CHIP_CAP *cap = hc_get_chip_cap(pAd->hdev_ctrl);

	ret = NDIS_STATUS_SUCCESS;
	fw_sync = AsicGetFwSyncValue(pAd);

	switch (target_stage) {
	case FWDL_STAGE_FW_NOT_DL:
		/* could be in both state because of wifi default on/off */
		if (fw_sync == WIFI_TASK_STATE_INITIAL || fw_sync == WIFI_TASK_STATE_FW_DOWNLOAD) {
			ret = NDIS_STATUS_SUCCESS;
			break;
		}

		/* fw restart cmd*/
		ret = MtCmdRestartDLReq(pAd);
		if (ret)
			break;

		/* polling */
		loop = 0;
		ret = NDIS_STATUS_FAILURE;
		do {
			fw_sync = AsicGetFwSyncValue(pAd);

			if (fw_sync == WIFI_TASK_STATE_INITIAL || fw_sync == WIFI_TASK_STATE_FW_DOWNLOAD) {
				ret = NDIS_STATUS_SUCCESS;
				break;
			}

			RtmpOsMsDelay(1);
			loop++;
		} while (loop <= WAIT_LOOP);

		break;
	case FWDL_STAGE_CMD_EVENT:
	case FWDL_STAGE_SCATTER:
		if (fw_sync == WIFI_TASK_STATE_FW_DOWNLOAD) {
			ret = NDIS_STATUS_SUCCESS;
			break;
		} else if (fw_sync == WIFI_TASK_STATE_INITIAL) {
			/* power on wifi sys */
			ret = MtCmdPowerOnWiFiSys(pAd);
			if (ret)
				break;
		} else {
			/* in RAM code, use restart cmd */
			ret = MtCmdRestartDLReq(pAd);
			if (ret)
				break;

			/* power on wifi sys */
			ret = MtCmdPowerOnWiFiSys(pAd);
			if (ret)
				break;
		}

		/* polling */
		loop = 0;
		ret = NDIS_STATUS_FAILURE;
		do {
			fw_sync = AsicGetFwSyncValue(pAd);

			if (fw_sync == WIFI_TASK_STATE_FW_DOWNLOAD) {
				ret = NDIS_STATUS_SUCCESS;
				break;
			}

			RtmpOsMsDelay(1);
			loop++;
		} while (loop <= WAIT_LOOP);

		break;
	case FWDL_STAGE_FW_RUNNING:
		{
			UINT32 target_fw_sync;

			target_fw_sync = (cap->need_load_fw & BIT(WA_CPU)) ? WIFI_TASK_STATE_WACPU_RDY : WIFI_TASK_STATE_NORMAL_TRX;

			if (fw_sync == target_fw_sync) {
				ret = NDIS_STATUS_SUCCESS;
				break;
			}

			/* polling */
			loop = 0;
			ret = NDIS_STATUS_FAILURE;
			do {
				fw_sync = AsicGetFwSyncValue(pAd);

				if (fw_sync == target_fw_sync) {
					ret = NDIS_STATUS_SUCCESS;
					break;
				}

				RtmpOsMsDelay(1);
				loop++;
			} while (loop <= WAIT_LOOP);
		}

		break;
	default:
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "invalid target stage\n");
		ret = NDIS_STATUS_FAILURE;
		break;
	}

	if (ret)
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR,
			"fail, target stage = %d, current sync CR = %d \n", target_stage, fw_sync);

	return ret;
}

NDIS_STATUS mt_fwdl_hook_init(struct _RTMP_ADAPTER *pAd)
{
	NDIS_STATUS ret;
	struct fwdl_op *op = &pAd->MCUCtrl.fwdl_ctrl.fwdl_op;
	RTMP_CHIP_CAP *pChipCap = hc_get_chip_cap(pAd->hdev_ctrl);

	ret = NDIS_STATUS_SUCCESS;

	if (pChipCap->need_load_patch) {
		if (pChipCap->load_patch_flow == PATCH_FLOW_V1)
			op->load_patch = load_patch_v1;
		else
			ret = NDIS_STATUS_FAILURE;

		if (pChipCap->patch_format == PATCH_FORMAT_V1)
			op->parse_patch = parse_patch_v1;
		else if (pChipCap->patch_format == PATCH_FORMAT_V2)
			op->parse_patch = parse_patch_v2;
		else
			ret = NDIS_STATUS_FAILURE;
	}

#ifdef WIFI_RAM_EMI_SUPPORT
	if (pChipCap->need_load_emi_fw)
		op->load_emi_fw = load_emi_fw;
#endif /* WIFI_RAM_EMI_SUPPORT */

	if (pChipCap->need_load_fw) {
		if (pChipCap->load_fw_flow == FW_FLOW_V1)
			op->load_fw = load_fw_v1;
		else if (pChipCap->load_fw_flow == FW_FLOW_V2_COMPRESS_IMG)
			op->load_fw = load_fw_v2_compressimg; /* compress img must use v3 format*/
		else
			ret = NDIS_STATUS_FAILURE;

		if (pChipCap->fw_format == FW_FORMAT_V1)
			op->parse_fw = parse_fw_v1;
		else if (pChipCap->fw_format == FW_FORMAT_V2)
			op->parse_fw = parse_fw_v2;
		else if (pChipCap->fw_format == FW_FORMAT_V3)
			op->parse_fw = parse_fw_v3;
		else
			ret = NDIS_STATUS_FAILURE;
	}

		op->ctrl_fw_state = ctrl_fw_state_v2;

	if (ret)
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "FWDL hook fail\n");

	return ret;
}

NDIS_STATUS mt_load_patch(struct _RTMP_ADAPTER *pAd)
{
	RTMP_CHIP_CAP *chip_cap = hc_get_chip_cap(pAd->hdev_ctrl);
	RTMP_CHIP_OP *chip_op = hc_get_chip_ops(pAd->hdev_ctrl);
	struct fwdl_op *fwdl_op = &pAd->MCUCtrl.fwdl_ctrl.fwdl_op;
	struct patch_dl_target target;
	NDIS_STATUS ret;
	UINT32 i, need_load;

	ret = NDIS_STATUS_SUCCESS;

	if (chip_op->fwdl_datapath_setup)
		chip_op->fwdl_datapath_setup(pAd, TRUE);

	if (fwdl_op->ctrl_fw_state) {
		ret = fwdl_op->ctrl_fw_state(pAd, FWDL_STAGE_CMD_EVENT);
		if (ret)
			goto done;
	}

	need_load = chip_cap->need_load_patch;
	if (need_load && (!fwdl_op->parse_patch || !fwdl_op->load_patch)) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "no hook function available\n");
		ret = NDIS_STATUS_FAILURE;
		goto done;
	}

	for (i = 0; i < MAX_CPU; i++) {
		if (need_load & BIT(i)) {
			ret = fwdl_op->parse_patch(pAd, i, &target);
			if (ret)
				goto done;
			ret = fwdl_op->load_patch(pAd, i, &target);
			if (ret)
				goto done;
		}
	}

done:

	if (chip_op->fwdl_datapath_setup)
		chip_op->fwdl_datapath_setup(pAd, FALSE);

	free_patch_buf(pAd);

	if (ret) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "patch download fail\n");
		show_trinfo_proc(pAd, "");
	}

	return ret;
}

NDIS_STATUS mt_load_fw(struct _RTMP_ADAPTER *pAd)
{
	RTMP_CHIP_CAP *chip_cap = hc_get_chip_cap(pAd->hdev_ctrl);
	RTMP_CHIP_OP *chip_op = hc_get_chip_ops(pAd->hdev_ctrl);
	struct fwdl_op *fwdl_op = &pAd->MCUCtrl.fwdl_ctrl.fwdl_op;
	struct fw_dl_target target;
	NDIS_STATUS ret;
	UINT32 i, need_load;

	ret = NDIS_STATUS_SUCCESS;

	if (chip_op->fwdl_datapath_setup)
		chip_op->fwdl_datapath_setup(pAd, TRUE);

	if (fwdl_op->ctrl_fw_state) {
		ret = fwdl_op->ctrl_fw_state(pAd, FWDL_STAGE_CMD_EVENT);
		if (ret)
			goto done;
	}

	need_load = chip_cap->need_load_fw;
	if (need_load && (!fwdl_op->parse_fw || !fwdl_op->load_fw)) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "no hook function available\n");
		ret = NDIS_STATUS_FAILURE;
		goto done;
	}

	for (i = 0; i < MAX_CPU; i++) {
		if (need_load & BIT(i)) {
			ret = fwdl_op->parse_fw(pAd, i, &target);
			if (ret)
				goto done;

#ifdef WIFI_RAM_EMI_SUPPORT
			if (chip_cap->need_load_emi_fw &&
				(chip_cap->need_load_emi_fw & BIT(i))) {
				if (fwdl_op->load_emi_fw) {
					ret = fwdl_op->load_emi_fw(pAd, i, &target);
					if (ret)
						goto done;
				}
			}
#endif /* WIFI_RAM_EMI_SUPPORT */

			ret = fwdl_op->load_fw(pAd, i, &target);
			if (ret)
				goto done;
		}
	}

	if (fwdl_op->ctrl_fw_state) {
		ret = fwdl_op->ctrl_fw_state(pAd, FWDL_STAGE_FW_RUNNING);
	}

done:

	if (chip_op->fwdl_datapath_setup)
		chip_op->fwdl_datapath_setup(pAd, FALSE);

	free_fw_buf(pAd);

	if (ret) {
		MTWF_DBG(pAd, DBG_CAT_FW, DBG_SUBCAT_ALL, DBG_LVL_ERROR, "fw download fail\n");
		pAd->MCUCtrl.fwdl_ctrl.stage = FWDL_STAGE_FW_NOT_DL;
		show_trinfo_proc(pAd, "");
	} else {
		pAd->MCUCtrl.fwdl_ctrl.stage = FWDL_STAGE_FW_RUNNING;
	}

	return ret;
}

NDIS_STATUS mt_restart_fw(struct _RTMP_ADAPTER *pAd)
{
	NDIS_STATUS ret;
	struct fwdl_op *fwdl_op = &pAd->MCUCtrl.fwdl_ctrl.fwdl_op;

	ret = NDIS_STATUS_SUCCESS;

#ifdef WF_RESET_SUPPORT
	if (pAd->wf_reset_in_progress == TRUE) {
		MTWF_DBG(pAd, DBG_CAT_MISC, DBG_SUBCAT_ALL, DBG_LVL_INFO, "wf reset skip\n");
		return ret;
	}
#endif

	if (fwdl_op->ctrl_fw_state)
		ret = fwdl_op->ctrl_fw_state(pAd, FWDL_STAGE_FW_NOT_DL);

	return ret;
}

