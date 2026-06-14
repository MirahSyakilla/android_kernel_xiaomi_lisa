// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2019-2021 The Linux Foundation. All rights reserved.
//               2022      The LineageOS Project
//

#define pr_fmt(fmt) "BATTERY_CHG: %s: " fmt, __func__
#include <crypto/sha.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/power_supply.h>
#include <linux/qti_power_supply.h>
#include <linux/soc/qcom/altmode-glink.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/battery_charger.h>
#include <asm/unaligned.h>
#include <linux/usb/ucsi_glink.h>

#include "qti_battery_charger.h"

extern int battery_chg_write(struct battery_chg_dev *bcdev, void *data,
			     int len);
extern int write_property_id(struct battery_chg_dev *bcdev,
			     struct psy_state *pst, u32 prop_id, u32 val);
extern int read_property_id(struct battery_chg_dev *bcdev,
			    struct psy_state *pst, u32 prop_id);
extern int usb_psy_get_prop(struct power_supply *psy,
			    enum power_supply_property prop,
			    union power_supply_propval *pval);

extern const char *const power_supply_usb_type_text[];

#define XIAOMI_UVDM_PAN_WORDS		2
#define XIAOMI_PAN_SVID			0xff00
#define XM_USBPD_STATE_SNK_STARTUP	25
#define XM_USBPD_STATE_SNK_READY	31
#define XM_USBPD_STATE_SRC_READY	5
#define XM_PD_PPS_TARGET_VOLTAGE_UV	11000000
#define XM_PD_PPS_TARGET_CURRENT_UA	2750000
#define XM_PD_COMPAT_PDO2_9V3A		0x0002d12c
#define XM_PD_COMPAT_ADAPTER_ID		ADAPTER_XIAOMI_PD_30W
#define XM_PD_RENEGOTIATION_DELAY_MS	2000
#define XM_PD_RENEGOTIATION_CONNECTOR	1
#define XM_UVDM_AUTH_PAYLOAD_LEN	16
#define XM_UVDM_AUTH_MSG_LEN		20
#define XM_UVDM_AUTH_ROWS		10

static const u8 xm_uvdm_auth_key_table[XM_UVDM_AUTH_ROWS][32] = {
	{
		0x5c, 0x4d, 0x41, 0x79, 0xda, 0x15, 0x01, 0xed,
		0x11, 0x74, 0x74, 0x92, 0xe2, 0x60, 0x83, 0xd9,
		0x6b, 0x8f, 0xbc, 0x73, 0xc8, 0x2e, 0x7b, 0xdf,
		0x5d, 0x32, 0x55, 0xc2, 0xa0, 0x36, 0xa1, 0xdc,
	},
	{
		0x45, 0x54, 0xea, 0x90, 0x7f, 0xc3, 0x25, 0x0d,
		0x20, 0x78, 0xbd, 0x62, 0x77, 0xd6, 0xcf, 0x0c,
		0xb8, 0x23, 0xe6, 0x22, 0x58, 0xd0, 0x87, 0x37,
		0x8a, 0xf2, 0x9c, 0x46, 0x6b, 0x10, 0x10, 0x26,
	},
	{
		0x83, 0xd3, 0x06, 0x25, 0xcb, 0x0f, 0x7c, 0xe6,
		0xd4, 0x96, 0x49, 0xff, 0x4c, 0xfc, 0xf9, 0xce,
		0xbe, 0x62, 0x46, 0x9b, 0x57, 0xe3, 0x1b, 0xa6,
		0x2d, 0x01, 0xde, 0x28, 0x10, 0xff, 0x62, 0x1a,
	},
	{
		0xa1, 0x44, 0x73, 0x05, 0x16, 0x6b, 0x4a, 0x78,
		0x6d, 0xf5, 0xdd, 0xd7, 0x07, 0xa8, 0x36, 0x0a,
		0x73, 0x28, 0xce, 0xd7, 0x03, 0x84, 0x87, 0x60,
		0x7f, 0xa2, 0x11, 0x28, 0xa1, 0x5d, 0x18, 0x64,
	},
	{
		0xe4, 0xfb, 0x00, 0xaf, 0xd1, 0x51, 0x86, 0xaf,
		0x1a, 0x44, 0x50, 0x20, 0x05, 0xd3, 0xd1, 0xb0,
		0xb1, 0xe9, 0xd1, 0xa6, 0x36, 0xa5, 0x1c, 0x26,
		0x34, 0x94, 0x22, 0xe7, 0xdb, 0xad, 0xc2, 0x73,
	},
	{
		0x46, 0xb8, 0xca, 0xe9, 0xa4, 0x6a, 0x32, 0x54,
		0xee, 0x55, 0x12, 0x82, 0xcf, 0x07, 0xb7, 0xbb,
		0x6b, 0x2e, 0xf1, 0x2b, 0xf6, 0x15, 0x46, 0x3c,
		0x23, 0x9b, 0xb6, 0x3b, 0x98, 0x4e, 0x42, 0xa9,
	},
	{
		0xea, 0xf4, 0x5b, 0x6f, 0xf3, 0xcc, 0xce, 0x76,
		0x9e, 0x82, 0xbd, 0x0b, 0x00, 0x7b, 0x69, 0xc4,
		0x06, 0xa6, 0x6b, 0x98, 0xf4, 0x27, 0xa2, 0xbb,
		0xe4, 0x55, 0xd1, 0xff, 0xcb, 0x1e, 0xcd, 0xcc,
	},
	{
		0xe7, 0x87, 0xdb, 0x56, 0x8b, 0x7d, 0x70, 0x4d,
		0x29, 0xcc, 0x50, 0xa2, 0xbb, 0x1e, 0x97, 0x1f,
		0x6f, 0x1f, 0xd7, 0xfe, 0x96, 0x04, 0xde, 0xb4,
		0xd4, 0xa1, 0xa7, 0xa7, 0x2c, 0x63, 0xff, 0x42,
	},
	{
		0xb5, 0xf7, 0x04, 0xa7, 0x5d, 0x0d, 0xb5, 0x4a,
		0x0b, 0x5e, 0x73, 0x17, 0x79, 0x94, 0x9b, 0x32,
		0x11, 0x05, 0xad, 0xc9, 0xd5, 0xc0, 0x85, 0x8c,
		0x50, 0xef, 0xc9, 0xbf, 0x92, 0x89, 0x2c, 0xf4,
	},
	{
		0xdf, 0x47, 0xcc, 0x32, 0x49, 0x83, 0x3b, 0xde,
		0xfe, 0xb4, 0xa0, 0x4a, 0x25, 0x74, 0x89, 0xfc,
		0x75, 0x00, 0xd8, 0x27, 0xb2, 0xb6, 0x6b, 0x26,
		0x89, 0xbd, 0xed, 0x7f, 0xfe, 0x49, 0x46, 0x5e,
	},
};

static const u8 xm_uvdm_auth_challenge_table[XM_UVDM_AUTH_ROWS]
					      [XM_UVDM_AUTH_PAYLOAD_LEN] = {
	{
		0xd6, 0xe9, 0x77, 0x05, 0x06, 0xc7, 0xf5, 0xf4,
		0xdf, 0x7a, 0x40, 0x00, 0x8c, 0x74, 0x67, 0xb8,
	},
	{
		0x09, 0x60, 0x4e, 0xb9, 0xad, 0x37, 0xe1, 0x7b,
		0xd6, 0xb2, 0xa4, 0xf2, 0xa3, 0x51, 0x59, 0x84,
	},
	{
		0xf6, 0x29, 0x5c, 0xe3, 0xbd, 0xe8, 0x67, 0x8c,
		0x9f, 0xfe, 0xab, 0x61, 0x60, 0x98, 0xab, 0xa0,
	},
	{
		0xae, 0x0c, 0x8e, 0xe9, 0xb0, 0x5b, 0xa2, 0x33,
		0x79, 0x9b, 0xad, 0xcb, 0x9f, 0xa2, 0x2b, 0x41,
	},
	{
		0x56, 0xb7, 0x47, 0x0c, 0x9d, 0x6f, 0x22, 0xca,
		0x00, 0x3e, 0x40, 0x32, 0x6f, 0xa7, 0x7e, 0xb5,
	},
	{
		0x60, 0x19, 0xb4, 0x57, 0xcd, 0x77, 0xfd, 0x5f,
		0xed, 0xff, 0xdc, 0x3d, 0x0e, 0xd0, 0x61, 0x06,
	},
	{
		0x09, 0xe4, 0x7c, 0x32, 0x76, 0x0f, 0xb2, 0xfa,
		0xcf, 0xc5, 0x84, 0x3d, 0xf0, 0x71, 0x64, 0x65,
	},
	{
		0xa6, 0x01, 0x93, 0x4c, 0x14, 0x03, 0xbf, 0xd9,
		0x55, 0xeb, 0xd7, 0x7f, 0x50, 0x9b, 0xa1, 0x7f,
	},
	{
		0x3e, 0x02, 0xb4, 0x4e, 0x19, 0xdc, 0x24, 0xf4,
		0x38, 0xe6, 0x2f, 0x02, 0x6a, 0xde, 0x4a, 0xa7,
	},
	{
		0x34, 0x3d, 0x6b, 0x41, 0xb9, 0x00, 0xbe, 0xb4,
		0xc8, 0xbf, 0x0e, 0x8f, 0xa7, 0xdf, 0x8c, 0x13,
	},
};

static const char *const power_supply_usbc_text[] = {
	"Nothing attached",
	"Source attached (default current)",
	"Source attached (medium current)",
	"Source attached (high current)",
	"Non compliant",
	"Sink attached",
	"Powered cable w/ sink",
	"Debug Accessory",
	"Audio Adapter",
	"Powered cable w/o sink",
};

#define BSWAP_32(x)                                                            \
	(u32)((((u32)(x)&0xff000000) >> 24) | (((u32)(x)&0x00ff0000) >> 8) |   \
	      (((u32)(x)&0x0000ff00) << 8) | (((u32)(x)&0x000000ff) << 24))

static bool usbpd_is_pd_active(struct battery_chg_dev *bcdev);

static void xm_pd_schedule_renegotiation(struct battery_chg_dev *bcdev,
					 const char *reason,
					 unsigned int delay_ms);

static u32 xm_pd_effective_adapter_id(struct battery_chg_dev *bcdev)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	u32 adapter_id = ADAPTER_NONE;

	if (bcdev->xm_adapter_id_override)
		return bcdev->xm_adapter_id_override;

	if (!read_property_id(bcdev, pst, XM_PROP_ADAPTER_ID))
		adapter_id = pst->prop[XM_PROP_ADAPTER_ID];

	if (bcdev->xm_pd_auth_compat && adapter_id == ADAPTER_NONE)
		adapter_id = XM_PD_COMPAT_ADAPTER_ID;

	return adapter_id;
}

static void xm_uvdm_auth_reset(struct battery_chg_dev *bcdev)
{
	bcdev->xm_uvdm_auth_row_valid = false;
	bcdev->xm_uvdm_auth_payload_valid = false;
	bcdev->xm_uvdm_auth_row = 0;
	bcdev->xm_uvdm_auth_adapter_id = ADAPTER_NONE;
	memset(bcdev->xm_uvdm_auth_payload, 0,
	       sizeof(bcdev->xm_uvdm_auth_payload));
	memset(bcdev->xm_uvdm_auth_response, 0,
	       sizeof(bcdev->xm_uvdm_auth_response));
}

static void xm_uvdm_hmac_sha256(const u8 *key, size_t key_len,
				const u8 *data, size_t data_len,
				u8 *out)
{
	struct sha256_state sctx;
	u8 key_block[SHA256_BLOCK_SIZE] = { 0 };
	u8 ipad[SHA256_BLOCK_SIZE];
	u8 opad[SHA256_BLOCK_SIZE];
	u8 inner[SHA256_DIGEST_SIZE];
	size_t i;

	if (key_len > SHA256_BLOCK_SIZE) {
		sha256(key, key_len, key_block);
	} else if (key_len) {
		memcpy(key_block, key, key_len);
	}

	for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
		ipad[i] = key_block[i] ^ 0x36;
		opad[i] = key_block[i] ^ 0x5c;
	}

	sha256_init(&sctx);
	sha256_update(&sctx, ipad, sizeof(ipad));
	sha256_update(&sctx, data, data_len);
	sha256_final(&sctx, inner);

	sha256_init(&sctx);
	sha256_update(&sctx, opad, sizeof(opad));
	sha256_update(&sctx, inner, sizeof(inner));
	sha256_final(&sctx, out);
}

static bool xm_uvdm_capture_challenge(struct battery_chg_dev *bcdev,
				      const u8 *data, size_t len)
{
	int i;

	if (!data || len != XM_UVDM_AUTH_PAYLOAD_LEN)
		return false;

	for (i = 0; i < XM_UVDM_AUTH_ROWS; i++) {
		if (memcmp(data, xm_uvdm_auth_challenge_table[i], len))
			continue;

		bcdev->xm_uvdm_auth_row = i;
		bcdev->xm_uvdm_auth_row_valid = true;
		pr_info("captured Xiaomi UVDM challenge row=%d\n", i + 1);
		return true;
	}

	return false;
}

static bool xm_uvdm_capture_auth_payload(struct battery_chg_dev *bcdev,
					 const u8 *data, size_t len)
{
	u8 auth_msg[XM_UVDM_AUTH_MSG_LEN] = { 0 };
	u8 auth_digest[SHA256_DIGEST_SIZE];
	u32 adapter_id;

	if (!data || len != XM_UVDM_AUTH_PAYLOAD_LEN ||
	    !bcdev->xm_uvdm_auth_row_valid)
		return false;

	adapter_id = xm_pd_effective_adapter_id(bcdev);

	memcpy(bcdev->xm_uvdm_auth_payload, data, len);
	bcdev->xm_uvdm_auth_adapter_id = adapter_id;
	bcdev->xm_uvdm_auth_payload_valid = true;

	memcpy(auth_msg, data, len);
	put_unaligned_be32(adapter_id, &auth_msg[XM_UVDM_AUTH_PAYLOAD_LEN]);

	xm_uvdm_hmac_sha256(xm_uvdm_auth_key_table[bcdev->xm_uvdm_auth_row],
			    sizeof(xm_uvdm_auth_key_table[0]),
			    auth_msg, sizeof(auth_msg), auth_digest);
	memcpy(bcdev->xm_uvdm_auth_response, auth_digest,
	       sizeof(bcdev->xm_uvdm_auth_response));

	pr_info("captured Xiaomi UVDM auth payload row=%d adapter=%#x first_bytes=%*phN\n",
		bcdev->xm_uvdm_auth_row + 1, adapter_id, 4, data);

	return true;
}

static void xm_uvdm_capture_cmd_payload(struct battery_chg_dev *bcdev,
					enum uvdm_state cmd,
					const u8 *data, size_t len)
{
	switch (cmd) {
	case USBPD_UVDM_SESSION_SEED:
		if (!xm_uvdm_capture_challenge(bcdev, data, len))
			xm_uvdm_auth_reset(bcdev);
		break;
	case USBPD_UVDM_AUTHENTICATION:
		if (!xm_uvdm_capture_auth_payload(bcdev, data, len))
			pr_info("skip Xiaomi UVDM auth payload capture row_valid=%u len=%zu\n",
				bcdev->xm_uvdm_auth_row_valid, len);
		break;
	case USBPD_UVDM_VERIFIED:
		if (!data || !len || !data[0])
			xm_uvdm_auth_reset(bcdev);
		break;
	default:
		break;
	}
}

int StringToHex(char *str, unsigned char *out, unsigned int *outlen)
{
	unsigned int cnt, len = strlen(str);
	int high, low;

	if (len % 2)
		return -EINVAL;

	for (cnt = 0; cnt < len / 2; cnt++) {
		high = hex_to_bin(str[2 * cnt]);
		low = hex_to_bin(str[2 * cnt + 1]);
		if (high < 0 || low < 0)
			return -EINVAL;

		out[cnt] = high << 4 | low;
	}

	if (outlen)
		*outlen = len / 2;

	return len / 2;
}

static int write_ss_auth_prop_id(struct battery_chg_dev *bcdev,
				 struct psy_state *pst, u32 prop_id, u32 *buff)
{
	struct xm_ss_auth_resp_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_set;
	memcpy(req_msg.data, buff, BATTERY_SS_AUTH_DATA_LEN * sizeof(u32));

	pr_debug(
		"psy: prop_id:%d size:%d data[0]:0x%x data[1]:0x%x data[2]:0x%x data[3]:0x%x\n",
		req_msg.property_id, sizeof(req_msg), req_msg.data[0],
		req_msg.data[1], req_msg.data[2], req_msg.data[3]);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int read_ss_auth_property_id(struct battery_chg_dev *bcdev,
				    struct psy_state *pst, u32 prop_id)
{
	struct xm_ss_auth_resp_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_get;

	pr_debug("psy: %s prop_id: %u\n", pst->psy->desc->name,
		 req_msg.property_id);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int write_verify_digest_prop_id(struct battery_chg_dev *bcdev,
				       struct psy_state *pst, u32 prop_id,
				       u8 *buff)
{
	struct xm_verify_digest_resp_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_set;
	req_msg.slave_fg = bcdev->slave_fg_verify_flag;
	memcpy(req_msg.digest, buff, BATTERY_DIGEST_LEN);

	pr_debug("psy: prop_id:%d size:%d\n", req_msg.property_id,
		 sizeof(req_msg));

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static void xm_pd_apply_power_profile(struct battery_chg_dev *bcdev,
				      const char *reason)
{
	struct power_supply *usb_psy = bcdev->psy_list[PSY_TYPE_USB].psy;
	struct psy_state *usb_pst = &bcdev->psy_list[PSY_TYPE_USB];
	struct psy_state *xm_pst = &bcdev->psy_list[PSY_TYPE_XM];
	union power_supply_propval val = { 0 };
	u32 target_voltage_uv = XM_PD_PPS_TARGET_VOLTAGE_UV;
	u32 target_current_ua = XM_PD_PPS_TARGET_CURRENT_UA;
	int rc, current_rc, voltage_rc;

	if (!usb_psy || !usbpd_is_pd_active(bcdev))
		return;

	if (bcdev->xm_pd_power_profile_applied &&
	    bcdev->usb_current_max_ua == target_current_ua &&
	    bcdev->usb_voltage_max_uv == target_voltage_uv)
		return;

	val.intval = target_current_ua;
	current_rc = power_supply_set_property(usb_psy,
					       POWER_SUPPLY_PROP_CURRENT_MAX,
					       &val);
	if (current_rc < 0)
		pr_err("failed to set PD current_max from %s rc=%d\n",
		       reason, current_rc);
	else
		pr_info("requested PD current_max=%d from %s\n",
			target_current_ua, reason);

	val.intval = target_voltage_uv;
	voltage_rc = power_supply_set_property(usb_psy,
					       POWER_SUPPLY_PROP_VOLTAGE_MAX,
					       &val);
	if (voltage_rc < 0)
		pr_err("failed to set PD voltage_max from %s rc=%d\n",
		       reason, voltage_rc);
	else
		pr_info("requested PD voltage_max=%d from %s\n",
			target_voltage_uv, reason);

	rc = current_rc ? current_rc : voltage_rc;
	bcdev->xm_pd_power_profile_applied = !rc;

	rc = read_property_id(bcdev, usb_pst, USB_VOLT_MAX);
	if (rc < 0)
		pr_err("failed to read back USB voltage_max after %s rc=%d\n",
		       reason, rc);

	rc = read_property_id(bcdev, usb_pst, USB_CURR_MAX);
	if (rc < 0)
		pr_err("failed to read back USB current_max after %s rc=%d\n",
		       reason, rc);

	rc = read_property_id(bcdev, usb_pst, USB_INPUT_CURR_LIMIT);
	if (rc < 0)
		pr_err("failed to read back USB input_current_limit after %s rc=%d\n",
		       reason, rc);

	rc = read_property_id(bcdev, xm_pst, XM_PROP_PDO2);
	if (rc < 0)
		pr_err("failed to read back PDO2 after %s rc=%d\n", reason, rc);

	rc = read_property_id(bcdev, xm_pst, XM_PROP_APDO_MAX);
	if (rc < 0)
		pr_err("failed to read back APDO max after %s rc=%d\n",
		       reason, rc);

	pr_info("PD profile readback after %s: usb_vmax=%u usb_cmax=%u usb_icl=%u pdo2=%#08x apdo_max=%u\n",
		reason,
		usb_pst->prop[USB_VOLT_MAX],
		usb_pst->prop[USB_CURR_MAX],
		usb_pst->prop[USB_INPUT_CURR_LIMIT],
		xm_pst->prop[XM_PROP_PDO2],
		xm_pst->prop[XM_PROP_APDO_MAX]);
}

static void xm_pd_renegotiation_workfunc(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(work,
			struct battery_chg_dev, xm_pd_renegotiation_work.work);
	struct psy_state *xm_pst = &bcdev->psy_list[PSY_TYPE_XM];
	bool active;
	int rc;

	rc = read_property_id(bcdev, xm_pst, XM_PROP_INPUT_SUSPEND);
	if (rc < 0) {
		pr_err("failed to read input_suspend before PD renegotiation rc=%d\n",
		       rc);
		bcdev->xm_pd_renegotiation_fail_count++;
		return;
	}

	if (xm_pst->prop[XM_PROP_INPUT_SUSPEND]) {
		bcdev->xm_pd_renegotiation_defer_count++;
		pr_info("deferring PD renegotiation while input is suspended=%u\n",
			xm_pst->prop[XM_PROP_INPUT_SUSPEND]);
		return;
	}

	active = usbpd_is_pd_active(bcdev);
	if (!active || !bcdev->xm_uvdm_compat_verified) {
		pr_info("skip PD renegotiation active=%u compat_verified=%u\n",
			active, bcdev->xm_uvdm_compat_verified);
		return;
	}

	bcdev->xm_pd_renegotiation_count++;
	bcdev->xm_pd_power_profile_applied = false;
	pr_info("starting PD renegotiation #%u\n",
		bcdev->xm_pd_renegotiation_count);

	rc = ucsi_glink_connector_reset(XM_PD_RENEGOTIATION_CONNECTOR, false);
	if (rc < 0) {
		bcdev->xm_pd_renegotiation_fail_count++;
		pr_err("PD renegotiation connector reset failed rc=%d\n", rc);
		return;
	}

	msleep(5000);
	rc = read_property_id(bcdev, xm_pst, XM_PROP_INPUT_SUSPEND);
	if (!rc && xm_pst->prop[XM_PROP_INPUT_SUSPEND]) {
		pr_info("skip PD profile reapply after reset, input suspended=%u\n",
			xm_pst->prop[XM_PROP_INPUT_SUSPEND]);
		return;
	}

	xm_pd_apply_power_profile(bcdev, "pd_renegotiation_work");
}

static void xm_pd_schedule_renegotiation(struct battery_chg_dev *bcdev,
					 const char *reason,
					 unsigned int delay_ms)
{
	if (!bcdev->initialized)
		return;

	if (!bcdev->xm_uvdm_compat_verified)
		return;

	pr_info("schedule PD renegotiation from %s delay=%u ms\n",
		reason, delay_ms);
	mod_delayed_work(system_long_wq, &bcdev->xm_pd_renegotiation_work,
			 msecs_to_jiffies(delay_ms));
}

static int read_verify_digest_property_id(struct battery_chg_dev *bcdev,
					  struct psy_state *pst, u32 prop_id)
{
	struct xm_verify_digest_resp_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_get;
	req_msg.slave_fg = bcdev->slave_fg_verify_flag;

	pr_debug("psy: %s prop_id: %u\n", pst->psy->desc->name,
		 req_msg.property_id);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static bool usbpd_is_pd_active(struct battery_chg_dev *bcdev)
{
	struct psy_state *usb_pst = &bcdev->psy_list[PSY_TYPE_USB];
	struct psy_state *xm_pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, usb_pst, USB_REAL_TYPE);
	if (rc < 0)
		return false;

	switch (usb_pst->prop[USB_REAL_TYPE]) {
	case POWER_SUPPLY_USB_TYPE_PD:
	case POWER_SUPPLY_USB_TYPE_PD_DRP:
	case POWER_SUPPLY_USB_TYPE_PD_PPS:
		break;
	default:
		return false;
	}

	rc = read_property_id(bcdev, xm_pst, XM_PROP_CURRENT_STATE);
	if (rc < 0)
		return false;

	switch (xm_pst->prop[XM_PROP_CURRENT_STATE]) {
	case QTI_POWER_SUPPLY_PD_ACTIVE:
	case QTI_POWER_SUPPLY_PD_PPS_ACTIVE:
	case XM_USBPD_STATE_SNK_READY:
		return true;
	default:
		return false;
	}
}

static bool xm_pd_effective_verified(struct battery_chg_dev *bcdev,
				     u32 fw_verified)
{
	if (fw_verified)
		return true;

	return bcdev->xm_uvdm_compat_verified && usbpd_is_pd_active(bcdev);
}

static void xm_pd_auth_compat_reset(struct battery_chg_dev *bcdev,
				    bool active, const char *reason)
{
	bcdev->xm_pd_auth_compat = active;
	bcdev->xm_uvdm_state = active ? USBPD_UVDM_CHARGER_VERSION :
					USBPD_UVDM_DISCONNECT;
	bcdev->xm_uvdm_last_cmd = USBPD_UVDM_DISCONNECT;
	bcdev->xm_uvdm_ack_pending = false;
	bcdev->xm_uvdm_compat_verified = false;
	bcdev->xm_pd_power_profile_applied = false;
	bcdev->xm_pd_auth_forced = false;
	bcdev->xm_uvdm_reset_count++;
	xm_uvdm_auth_reset(bcdev);
	cancel_delayed_work(&bcdev->xm_pd_renegotiation_work);

	pr_info("xiaomi pd auth compat reset reason=%s active=%u state=%u\n",
		reason, active, bcdev->xm_uvdm_state);
}

static void xm_pd_auth_compat_update(struct battery_chg_dev *bcdev)
{
	bool active = usbpd_is_pd_active(bcdev);

	if (active) {
		if (!bcdev->xm_pd_auth_compat) {
			xm_pd_auth_compat_reset(bcdev, true, "pd_active");
			pr_info("xiaomi pd auth compat enabled\n");
		}
		return;
	}

	if (bcdev->xm_pd_auth_compat)
		pr_info("xiaomi pd auth compat disabled\n");

	if (bcdev->xm_pd_auth_compat ||
	    bcdev->xm_uvdm_state != USBPD_UVDM_DISCONNECT ||
	    bcdev->xm_uvdm_last_cmd != USBPD_UVDM_DISCONNECT ||
	    bcdev->xm_uvdm_ack_pending ||
	    bcdev->xm_uvdm_compat_verified ||
	    bcdev->xm_pd_power_profile_applied ||
	    bcdev->xm_pd_auth_forced)
		xm_pd_auth_compat_reset(bcdev, false, "pd_inactive");
}

static void xm_pd_auth_compat_advance(struct battery_chg_dev *bcdev,
				      enum uvdm_state cmd, const u32 *data)
{
	if (!bcdev->xm_pd_auth_compat)
		return;

	if (cmd == USBPD_UVDM_VERIFIED) {
		bool verified = data && data[0];

		bcdev->xm_uvdm_state = cmd;
		bcdev->xm_uvdm_last_cmd = cmd;
		bcdev->xm_uvdm_ack_pending = true;
		bcdev->xm_uvdm_compat_verified = verified;

		pr_info("xiaomi pd auth verified cmd payload=%#x result=%u\n",
			data ? data[0] : 0, verified);
		if (verified) {
			xm_pd_apply_power_profile(bcdev, "uvdm_verified");
			xm_pd_schedule_renegotiation(bcdev, "uvdm_verified",
						     XM_PD_RENEGOTIATION_DELAY_MS);
		} else {
			xm_uvdm_auth_reset(bcdev);
			xm_pd_auth_compat_reset(bcdev, true,
						"uvdm_verify_failed");
		}
		return;
	}

	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		bcdev->xm_uvdm_state = USBPD_UVDM_CHARGER_VOLTAGE;
		break;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		bcdev->xm_uvdm_state = USBPD_UVDM_CHARGER_TEMP;
		break;
	case USBPD_UVDM_CHARGER_TEMP:
		bcdev->xm_uvdm_state = USBPD_UVDM_SESSION_SEED;
		break;
	case USBPD_UVDM_SESSION_SEED:
		bcdev->xm_uvdm_state = bcdev->xm_uvdm_auth_row_valid ?
			USBPD_UVDM_AUTHENTICATION :
			USBPD_UVDM_SESSION_SEED;
		break;
	case USBPD_UVDM_AUTHENTICATION:
		bcdev->xm_uvdm_state = bcdev->xm_uvdm_auth_payload_valid ?
			USBPD_UVDM_VERIFIED :
			USBPD_UVDM_AUTHENTICATION;
		break;
	case USBPD_UVDM_VERIFIED:
		bcdev->xm_uvdm_state = USBPD_UVDM_VERIFIED;
		break;
	case USBPD_UVDM_REMOVE_COMPENSATION:
	case USBPD_UVDM_REVERSE_AUTHEN:
	case USBPD_UVDM_CONNECT:
	case USBPD_UVDM_DISCONNECT:
	default:
		bcdev->xm_uvdm_state = cmd;
		break;
	}

	bcdev->xm_uvdm_last_cmd = cmd;
	bcdev->xm_uvdm_ack_pending = true;
}

static int xm_altmode_callback(void *priv, void *data, size_t len)
{
	struct battery_chg_dev *bcdev = priv;
	size_t copy_len = min_t(size_t, len, sizeof(bcdev->xm_uvdm_last_rx));
	struct altmode_pan_ack_msg ack = { 0 };
	int rc;

	memset(bcdev->xm_uvdm_last_rx, 0, sizeof(bcdev->xm_uvdm_last_rx));
	memcpy(bcdev->xm_uvdm_last_rx, data, copy_len);
	bcdev->xm_uvdm_rx_count++;
	bcdev->xm_uvdm_real_rx_seen = true;

	pr_debug("xiaomi altmode rx len=%zu payload=%*ph\n",
		 len, (int)copy_len, bcdev->xm_uvdm_last_rx);

	if (len && bcdev->xm_altmode_client) {
		ack.cmd_type = ALTMODE_PAN_ACK;
		ack.port_index = bcdev->xm_uvdm_last_rx[0];
		rc = altmode_send_data(bcdev->xm_altmode_client, &ack,
				       sizeof(ack));
		if (rc < 0) {
			bcdev->xm_uvdm_rx_ack_fail_count++;
			pr_err("xiaomi altmode ack port=%u failed rc=%d\n",
			       ack.port_index, rc);
		} else {
			bcdev->xm_uvdm_rx_ack_count++;
		}
	}

	return 0;
}

static int xm_altmode_register(struct battery_chg_dev *bcdev)
{
	const struct altmode_client_data client_data = {
		.svid = XIAOMI_PAN_SVID,
		.name = "xiaomi_pd_auth",
		.priv = bcdev,
		.callback = xm_altmode_callback,
	};
	struct altmode_client *client;

	if (bcdev->xm_altmode_registered)
		return 0;

	client = altmode_register_client(bcdev->dev, &client_data);
	if (IS_ERR(client))
		return PTR_ERR(client);

	bcdev->xm_altmode_client = client;
	bcdev->xm_altmode_registered = true;
	bcdev->xm_uvdm_state = USBPD_UVDM_DISCONNECT;
	pr_info("registered xiaomi altmode client svid=%#x\n", XIAOMI_PAN_SVID);

	return 0;
}

static void xm_altmode_probe_done(void *priv)
{
	struct battery_chg_dev *bcdev = priv;
	int rc;

	rc = xm_altmode_register(bcdev);
	if (rc < 0)
		pr_err("xiaomi altmode deferred registration failed rc=%d\n", rc);
}

static int xm_altmode_send_uvdm(struct battery_chg_dev *bcdev,
				enum uvdm_state cmd, const u32 *data)
{
	u32 msg[XIAOMI_UVDM_PAN_WORDS] = { 0 };
	int rc;

	if (data)
		memcpy(bcdev->xm_uvdm_last_tx, data,
		       sizeof(bcdev->xm_uvdm_last_tx));
	else
		memset(bcdev->xm_uvdm_last_tx, 0,
		       sizeof(bcdev->xm_uvdm_last_tx));

	if (!bcdev->xm_altmode_registered || !bcdev->xm_altmode_client) {
		bcdev->xm_uvdm_tx_skip_count++;
		pr_debug("xiaomi altmode tx skipped cmd=%u val=%#x registered=%u\n",
			 cmd, data ? data[0] : 0, bcdev->xm_altmode_registered);
		return -ENODEV;
	}

	msg[0] = cmd;
	if (data)
		msg[1] = data[0];

	rc = altmode_send_data(bcdev->xm_altmode_client, msg, sizeof(msg));
	if (rc < 0) {
		bcdev->xm_uvdm_tx_fail_count++;
		pr_err("xiaomi altmode tx cmd=%u val=%#x failed rc=%d\n",
		       cmd, msg[1], rc);
		return rc;
	}

	bcdev->xm_uvdm_tx_count++;
	pr_debug("xiaomi altmode tx cmd=%u val=%#x\n", cmd, msg[1]);

	return 0;
}

int qti_battery_charger_xiaomi_init(struct battery_chg_dev *bcdev)
{
	int rc;

	INIT_DELAYED_WORK(&bcdev->xm_pd_renegotiation_work,
			  xm_pd_renegotiation_workfunc);

	rc = xm_altmode_register(bcdev);
	if (rc == -EPROBE_DEFER) {
		rc = altmode_register_notifier(bcdev->dev, xm_altmode_probe_done,
					       bcdev);
		if (rc < 0) {
			pr_info("xiaomi altmode notifier unavailable rc=%d\n", rc);
			return 0;
		}

		bcdev->xm_altmode_notifier_registered = true;
		pr_info("xiaomi altmode registration deferred\n");
		return 0;
	}

	if (rc < 0) {
		pr_info("xiaomi altmode client unavailable rc=%d\n", rc);
		return 0;
	}

	return 0;
}

void qti_battery_charger_xiaomi_deinit(struct battery_chg_dev *bcdev)
{
	cancel_delayed_work_sync(&bcdev->xm_pd_renegotiation_work);

	if (bcdev->xm_altmode_notifier_registered) {
		altmode_deregister_notifier(bcdev->dev, bcdev);
		bcdev->xm_altmode_notifier_registered = false;
	}

	if (!bcdev->xm_altmode_registered || !bcdev->xm_altmode_client)
		return;

	altmode_deregister_client(bcdev->xm_altmode_client);
	bcdev->xm_altmode_client = NULL;
	bcdev->xm_altmode_registered = false;
}

#if defined(CONFIG_MI_WIRELESS)
static int write_wls_bin_prop_id(struct battery_chg_dev *bcdev,
				 struct psy_state *pst, u32 prop_id,
				 u16 total_length, u8 serial_number, u8 fw_area,
				 u8 *buff)
{
	struct xm_set_wls_bin_req_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_set;
	req_msg.total_length = total_length;
	req_msg.serial_number = serial_number;
	req_msg.fw_area = fw_area;
	if (serial_number < total_length / MAX_STR_LEN)
		memcpy(req_msg.wls_fw_bin, buff, MAX_STR_LEN);
	else if (serial_number == total_length / MAX_STR_LEN)
		memcpy(req_msg.wls_fw_bin, buff,
		       total_length - serial_number * MAX_STR_LEN);

	pr_debug("psy: prop_id:%d size:%d\n", req_msg.property_id,
		 sizeof(req_msg));

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int show_wls_fw_property_id(struct battery_chg_dev *bcdev,
				   struct psy_state *pst, u32 prop_id)
{
	struct wls_fw_resp_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_get;

	pr_debug("psy: %s prop_id: %u\n", pst->psy->desc->name,
		 req_msg.property_id);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

static int update_wls_fw_version(struct battery_chg_dev *bcdev,
				 struct psy_state *pst, u32 prop_id, u32 val)
{
	struct wls_fw_resp_msg req_msg = { { 0 } };

	req_msg.property_id = prop_id;
	req_msg.value = val;
	req_msg.hdr.owner = MSG_OWNER_BC;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = pst->opcode_set;

	pr_debug("psy: %s prop_id: %u val: %u\n", pst->psy->desc->name,
		 req_msg.property_id, val);

	return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}
#endif

static ssize_t wireless_register_store(struct class *c,
				       struct class_attribute *attr,
				       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	u32 val = 0;
	u8 reg_val;
	u16 reg_addr;
	ssize_t len;
	char *p, *token;
	char kbuf[50];
	int rc;

	pr_info("wireless_register_store: buf is %s\n", buf);

	len = min(count, sizeof(kbuf) - 1);
	memcpy(kbuf, buf, len);

	kbuf[len] = '\0';
	p = kbuf;

	token = strsep(&p, " ");
	if (!token)
		return -EINVAL;

	if (kstrtou16(token, 0, &reg_addr) || !reg_addr)
		return -EINVAL;

	if (kstrtou8(p, 0, &reg_val))
		return -EINVAL;

	val = reg_val | (reg_addr << 8);

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_WLS],
			       WLS_REGISTER, val);
	if (rc < 0)
		return rc;

	return count;
}
static CLASS_ATTR_WO(wireless_register);

static ssize_t wireless_input_curr_store(struct class *c,
					 struct class_attribute *attr,
					 const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	u32 val;

	if (kstrtou32(buf, 0, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_WLS],
			       WLS_INPUT_CURR, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t wireless_input_curr_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_WLS];
	int rc;

	rc = read_property_id(bcdev, pst, WLS_INPUT_CURR);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[WLS_INPUT_CURR]);
}
static CLASS_ATTR_RW(wireless_input_curr);

#if defined(CONFIG_MI_WIRELESS)
static ssize_t wireless_chip_fw_store(struct class *c,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_WLS];
	int rc;
	u32 val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	rc = update_wls_fw_version(bcdev, pst, WLS_FW_VER, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t wireless_chip_fw_show(struct class *c,
				     struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_WLS];
	int rc;

	rc = show_wls_fw_property_id(bcdev, pst, WLS_FW_VER);
	//rc = read_property_id(bcdev, pst, WLS_FW_VER);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%s\n", pst->version);
}
static CLASS_ATTR_RW(wireless_chip_fw);
#endif

static ssize_t real_type_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_REAL_TYPE);
	if (rc < 0)
		return rc;

	/* sanity check to avoid real_type value above maxium 13(USB_FLOAT) to cause kernel crash */
	if (pst->prop[XM_PROP_REAL_TYPE] > 13)
		pst->prop[XM_PROP_REAL_TYPE] = 0;

	return scnprintf(
		buf, PAGE_SIZE, "%s\n",
		power_supply_usb_type_text[pst->prop[XM_PROP_REAL_TYPE]]);
}
static CLASS_ATTR_RO(real_type);

static ssize_t resistance_id_show(struct class *c, struct class_attribute *attr,
				  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_RESISTANCE_ID);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_RESISTANCE_ID]);
}
static CLASS_ATTR_RO(resistance_id);

static ssize_t verify_digest_store(struct class *c,
				   struct class_attribute *attr,
				   const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	u8 random[BATTERY_DIGEST_LEN] = { 0 };
	char kbuf[2 * BATTERY_DIGEST_LEN + 1] = { 0 };
	u8 random_1s[BATTERY_DIGEST_LEN] = { 0 };
	char kbuf_1s[2 * BATTERY_DIGEST_LEN + 1] = { 0 };
	int rc;
	int i;

	if (bcdev->support_2s_charging) {
		memset(kbuf, 0, sizeof(kbuf));
		strlcpy(kbuf, buf, 2 * BATTERY_DIGEST_LEN + 1);
		if (StringToHex(kbuf, random, &i) < 0)
			return -EINVAL;
		rc = write_verify_digest_prop_id(bcdev,
						 &bcdev->psy_list[PSY_TYPE_XM],
						 XM_PROP_VERIFY_DIGEST, random);
	} else {
		size_t len = min_t(size_t, count, sizeof(kbuf_1s) - 1);

		memset(kbuf_1s, 0, sizeof(kbuf_1s));
		memcpy(kbuf_1s, buf, len);
		if (len && kbuf_1s[len - 1] == '\n')
			kbuf_1s[len - 1] = '\0';
		if (StringToHex(kbuf_1s, random_1s, &i) < 0)
			return -EINVAL;
		rc = write_verify_digest_prop_id(bcdev,
						 &bcdev->psy_list[PSY_TYPE_XM],
						 XM_PROP_VERIFY_DIGEST,
						 random_1s);
	}
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t verify_digest_show(struct class *c, struct class_attribute *attr,
				  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;
	u8 digest_buf[4];
	int i;
	int len;

	rc = read_verify_digest_property_id(bcdev, pst, XM_PROP_VERIFY_DIGEST);
	if (rc < 0)
		return rc;

	for (i = 0; i < BATTERY_DIGEST_LEN; i++) {
		memset(digest_buf, 0, sizeof(digest_buf));
		snprintf(digest_buf, sizeof(digest_buf) - 1, "%02x",
			 bcdev->digest[i]);
		strlcat(buf, digest_buf, BATTERY_DIGEST_LEN * 2 + 1);
	}
	len = strlen(buf);
	buf[len] = '\0';
	pr_err("verify_digest_show :%s \n", buf);
	return strlen(buf) + 1;
}
static CLASS_ATTR_RW(verify_digest);

static ssize_t verify_slave_flag_store(struct class *c,
				       struct class_attribute *attr,
				       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	bcdev->slave_fg_verify_flag = val;
	pr_err("verify_digest_flag :%d \n", val);

	return count;
}

static ssize_t verify_slave_flag_show(struct class *c,
				      struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);

	return scnprintf(buf, PAGE_SIZE, "%u\n", bcdev->slave_fg_verify_flag);
}
static CLASS_ATTR_RW(verify_slave_flag);

#if defined(CONFIG_MI_WIRELESS)
static ssize_t wls_bin_store(struct class *c, struct class_attribute *attr,
			     const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc, retry, tmp_serial;
	static u16 total_length = 0;
	static u8 serial_number = 0;
	static u8 fw_area = 0;
	int i;

	pr_err("buf:%s, count:%d\n", buf, count);
	if (strncmp("length:", buf, 7) == 0) {
		if (kstrtou16(buf + 7, 10, &total_length))
			return -EINVAL;
		serial_number = 0;
		pr_err("total_length:%d, serial_number:%d\n", total_length,
		       serial_number);
	} else if (strncmp("area:", buf, 5) == 0) {
		if (kstrtou8(buf + 5, 10, &fw_area))
			return -EINVAL;
		pr_err("area:%d\n", fw_area);
	} else {
		for (i = 0; i < count; ++i)
			pr_err("wls_bin_data[%d]=%x\n",
			       serial_number * MAX_STR_LEN + i, buf[i]);

		for (tmp_serial = 0;
		     (tmp_serial < (count + MAX_STR_LEN - 1) / MAX_STR_LEN) &&
		     (serial_number <
		      (total_length + MAX_STR_LEN - 1) / MAX_STR_LEN);
		     ++tmp_serial, ++serial_number) {
			for (retry = 0; retry < 3; ++retry) {
				rc = write_wls_bin_prop_id(
					bcdev, &bcdev->psy_list[PSY_TYPE_XM],
					XM_PROP_WLS_BIN, total_length,
					serial_number, fw_area,
					(u8 *)buf + tmp_serial * MAX_STR_LEN);
				pr_err("total_length:%d, serial_number:%d, retry:%d\n",
				       total_length, serial_number, retry);
				if (rc == 0)
					break;
			}
		}
	}
	return count;
}
static CLASS_ATTR_WO(wls_bin);
#endif

static ssize_t connector_temp_store(struct class *c,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_CONNECTOR_TEMP, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t connector_temp_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_CONNECTOR_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_CONNECTOR_TEMP]);
}
static CLASS_ATTR_RW(connector_temp);

static ssize_t authentic_store(struct class *c, struct class_attribute *attr,
			       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	pr_err("authentic_store: %d\n", val);
	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_AUTHENTIC, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t authentic_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_AUTHENTIC);
	if (rc < 0)
		return rc;

	pr_err("authentic_show: %d\n", pst->prop[XM_PROP_AUTHENTIC]);
	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_AUTHENTIC]);
}
static CLASS_ATTR_RW(authentic);

static ssize_t chip_ok_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_CHIP_OK);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_CHIP_OK]);
}
static CLASS_ATTR_RO(chip_ok);

#if defined(CONFIG_DUAL_FUEL_GAUGE)
static ssize_t slave_chip_ok_show(struct class *c, struct class_attribute *attr,
				  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SLAVE_CHIP_OK);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_SLAVE_CHIP_OK]);
}
static CLASS_ATTR_RO(slave_chip_ok);

static ssize_t slave_authentic_store(struct class *c,
				     struct class_attribute *attr,
				     const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	pr_err("slave_authentic_store: %d\n", val);

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_SLAVE_AUTHENTIC, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t slave_authentic_show(struct class *c,
				    struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SLAVE_AUTHENTIC);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_SLAVE_AUTHENTIC]);
}
static CLASS_ATTR_RW(slave_authentic);

static ssize_t fg1_vol_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_VOL);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FG1_VOL]);
}
static CLASS_ATTR_RO(fg1_vol);

static ssize_t fg1_soc_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_SOC);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FG1_SOC]);
}
static CLASS_ATTR_RO(fg1_soc);

static ssize_t fg1_temp_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TEMP]);
}
static CLASS_ATTR_RO(fg1_temp);

static ssize_t fg1_ibatt_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_IBATT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_IBATT]);
}
static CLASS_ATTR_RO(fg1_ibatt);

static ssize_t fg2_vol_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_VOL);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FG2_VOL]);
}
static CLASS_ATTR_RO(fg2_vol);

static ssize_t fg2_soc_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_SOC);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FG2_SOC]);
}
static CLASS_ATTR_RO(fg2_soc);

static ssize_t fg2_temp_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_TEMP]);
}
static CLASS_ATTR_RO(fg2_temp);

static ssize_t fg2_ibatt_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_IBATT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_IBATT]);
}
static CLASS_ATTR_RO(fg2_ibatt);

static ssize_t fg2_qmax_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_QMAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_QMAX]);
}
static CLASS_ATTR_RO(fg2_qmax);

static ssize_t fg2_rm_show(struct class *c, struct class_attribute *attr,
			   char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_RM);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_RM]);
}
static CLASS_ATTR_RO(fg2_rm);

static ssize_t fg2_fcc_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_FCC);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_FCC]);
}
static CLASS_ATTR_RO(fg2_fcc);

static ssize_t fg2_soh_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_SOH);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_SOH]);
}
static CLASS_ATTR_RO(fg2_soh);

static ssize_t fg2_fcc_soh_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_FCC_SOH);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG2_FCC_SOH]);
}
static CLASS_ATTR_RO(fg2_fcc_soh);

static ssize_t fg2_cycle_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_CYCLE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_CYCLE]);
}
static CLASS_ATTR_RO(fg2_cycle);

static ssize_t fg2_fastcharge_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_FAST_CHARGE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG2_FAST_CHARGE]);
}
static CLASS_ATTR_RO(fg2_fastcharge);

static ssize_t fg2_current_max_show(struct class *c,
				    struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_CURRENT_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG2_CURRENT_MAX]);
}
static CLASS_ATTR_RO(fg2_current_max);

static ssize_t fg2_vol_max_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_VOL_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG2_VOL_MAX]);
}
static CLASS_ATTR_RO(fg2_vol_max);

static ssize_t fg2_tsim_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_TSIM);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_TSIM]);
}
static CLASS_ATTR_RO(fg2_tsim);

static ssize_t fg2_tambient_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_TAMBIENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG2_TAMBIENT]);
}
static CLASS_ATTR_RO(fg2_tambient);

static ssize_t fg2_tremq_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_TREMQ);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_TREMQ]);
}
static CLASS_ATTR_RO(fg2_tremq);

static ssize_t fg2_tfullq_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG2_TFULLQ);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG2_TFULLQ]);
}
static CLASS_ATTR_RO(fg2_tfullq);

static ssize_t is_old_hw_store(struct class *c, struct class_attribute *attr,
			       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_IS_OLD_HW, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t is_old_hw_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_IS_OLD_HW);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_IS_OLD_HW]);
}
static CLASS_ATTR_RW(is_old_hw);
#endif

static ssize_t vbus_disable_store(struct class *c, struct class_attribute *attr,
				  const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_VBUS_DISABLE, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t vbus_disable_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_VBUS_DISABLE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_VBUS_DISABLE]);
}
static CLASS_ATTR_RW(vbus_disable);

static ssize_t cc_orientation_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_CC_ORIENTATION);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_CC_ORIENTATION]);
}
static CLASS_ATTR_RO(cc_orientation);

static ssize_t slave_batt_present_show(struct class *c,
				       struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SLAVE_BATT_PRESENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_SLAVE_BATT_PRESENT]);
}
static CLASS_ATTR_RO(slave_batt_present);

#if defined(CONFIG_BQ2597X)
static ssize_t bq2597x_chip_ok_show(struct class *c,
				    struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_CHIP_OK);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_CHIP_OK]);
}
static CLASS_ATTR_RO(bq2597x_chip_ok);

static ssize_t bq2597x_slave_chip_ok_show(struct class *c,
					  struct class_attribute *attr,
					  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_SLAVE_CHIP_OK);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_SLAVE_CHIP_OK]);
}
static CLASS_ATTR_RO(bq2597x_slave_chip_ok);

static ssize_t bq2597x_bus_current_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_BUS_CURRENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_BUS_CURRENT]);
}
static CLASS_ATTR_RO(bq2597x_bus_current);

static ssize_t bq2597x_slave_bus_current_show(struct class *c,
					      struct class_attribute *attr,
					      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_SLAVE_BUS_CURRENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_SLAVE_BUS_CURRENT]);
}
static CLASS_ATTR_RO(bq2597x_slave_bus_current);

static ssize_t bq2597x_bus_delta_show(struct class *c,
				      struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_BUS_DELTA);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_BUS_DELTA]);
}
static CLASS_ATTR_RO(bq2597x_bus_delta);

static ssize_t bq2597x_bus_voltage_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_BUS_VOLTAGE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_BUS_VOLTAGE]);
}
static CLASS_ATTR_RO(bq2597x_bus_voltage);

static ssize_t bq2597x_battery_present_show(struct class *c,
					    struct class_attribute *attr,
					    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_BATTERY_PRESENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_BATTERY_PRESENT]);
}
static CLASS_ATTR_RO(bq2597x_battery_present);

static ssize_t bq2597x_slave_battery_present_show(struct class *c,
						  struct class_attribute *attr,
						  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst,
			      XM_PROP_BQ2597X_SLAVE_BATTERY_PRESENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_SLAVE_BATTERY_PRESENT]);
}
static CLASS_ATTR_RO(bq2597x_slave_battery_present);
static ssize_t bq2597x_battery_voltage_show(struct class *c,
					    struct class_attribute *attr,
					    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_BATTERY_VOLTAGE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BQ2597X_BATTERY_VOLTAGE]);
}
static CLASS_ATTR_RO(bq2597x_battery_voltage);

static ssize_t cool_mode_store(struct class *c, struct class_attribute *attr,
			       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_COOL_MODE, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t cool_mode_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_COOL_MODE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_COOL_MODE]);
}
static CLASS_ATTR_RW(cool_mode);
#endif

#if defined(CONFIG_REDWOOD_FOR_BUILD)
static ssize_t bq2597x_slave_connector_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BQ2597X_SLAVE_CONNECTOR);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_BQ2597X_SLAVE_CONNECTOR]);
}
static CLASS_ATTR_RO(bq2597x_slave_connector);
#endif

#if !defined(CONFIG_VENUS_FOR_BUILD)
static ssize_t bt_transfer_start_store(struct class *c,
				       struct class_attribute *attr,
				       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	pr_err("transfer_start_store %d build: 0x%x\n", val,
	       bcdev->hw_version_build);
	switch (bcdev->hw_version_build) {
	case 0x110001:
	case 0x10001:
	case 0x120000:
	case 0x20000:
	case 0x120005:
	case 0x120001:
	case 0x20001:
	case 0x90002:
		rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
				       XM_PROP_BT_TRANSFER_START, val);
		break;
	default:
		break;
	}

	return count;
}

static ssize_t bt_transfer_start_show(struct class *c,
				      struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BT_TRANSFER_START);
	if (rc < 0)
		return rc;
	pr_err("transfer_start_show %d\n",
	       pst->prop[XM_PROP_BT_TRANSFER_START]);
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_BT_TRANSFER_START]);
}
static CLASS_ATTR_RW(bt_transfer_start);
#endif

static ssize_t master_smb1396_online_show(struct class *c,
					  struct class_attribute *attr,
					  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_MASTER_SMB1396_ONLINE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_MASTER_SMB1396_ONLINE]);
}
static CLASS_ATTR_RO(master_smb1396_online);

static ssize_t master_smb1396_iin_show(struct class *c,
				       struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_MASTER_SMB1396_IIN);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_MASTER_SMB1396_IIN]);
}
static CLASS_ATTR_RO(master_smb1396_iin);

static ssize_t slave_smb1396_online_show(struct class *c,
					 struct class_attribute *attr,
					 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SLAVE_SMB1396_ONLINE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_SLAVE_SMB1396_ONLINE]);
}
static CLASS_ATTR_RO(slave_smb1396_online);

static ssize_t slave_smb1396_iin_show(struct class *c,
				      struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SLAVE_SMB1396_IIN);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_SLAVE_SMB1396_IIN]);
}
static CLASS_ATTR_RO(slave_smb1396_iin);

static ssize_t smb_iin_diff_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SMB_IIN_DIFF);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_SMB_IIN_DIFF]);
}
static CLASS_ATTR_RO(smb_iin_diff);

static ssize_t soc_decimal_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SOC_DECIMAL);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_SOC_DECIMAL]);
}
static CLASS_ATTR_RO(soc_decimal);

static ssize_t soc_decimal_rate_show(struct class *c,
				     struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SOC_DECIMAL_RATE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u",
			 pst->prop[XM_PROP_SOC_DECIMAL_RATE]);
}
static CLASS_ATTR_RO(soc_decimal_rate);

static ssize_t shutdown_delay_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SHUTDOWN_DELAY);
	if (rc < 0)
		return rc;

	if (!bcdev->shutdown_delay_en)
		pst->prop[XM_PROP_SHUTDOWN_DELAY] = 0;

	return scnprintf(buf, PAGE_SIZE, "%u",
			 pst->prop[XM_PROP_SHUTDOWN_DELAY]);
}

static ssize_t shutdown_delay_store(struct class *c,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	bcdev->shutdown_delay_en = val;
	pr_err("use contral shutdown delay featue enable= %d\n",
	       bcdev->shutdown_delay_en);

	return count;
}

static CLASS_ATTR_RW(shutdown_delay);

#if defined(CONFIG_MI_WIRELESS)
static ssize_t tx_mac_show(struct class *c, struct class_attribute *attr,
			   char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;
	u64 value = 0;

	rc = read_property_id(bcdev, pst, XM_PROP_TX_MACL);
	if (rc < 0)
		return rc;

	rc = read_property_id(bcdev, pst, XM_PROP_TX_MACH);
	if (rc < 0)
		return rc;
	value = pst->prop[XM_PROP_TX_MACH];
	value = (value << 32) + pst->prop[XM_PROP_TX_MACL];

	return scnprintf(buf, PAGE_SIZE, "%llx", value);
}
static CLASS_ATTR_RO(tx_mac);

static ssize_t rx_cr_show(struct class *c, struct class_attribute *attr,
			  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;
	u64 value = 0;
	rc = read_property_id(bcdev, pst, XM_PROP_RX_CRL);
	if (rc < 0)
		return rc;

	rc = read_property_id(bcdev, pst, XM_PROP_RX_CRH);
	if (rc < 0)
		return rc;
	value = pst->prop[XM_PROP_RX_CRH];
	value = (value << 32) + pst->prop[XM_PROP_RX_CRL];

	return scnprintf(buf, PAGE_SIZE, "%llx", value);
}
static CLASS_ATTR_RO(rx_cr);

static ssize_t rx_cep_show(struct class *c, struct class_attribute *attr,
			   char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_RX_CEP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%x", pst->prop[XM_PROP_RX_CEP]);
}
static CLASS_ATTR_RO(rx_cep);

static ssize_t bt_state_store(struct class *c, struct class_attribute *attr,
			      const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_BT_STATE, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t bt_state_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_BT_STATE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_BT_STATE]);
}
static CLASS_ATTR_RW(bt_state);
#endif

static ssize_t voter_debug_store(struct class *c, struct class_attribute *attr,
				 const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_VOTER_DEBUG, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t voter_debug_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_VOTER_DEBUG);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_VOTER_DEBUG]);
}
static CLASS_ATTR_RW(voter_debug);

static ssize_t wlscharge_control_limit_store(struct class *c,
					     struct class_attribute *attr,
					     const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (!bcdev->support_wireless_charge)
		return -EINVAL;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val == bcdev->curr_wlsthermal_level)
		return count;

	pr_err("set thermal-level: %d num_thermal_levels: %d \n", val,
	       bcdev->num_thermal_levels);

	if (bcdev->num_thermal_levels <= 0) {
		pr_err("Incorrect num_thermal_levels\n");
		return -EINVAL;
	}

	if (val < 0 || val >= bcdev->num_thermal_levels)
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_WLSCHARGE_CONTROL_LIMIT, val);
	if (rc < 0)
		return rc;

	bcdev->curr_wlsthermal_level = val;

	return count;
}

static ssize_t wlscharge_control_limit_show(struct class *c,
					    struct class_attribute *attr,
					    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	if (!bcdev->support_wireless_charge)
		return -EINVAL;

	rc = read_property_id(bcdev, pst, XM_PROP_WLSCHARGE_CONTROL_LIMIT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u",
			 pst->prop[XM_PROP_WLSCHARGE_CONTROL_LIMIT]);
}
static CLASS_ATTR_RW(wlscharge_control_limit);

#if defined(CONFIG_MI_WIRELESS)
static ssize_t reverse_chg_mode_store(struct class *c,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_REVERSE_CHG_MODE, val);
	if (rc < 0)
		return rc;
	return count;
}

static ssize_t reverse_chg_mode_show(struct class *c,
				     struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_REVERSE_CHG_MODE);
	if (rc < 0)
		goto out;

	if (bcdev->reverse_chg_flag != pst->prop[XM_PROP_REVERSE_CHG_MODE]) {
		if (pst->prop[XM_PROP_REVERSE_CHG_MODE]) {
			pm_stay_awake(bcdev->dev);
			dev_info(bcdev->dev, "reverse chg add lock\n");
		} else {
			pm_relax(bcdev->dev);
			dev_info(bcdev->dev, "reverse chg release lock\n");
		}
		bcdev->reverse_chg_flag = pst->prop[XM_PROP_REVERSE_CHG_MODE];
	}

	return scnprintf(buf, PAGE_SIZE, "%u",
			 pst->prop[XM_PROP_REVERSE_CHG_MODE]);

out:
	dev_err(bcdev->dev, "read reverse chg mode error\n");
	bcdev->reverse_chg_flag = 0;
	pm_relax(bcdev->dev);
	return rc;
}
static CLASS_ATTR_RW(reverse_chg_mode);

#if !defined(CONFIG_VENUS_FOR_BUILD)
static ssize_t wls_tx_speed_store(struct class *c, struct class_attribute *attr,
				  const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;
	if (kstrtoint(buf, 10, &val))
		return -EINVAL;
	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_WLS_TX_SPEED, val);
	if (rc < 0)
		return rc;
	return count;
}

static ssize_t wls_tx_speed_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_WLS_TX_SPEED);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_WLS_TX_SPEED]);
}
static CLASS_ATTR_RW(wls_tx_speed);
#endif

static ssize_t reverse_chg_state_show(struct class *c,
				      struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_REVERSE_CHG_STATE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u",
			 pst->prop[XM_PROP_REVERSE_CHG_STATE]);
}
static CLASS_ATTR_RO(reverse_chg_state);
#if 0
static ssize_t wls_fw_state_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_WLS_FW_STATE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_WLS_FW_STATE]);
}
static CLASS_ATTR_RO(wls_fw_state);
#endif
static ssize_t wls_car_adapter_show(struct class *c,
				    struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_WLS_CAR_ADAPTER);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u",
			 pst->prop[XM_PROP_WLS_CAR_ADAPTER]);
}
static CLASS_ATTR_RO(wls_car_adapter);

static ssize_t rx_vout_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_RX_VOUT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_RX_VOUT]);
}
static CLASS_ATTR_RO(rx_vout);

static ssize_t rx_vrect_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_RX_VRECT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_RX_VRECT]);
}
static CLASS_ATTR_RO(rx_vrect);

static ssize_t rx_iout_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_RX_IOUT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_RX_IOUT]);
}
static CLASS_ATTR_RO(rx_iout);

static ssize_t tx_adapter_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_TX_ADAPTER);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_TX_ADAPTER]);
}
static CLASS_ATTR_RO(tx_adapter);

static ssize_t op_mode_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_OP_MODE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_OP_MODE]);
}
static CLASS_ATTR_RO(op_mode);

static ssize_t wls_die_temp_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_WLS_DIE_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u", pst->prop[XM_PROP_WLS_DIE_TEMP]);
}
static CLASS_ATTR_RO(wls_die_temp);
#endif

static ssize_t power_max_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *xm_pst = &bcdev->psy_list[PSY_TYPE_XM];
	union power_supply_propval val = {
		0,
	};
	struct power_supply *usb_psy = NULL;
	int rc, usb_present = 0;
	usb_psy = bcdev->psy_list[PSY_TYPE_USB].psy;
	if (usb_psy != NULL) {
		rc = usb_psy_get_prop(usb_psy, POWER_SUPPLY_PROP_ONLINE, &val);
		if (!rc)
			usb_present = val.intval;
		else
			usb_present = 0;
		pr_err("usb_present: %d\n", usb_present);
	}
	if (usb_present) {
		rc = read_property_id(bcdev, xm_pst, XM_PROP_APDO_MAX);
		if (rc < 0)
			return rc;
		return scnprintf(buf, PAGE_SIZE, "%u",
				 xm_pst->prop[XM_PROP_APDO_MAX]);
	}
	pr_err("tx_adapter:%d\n", xm_pst->prop[XM_PROP_TX_ADAPTER]);
#if defined(CONFIG_MI_WIRELESS)
	switch (xm_pst->prop[XM_PROP_TX_ADAPTER]) {
	case ADAPTER_XIAOMI_PD_50W:
		return scnprintf(buf, PAGE_SIZE, "%u", 50);
	case ADAPTER_XIAOMI_PD_60W:
	case ADAPTER_XIAOMI_PD_100W:
		return scnprintf(buf, PAGE_SIZE, "%u", 67);
	case ADAPTER_XIAOMI_PD_30W:
	case ADAPTER_VOICE_BOX_30W:
		return scnprintf(buf, PAGE_SIZE, "%u", 30);
	case ADAPTER_XIAOMI_QC3_20W:
	case ADAPTER_XIAOMI_PD_20W:
	case ADAPTER_XIAOMI_CAR_20W:
		return scnprintf(buf, PAGE_SIZE, "%u", 20);
	default:
		return scnprintf(buf, PAGE_SIZE, "%u", 0);
	}
#endif
	return scnprintf(buf, PAGE_SIZE, "%u", 0);
}
static CLASS_ATTR_RO(power_max);

static ssize_t input_suspend_store(struct class *c,
				   struct class_attribute *attr,
				   const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	unsigned int val;
	int rc;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;

	if (val > 2)
		return -EINVAL;

	pr_err("set charger input suspend %u\n", val);

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_INPUT_SUSPEND, val);
	if (rc < 0)
		return rc;

	if (val)
		cancel_delayed_work_sync(&bcdev->xm_pd_renegotiation_work);
	else if (bcdev->xm_uvdm_compat_verified)
		xm_pd_schedule_renegotiation(bcdev, "input_suspend_store",
					     XM_PD_RENEGOTIATION_DELAY_MS);

	return count;
}

static ssize_t input_suspend_show(struct class *c, struct class_attribute *attr,
				  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_INPUT_SUSPEND);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_INPUT_SUSPEND]);
}
static CLASS_ATTR_RW(input_suspend);

static ssize_t night_charging_store(struct class *c,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	pr_err("set charger night charging %d\n", val);

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_NIGHT_CHARGING, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t night_charging_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_NIGHT_CHARGING);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_NIGHT_CHARGING]);
}
static CLASS_ATTR_RW(night_charging);

#if !defined(CONFIG_VENUS_FOR_BUILD)
static ssize_t smart_batt_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	pr_err("set smart batt charging %d\n", val);

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_SMART_BATT, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t smart_batt_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SMART_BATT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_SMART_BATT]);
}
static CLASS_ATTR_RW(smart_batt);
#endif

static ssize_t verify_process_store(struct class *c,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_VERIFY_PROCESS, val);
	if (rc < 0)
		return rc;

	if (val || !bcdev->xm_uvdm_compat_verified)
		xm_pd_auth_compat_reset(bcdev, usbpd_is_pd_active(bcdev),
					val ? "verify_process_start" :
					      "verify_process_stop");

	return count;
}

static ssize_t verify_process_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_VERIFY_PROCESS);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_VERIFY_PROCESS]);
}
static CLASS_ATTR_RW(verify_process);

static void usbpd_sha256_bitswap32(unsigned int *array, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		array[i] = BSWAP_32(array[i]);
	}
}

static void usbpd_request_vdm_cmd(struct battery_chg_dev *bcdev,
				  enum uvdm_state cmd, unsigned int *data)
{
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	u32 prop_id, val = 0;
	int rc;

	pr_info("usbpd_request_vdm_cmd: cmd=%d\n", cmd);
	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		prop_id = XM_PROP_VDM_CMD_CHARGER_VERSION;
		break;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		prop_id = XM_PROP_VDM_CMD_CHARGER_VOLTAGE;
		break;
	case USBPD_UVDM_CHARGER_TEMP:
		prop_id = XM_PROP_VDM_CMD_CHARGER_TEMP;
		break;
	case USBPD_UVDM_SESSION_SEED:
		prop_id = XM_PROP_VDM_CMD_SESSION_SEED;
		usbpd_sha256_bitswap32(data, USBPD_UVDM_SS_LEN);
		val = *data;
		pr_debug("SESSION_SEED:data = %d\n", val);
		break;
	case USBPD_UVDM_AUTHENTICATION:
		prop_id = XM_PROP_VDM_CMD_AUTHENTICATION;
		usbpd_sha256_bitswap32(data, USBPD_UVDM_SS_LEN);
		val = *data;
		pr_debug("AUTHENTICATION:data = %d\n", val);
		break;
#if !defined(CONFIG_VENUS_FOR_BUILD)
	case USBPD_UVDM_REVERSE_AUTHEN:
		prop_id = XM_PROP_VDM_CMD_REVERSE_AUTHEN;
		usbpd_sha256_bitswap32(data, USBPD_UVDM_SS_LEN);
		val = *data;
		pr_debug("AUTHENTICATION:data = %d\n", val);
		break;
#endif
	case USBPD_UVDM_REMOVE_COMPENSATION:
		prop_id = XM_PROP_VDM_CMD_REMOVE_COMPENSATION;
		val = *data;
		break;
	case USBPD_UVDM_VERIFIED:
		prop_id = XM_PROP_VDM_CMD_VERIFIED;
		val = *data;
		break;
	default:
		prop_id = XM_PROP_VDM_CMD_CHARGER_VERSION;
		pr_info("cmd:%d is not support\n", cmd);
		break;
	}

	if (cmd == USBPD_UVDM_SESSION_SEED ||
	    cmd == USBPD_UVDM_AUTHENTICATION ||
	    cmd == USBPD_UVDM_REVERSE_AUTHEN) {
		rc = write_ss_auth_prop_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
					   prop_id, data);
	} else
		rc = write_property_id(bcdev, pst, prop_id, val);

	xm_altmode_send_uvdm(bcdev, cmd, data);
	xm_pd_auth_compat_advance(bcdev, cmd, data);

	if (rc < 0)
		pr_err("usbpd_request_vdm_cmd: failed cmd=%d prop=%u rc=%d\n",
		       cmd, prop_id, rc);
	else
		pr_info("usbpd_request_vdm_cmd: sent cmd=%d prop=%u\n",
			cmd, prop_id);
}

static bool usbpd_vdm_cmd_requires_data(enum uvdm_state cmd)
{
	switch (cmd) {
	case USBPD_UVDM_SESSION_SEED:
	case USBPD_UVDM_AUTHENTICATION:
#if !defined(CONFIG_VENUS_FOR_BUILD)
	case USBPD_UVDM_REVERSE_AUTHEN:
#endif
	case USBPD_UVDM_REMOVE_COMPENSATION:
		return true;
	default:
		return false;
	}
}

static ssize_t request_vdm_cmd_store(struct class *c,
				     struct class_attribute *attr,
				     const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int cmd;
	char kbuf[64];
	char buffer[2 * BATTERY_SS_AUTH_DATA_LEN * sizeof(u32) + 1];
	char *cmd_str, *payload;
	u8 data[BATTERY_SS_AUTH_DATA_LEN * sizeof(u32)];
	bool null_payload;
	int ccount;
	size_t len;

	memset(kbuf, 0, sizeof(kbuf));
	memset(buffer, 0, sizeof(buffer));
	memset(data, 0, sizeof(data));

	len = min_t(size_t, count, sizeof(kbuf) - 1);
	memcpy(kbuf, buf, len);
	cmd_str = strim(kbuf);
	payload = strchr(cmd_str, ',');
	if (payload) {
		*payload = '\0';
		payload = strim(payload + 1);
	}

	if (kstrtoint(strim(cmd_str), 0, &cmd))
		return -EINVAL;

	null_payload = !payload || !payload[0] ||
		!strcasecmp(payload, "null");

	if (usbpd_vdm_cmd_requires_data(cmd) && null_payload)
		return -EINVAL;

	if (payload && !null_payload)
		strscpy(buffer, payload, sizeof(buffer));

	pr_info("%s: cmd=%d payload=%u null=%u\n", __func__, cmd,
		!!(payload && !null_payload), null_payload);

	if (payload && !null_payload && StringToHex(buffer, data, &ccount) < 0)
		return -EINVAL;
	if (payload && !null_payload)
		xm_uvdm_capture_cmd_payload(bcdev, cmd, data, ccount);
	if (cmd == USBPD_UVDM_VERIFIED && null_payload)
		data[0] = 1;
	usbpd_request_vdm_cmd(bcdev, cmd, (unsigned int *)data);
	return count;
}

static ssize_t request_vdm_cmd_show(struct class *c,
				    struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;
	u32 prop_id = 0;
	int i;
	char data[16], str_buf[128] = { 0 };
	enum uvdm_state cmd;

	rc = read_property_id(bcdev, pst, XM_PROP_UVDM_STATE);
	if (rc < 0)
		return rc;

	cmd = pst->prop[XM_PROP_UVDM_STATE];
	if (!usbpd_is_pd_active(bcdev)) {
		if (cmd != USBPD_UVDM_DISCONNECT)
			pr_info_ratelimited("request_vdm_cmd_show: suppress stale uvdm_state=%u while PD inactive\n",
					    cmd);
		xm_pd_auth_compat_update(bcdev);
		return snprintf(buf, PAGE_SIZE, "%d,Null",
				USBPD_UVDM_DISCONNECT);
	}

	xm_pd_auth_compat_update(bcdev);
	if (bcdev->xm_pd_auth_compat) {
		if (bcdev->xm_uvdm_state == USBPD_UVDM_DISCONNECT)
			bcdev->xm_uvdm_state = USBPD_UVDM_CHARGER_VERSION;
		if (bcdev->xm_uvdm_ack_pending) {
			cmd = bcdev->xm_uvdm_last_cmd;
			bcdev->xm_uvdm_ack_pending = false;
		} else {
			cmd = bcdev->xm_uvdm_state;
		}
	}
	pr_debug("request_vdm_cmd_show uvdm_state=%d compat=%d last=%u ack=%d\n",
		 cmd, bcdev->xm_pd_auth_compat, bcdev->xm_uvdm_last_cmd,
		 bcdev->xm_uvdm_ack_pending);
	pr_info_ratelimited("request_vdm_cmd_show: cmd=%d compat=%u last=%u ack=%u\n",
			    cmd, bcdev->xm_pd_auth_compat,
			    bcdev->xm_uvdm_last_cmd,
			    bcdev->xm_uvdm_ack_pending);

	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		prop_id = XM_PROP_VDM_CMD_CHARGER_VERSION;
		rc = read_property_id(bcdev, pst, prop_id);
		return snprintf(buf, PAGE_SIZE, "%d,%d", cmd,
				pst->prop[prop_id]);
		break;
	case USBPD_UVDM_CHARGER_TEMP:
		prop_id = XM_PROP_VDM_CMD_CHARGER_TEMP;
		rc = read_property_id(bcdev, pst, prop_id);
		return snprintf(buf, PAGE_SIZE, "%d,%d", cmd,
				pst->prop[prop_id]);
		break;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		prop_id = XM_PROP_VDM_CMD_CHARGER_VOLTAGE;
		rc = read_property_id(bcdev, pst, prop_id);
		return snprintf(buf, PAGE_SIZE, "%d,%d", cmd,
				pst->prop[prop_id]);
		break;
	case USBPD_UVDM_CONNECT:
	case USBPD_UVDM_DISCONNECT:
	case USBPD_UVDM_SESSION_SEED:
	case USBPD_UVDM_VERIFIED:
	case USBPD_UVDM_REMOVE_COMPENSATION:
	case USBPD_UVDM_REVERSE_AUTHEN:
		return snprintf(buf, PAGE_SIZE, "%d,Null", cmd);
		break;
	case USBPD_UVDM_AUTHENTICATION:
		prop_id = XM_PROP_VDM_CMD_AUTHENTICATION;
		if (bcdev->xm_uvdm_auth_payload_valid) {
			for (i = 0; i < USBPD_UVDM_SS_LEN; i++) {
				u32 word = get_unaligned_be32(
					&bcdev->xm_uvdm_auth_response
					 [i * sizeof(u32)]);

				memset(data, 0, sizeof(data));
				snprintf(data, sizeof(data), "%08x", word);
				strlcat(str_buf, data, sizeof(str_buf));
			}
			pr_info("auth compat row=%d response=%s\n",
				bcdev->xm_uvdm_auth_row + 1, str_buf);
			return snprintf(buf, PAGE_SIZE, "%d,%s", cmd, str_buf);
		}

		rc = read_ss_auth_property_id(bcdev, pst, prop_id);
		if (rc < 0)
			return rc;
		pr_info("auth:0x%x 0x%x 0x%x 0x%x\n", bcdev->ss_auth_data[0],
			bcdev->ss_auth_data[1], bcdev->ss_auth_data[2],
			bcdev->ss_auth_data[3]);
		for (i = 0; i < USBPD_UVDM_SS_LEN; i++) {
			memset(data, 0, sizeof(data));
			snprintf(data, sizeof(data), "%08x",
				 bcdev->ss_auth_data[i]);
			strlcat(str_buf, data, sizeof(str_buf));
		}
		return snprintf(buf, PAGE_SIZE, "%d,%s", cmd, str_buf);
		break;
	default:
		pr_info("feedbak cmd:%d is not support\n", cmd);
		break;
	}

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[prop_id]);
}
static CLASS_ATTR_RW(request_vdm_cmd);

static const char *const usbpd_state_strings[] = {
	"UNKNOWN",
	"SNK_Startup",
	"SNK_Ready",
	"SRC_Ready",
};

static ssize_t current_state_show(struct class *c, struct class_attribute *attr,
				  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_CURRENT_STATE);
	if (rc < 0)
		return rc;
	if (pst->prop[XM_PROP_CURRENT_STATE] == 25)
		return snprintf(buf, PAGE_SIZE, "%s", usbpd_state_strings[1]);
	else if (pst->prop[XM_PROP_CURRENT_STATE] == 31)
		return snprintf(buf, PAGE_SIZE, "%s", usbpd_state_strings[2]);
	else if (pst->prop[XM_PROP_CURRENT_STATE] == 5)
		return snprintf(buf, PAGE_SIZE, "%s", usbpd_state_strings[3]);
	else
		return snprintf(buf, PAGE_SIZE, "%s", usbpd_state_strings[0]);
}
static CLASS_ATTR_RO(current_state);

static ssize_t adapter_id_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	u32 adapter_id;
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_ADAPTER_ID);
	if (rc < 0)
		return rc;

	adapter_id = pst->prop[XM_PROP_ADAPTER_ID];

	if (bcdev->xm_adapter_id_override) {
		pr_info_ratelimited("adapter_id_show: override raw=%08x effective=%08x\n",
				    adapter_id,
				    bcdev->xm_adapter_id_override);
		return scnprintf(buf, PAGE_SIZE, "%08x",
				 bcdev->xm_adapter_id_override);
	}

	xm_pd_auth_compat_update(bcdev);
	if (bcdev->xm_pd_auth_compat &&
	    adapter_id == ADAPTER_NONE) {
		pr_info_ratelimited("adapter_id_show: expose Xiaomi PD30 raw=%08x compat=1\n",
				    adapter_id);
		return scnprintf(buf, PAGE_SIZE, "%08x",
				 XM_PD_COMPAT_ADAPTER_ID);
	}

	return scnprintf(buf, PAGE_SIZE, "%08x", adapter_id);
}
static CLASS_ATTR_RO(adapter_id);

static ssize_t adapter_id_override_show(struct class *c,
					struct class_attribute *attr,
					char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);

	return scnprintf(buf, PAGE_SIZE, "%08x\n",
			 bcdev->xm_adapter_id_override);
}

static ssize_t adapter_id_override_store(struct class *c,
					 struct class_attribute *attr,
					 const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	u32 val;

	if (kstrtou32(buf, 0, &val))
		return -EINVAL;

	bcdev->xm_adapter_id_override = val;
	bcdev->xm_uvdm_compat_verified = false;
	bcdev->xm_pd_power_profile_applied = false;
	cancel_delayed_work_sync(&bcdev->xm_pd_renegotiation_work);

	pr_info("adapter_id_override_store: override=%08x\n", val);

	return count;
}
static CLASS_ATTR_RW(adapter_id_override);

static ssize_t adapter_svid_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_ADAPTER_SVID);
	if (rc < 0)
		return rc;

	xm_pd_auth_compat_update(bcdev);
	if (bcdev->xm_pd_auth_compat &&
	    pst->prop[XM_PROP_ADAPTER_SVID] == ADAPTER_NONE) {
		pr_info_ratelimited("adapter_svid_show: expose Xiaomi SVID raw=%04x compat=1\n",
				    pst->prop[XM_PROP_ADAPTER_SVID]);
		return scnprintf(buf, PAGE_SIZE, "%04x", XIAOMI_PD_SVID);
	}

	pr_info_ratelimited("adapter_svid_show: raw=%04x compat=%u\n",
			    pst->prop[XM_PROP_ADAPTER_SVID],
			    bcdev->xm_pd_auth_compat);

	return scnprintf(buf, PAGE_SIZE, "%04x",
			 pst->prop[XM_PROP_ADAPTER_SVID]);
}
static CLASS_ATTR_RO(adapter_svid);

static ssize_t pd_verifed_store(struct class *c, struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, pst, XM_PROP_PD_VERIFED, val);
	if (rc < 0)
		return rc;

	pr_info("pd_verifed_store: val=%u\n", val);

	if (val) {
		bcdev->xm_uvdm_compat_verified = true;
		xm_pd_apply_power_profile(bcdev, "pd_verifed_store");
		xm_pd_schedule_renegotiation(bcdev, "pd_verifed_store",
					     XM_PD_RENEGOTIATION_DELAY_MS);
	} else {
		cancel_delayed_work_sync(&bcdev->xm_pd_renegotiation_work);
		xm_pd_auth_compat_reset(bcdev, usbpd_is_pd_active(bcdev),
					"pd_verifed_clear");
	}

	return count;
}

static ssize_t pd_verifed_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_PD_VERIFED);
	if (rc < 0)
		return rc;

	pr_info_ratelimited("pd_verifed_show: fw=%u compat=%u\n",
			    pst->prop[XM_PROP_PD_VERIFED],
			    bcdev->xm_uvdm_compat_verified);

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_PD_VERIFED]);
}
static CLASS_ATTR_RW(pd_verifed);

static ssize_t xiaomi_pd_auth_debug_show(struct class *c,
					 struct class_attribute *attr,
					 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *usb_pst = &bcdev->psy_list[PSY_TYPE_USB];
	struct psy_state *xm_pst = &bcdev->psy_list[PSY_TYPE_XM];
	u32 usb_real_type = 0, current_state = 0, adapter_svid = 0;
	u32 adapter_id = 0;
	u32 pd_verified = 0, pdo2 = 0, apdo_max = 0;
	bool effective_verified;
	int len = 0;
	int rc;

	rc = read_property_id(bcdev, usb_pst, USB_REAL_TYPE);
	if (!rc)
		usb_real_type = usb_pst->prop[USB_REAL_TYPE];
	rc = read_property_id(bcdev, xm_pst, XM_PROP_CURRENT_STATE);
	if (!rc)
		current_state = xm_pst->prop[XM_PROP_CURRENT_STATE];
	rc = read_property_id(bcdev, xm_pst, XM_PROP_ADAPTER_SVID);
	if (!rc)
		adapter_svid = xm_pst->prop[XM_PROP_ADAPTER_SVID];
	rc = read_property_id(bcdev, xm_pst, XM_PROP_ADAPTER_ID);
	if (!rc)
		adapter_id = xm_pst->prop[XM_PROP_ADAPTER_ID];
	rc = read_property_id(bcdev, xm_pst, XM_PROP_PD_VERIFED);
	if (!rc)
		pd_verified = xm_pst->prop[XM_PROP_PD_VERIFED];
	rc = read_property_id(bcdev, xm_pst, XM_PROP_PDO2);
	if (!rc)
		pdo2 = xm_pst->prop[XM_PROP_PDO2];
	rc = read_property_id(bcdev, xm_pst, XM_PROP_APDO_MAX);
	if (!rc)
		apdo_max = xm_pst->prop[XM_PROP_APDO_MAX];
	effective_verified = xm_pd_effective_verified(bcdev, pd_verified);

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "compat=%u altmode=%u notifier=%u state=%u last_cmd=%u ack=%u reset=%u\n",
			 bcdev->xm_pd_auth_compat,
			 bcdev->xm_altmode_registered,
			 bcdev->xm_altmode_notifier_registered,
			 bcdev->xm_uvdm_state, bcdev->xm_uvdm_last_cmd,
			 bcdev->xm_uvdm_ack_pending,
			 bcdev->xm_uvdm_reset_count);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "real_rx=%u compat_verified=%u forced=%u rx=%u tx=%u tx_skip=%u tx_fail=%u ack=%u ack_fail=%u renegotiation=%u defer=%u fail=%u\n",
			 bcdev->xm_uvdm_real_rx_seen,
			 bcdev->xm_uvdm_compat_verified,
			 bcdev->xm_pd_auth_forced,
			 bcdev->xm_uvdm_rx_count, bcdev->xm_uvdm_tx_count,
			 bcdev->xm_uvdm_tx_skip_count,
			 bcdev->xm_uvdm_tx_fail_count,
			 bcdev->xm_uvdm_rx_ack_count,
			 bcdev->xm_uvdm_rx_ack_fail_count,
			 bcdev->xm_pd_renegotiation_count,
			 bcdev->xm_pd_renegotiation_defer_count,
			 bcdev->xm_pd_renegotiation_fail_count);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "last_tx=%08x %08x %08x %08x\n",
			 bcdev->xm_uvdm_last_tx[0], bcdev->xm_uvdm_last_tx[1],
			 bcdev->xm_uvdm_last_tx[2], bcdev->xm_uvdm_last_tx[3]);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "auth_row_valid=%u auth_row=%u auth_payload_valid=%u auth_adapter_id=%08x auth_payload=%*phN auth_response=%*phN\n",
			 bcdev->xm_uvdm_auth_row_valid,
			 bcdev->xm_uvdm_auth_row + 1,
			 bcdev->xm_uvdm_auth_payload_valid,
			 bcdev->xm_uvdm_auth_adapter_id,
			 (int)sizeof(bcdev->xm_uvdm_auth_payload),
			 bcdev->xm_uvdm_auth_payload,
			 (int)sizeof(bcdev->xm_uvdm_auth_response),
			 bcdev->xm_uvdm_auth_response);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "last_rx=%*phN\n",
			 (int)sizeof(bcdev->xm_uvdm_last_rx),
			 bcdev->xm_uvdm_last_rx);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "live_usb_real=%u live_state=%u live_svid=%04x live_adapter_id=%08x override_adapter_id=%08x live_pd_verified=%u effective_pd_verified=%u live_pdo2=%08x live_apdo_max=%u\n",
			 usb_real_type, current_state, adapter_svid,
			 adapter_id, bcdev->xm_adapter_id_override,
			 pd_verified, effective_verified, pdo2, apdo_max);

	return len;
}
static CLASS_ATTR_RO(xiaomi_pd_auth_debug);

static ssize_t pdo2_show(struct class *c, struct class_attribute *attr,
			 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_PDO2);
	if (rc < 0)
		return rc;

	if (!pst->prop[XM_PROP_PDO2] && usbpd_is_pd_active(bcdev)) {
		pr_info_ratelimited("pdo2_show: firmware PDO2 is zero while PD active, exposing compat PDO2=%08x\n",
				    XM_PD_COMPAT_PDO2_9V3A);
		return scnprintf(buf, PAGE_SIZE, "%08x\n",
				 XM_PD_COMPAT_PDO2_9V3A);
	}

	pr_info_ratelimited("pdo2_show: raw=%08x\n",
			    pst->prop[XM_PROP_PDO2]);

	return scnprintf(buf, PAGE_SIZE, "%08x\n", pst->prop[XM_PROP_PDO2]);
}
static CLASS_ATTR_RO(pdo2);

static ssize_t fastchg_mode_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FASTCHGMODE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_FASTCHGMODE]);
}
static CLASS_ATTR_RO(fastchg_mode);

static ssize_t apdo_max_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_APDO_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_APDO_MAX]);
}
static CLASS_ATTR_RO(apdo_max);

static ssize_t thermal_remove_store(struct class *c,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_THERMAL_REMOVE, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t thermal_remove_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_THERMAL_REMOVE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_THERMAL_REMOVE]);
}
static CLASS_ATTR_RW(thermal_remove);

static ssize_t fg_rm_show(struct class *c, struct class_attribute *attr,
			  char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG_RM);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FG_RM]);
}
static CLASS_ATTR_RO(fg_rm);

static ssize_t mtbf_current_store(struct class *c, struct class_attribute *attr,
				  const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_MTBF_CURRENT, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t mtbf_current_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_MTBF_CURRENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 pst->prop[XM_PROP_MTBF_CURRENT]);
}
static CLASS_ATTR_RW(mtbf_current);

#if defined(CONFIG_BQ_FUEL_GAUGE)
static ssize_t fake_temp_store(struct class *c, struct class_attribute *attr,
			       const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_FAKE_TEMP, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t fake_temp_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FAKE_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FAKE_TEMP]);
}
static CLASS_ATTR_RW(fake_temp);
#endif

static ssize_t qbg_vbat_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_QBG_VBAT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_QBG_VBAT]);
}
static CLASS_ATTR_RO(qbg_vbat);

static ssize_t vph_pwr_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_QBG_VPH_PWR);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_QBG_VPH_PWR]);
}
static CLASS_ATTR_RO(vph_pwr);

static ssize_t qbg_temp_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_QBG_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_QBG_TEMP]);
}
static CLASS_ATTR_RO(qbg_temp);

static ssize_t typec_mode_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_TYPEC_MODE);
	if (rc < 0 || pst->prop[XM_PROP_TYPEC_MODE] > 9 ||
	    pst->prop[XM_PROP_TYPEC_MODE] < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 power_supply_usbc_text[pst->prop[XM_PROP_TYPEC_MODE]]);
}
static CLASS_ATTR_RO(typec_mode);

#if !defined(CONFIG_VENUS_FOR_BUILD)
static ssize_t fg1_qmax_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_QMAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_QMAX]);
}
static CLASS_ATTR_RO(fg1_qmax);

static ssize_t fg1_rm_show(struct class *c, struct class_attribute *attr,
			   char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_RM);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_RM]);
}
static CLASS_ATTR_RO(fg1_rm);

static ssize_t fg1_fcc_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_FCC);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_FCC]);
}
static CLASS_ATTR_RO(fg1_fcc);

static ssize_t fg1_soh_show(struct class *c, struct class_attribute *attr,
			    char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_SOH);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_SOH]);
}
static CLASS_ATTR_RO(fg1_soh);

static ssize_t fg1_fcc_soh_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_FCC_SOH);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG1_FCC_SOH]);
}
static CLASS_ATTR_RO(fg1_fcc_soh);

static ssize_t fg1_cycle_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_CYCLE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_CYCLE]);
}
static CLASS_ATTR_RO(fg1_cycle);

static ssize_t fg1_fastcharge_show(struct class *c,
				   struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_FAST_CHARGE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG1_FAST_CHARGE]);
}
static CLASS_ATTR_RO(fg1_fastcharge);

static ssize_t fg1_current_max_show(struct class *c,
				    struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_CURRENT_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG1_CURRENT_MAX]);
}
static CLASS_ATTR_RO(fg1_current_max);

static ssize_t fg1_vol_max_show(struct class *c, struct class_attribute *attr,
				char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_VOL_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG1_VOL_MAX]);
}
static CLASS_ATTR_RO(fg1_vol_max);

static ssize_t fg1_tsim_show(struct class *c, struct class_attribute *attr,
			     char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TSIM);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TSIM]);
}
static CLASS_ATTR_RO(fg1_tsim);

static ssize_t fg1_tambient_show(struct class *c, struct class_attribute *attr,
				 char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TAMBIENT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 pst->prop[XM_PROP_FG1_TAMBIENT]);
}
static CLASS_ATTR_RO(fg1_tambient);

static ssize_t fg1_tremq_show(struct class *c, struct class_attribute *attr,
			      char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TREMQ);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TREMQ]);
}
static CLASS_ATTR_RO(fg1_tremq);

static ssize_t fg1_tfullq_show(struct class *c, struct class_attribute *attr,
			       char *buf)
{
	struct battery_chg_dev *bcdev =
		container_of(c, struct battery_chg_dev, battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TFULLQ);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TFULLQ]);
}
static CLASS_ATTR_RO(fg1_tfullq);

#if defined(CONFIG_REDWOOD_FOR_BUILD)
static ssize_t fg1_seal_set_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	pr_err("seal set %d\n", val);

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
				XM_PROP_FG1_SEAL_SET, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t fg1_seal_set_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_SEAL_SET);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_FG1_SEAL_SET]);
}
static CLASS_ATTR_RW(fg1_seal_set);

static ssize_t fg1_seal_state_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_SEAL_STATE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_SEAL_STATE]);
}
static CLASS_ATTR_RO(fg1_seal_state);

static ssize_t fg1_df_check_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_DF_CHECK);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_DF_CHECK]);
}
static CLASS_ATTR_RO(fg1_df_check);

static ssize_t fg1_voltage_max_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_VOLTAGE_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_VOLTAGE_MAX]);
}
static CLASS_ATTR_RO(fg1_voltage_max);

static ssize_t fg1_charge_current_max_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_Charge_Current_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_Charge_Current_MAX]);
}
static CLASS_ATTR_RO(fg1_charge_current_max);

static ssize_t fg1_discharge_current_max_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_Discharge_Current_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_Discharge_Current_MAX]);
}
static CLASS_ATTR_RO(fg1_discharge_current_max);

static ssize_t fg1_temp_max_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TEMP_MAX);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TEMP_MAX]);
}
static CLASS_ATTR_RO(fg1_temp_max);

static ssize_t fg1_temp_min_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TEMP_MIN);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TEMP_MIN]);
}
static CLASS_ATTR_RO(fg1_temp_min);

static ssize_t fg1_time_ht_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TIME_HT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TIME_HT]);
}
static CLASS_ATTR_RO(fg1_time_ht);

static ssize_t fg1_time_ot_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TIME_OT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TIME_OT]);
}
static CLASS_ATTR_RO(fg1_time_ot);

static ssize_t fg1_time_ut_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TIME_UT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TIME_UT]);
}
static CLASS_ATTR_RO(fg1_time_ut);

static ssize_t fg1_time_lt_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_TIME_LT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_TIME_LT]);
}
static CLASS_ATTR_RO(fg1_time_lt);
#endif

#if defined(CONFIG_BQ_CLOUD_AUTHENTICATION)
static ssize_t server_sn_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;
	int test[8] = {0};
	int i = 0;

	for(i = 0; i < 8; i++)
	{
		rc = read_property_id(bcdev, pst, XM_PROP_SERVER_SN);
		if (rc < 0)
			return rc;
		test[i] = pst->prop[XM_PROP_SERVER_SN];
	}
	return scnprintf(buf, PAGE_SIZE, "0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x 0x%0x\n", 
		(test[0]>>24)&0xff, (test[0]>>16)&0xff, (test[0]>>8)&0xff, (test[0]>>0)&0xff,
		(test[1]>>24)&0xff, (test[1]>>16)&0xff, (test[1]>>8)&0xff, (test[1]>>0)&0xff,
		(test[2]>>24)&0xff, (test[2]>>16)&0xff, (test[2]>>8)&0xff, (test[2]>>0)&0xff,
		(test[3]>>24)&0xff, (test[3]>>16)&0xff, (test[3]>>8)&0xff, (test[3]>>0)&0xff,
		(test[4]>>24)&0xff, (test[4]>>16)&0xff, (test[4]>>8)&0xff, (test[4]>>0)&0xff,
		(test[5]>>24)&0xff, (test[5]>>16)&0xff, (test[5]>>8)&0xff, (test[5]>>0)&0xff,
		(test[6]>>24)&0xff, (test[6]>>16)&0xff, (test[6]>>8)&0xff, (test[6]>>0)&0xff,
		(test[7]>>24)&0xff, (test[7]>>16)&0xff, (test[7]>>8)&0xff, (test[7]>>0)&0xff);
}
static CLASS_ATTR_RO(server_sn);

static ssize_t server_result_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	bool val;
	int rc;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, pst, XM_PROP_SERVER_RESULT, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t server_result_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SERVER_RESULT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_SERVER_RESULT]);
}
static CLASS_ATTR_RW(server_result);

static ssize_t adsp_result_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_ADSP_RESULT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_ADSP_RESULT]);
}
static CLASS_ATTR_RO(adsp_result);
#endif

#if defined(CONFIG_REDWOOD_FOR_BUILD)
static ssize_t shipmode_count_reset_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
				XM_PROP_SHIPMODE_COUNT_RESET, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t shipmode_count_reset_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SHIPMODE_COUNT_RESET);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_SHIPMODE_COUNT_RESET]);
}
static CLASS_ATTR_RW(shipmode_count_reset);

static ssize_t sport_mode_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int rc;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
				XM_PROP_SPORT_MODE, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t sport_mode_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_SPORT_MODE);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_SPORT_MODE]);
}
static CLASS_ATTR_RW(sport_mode);

static ssize_t cell1_volt_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_CELL1_VOLT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_CELL1_VOLT]);
}
static CLASS_ATTR_RO(cell1_volt);

static ssize_t cell2_volt_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_CELL2_VOLT);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_CELL2_VOLT]);
}
static CLASS_ATTR_RO(cell2_volt);

static ssize_t fg_vendor_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG_VENDOR_ID);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG_VENDOR_ID]);
}
static CLASS_ATTR_RO(fg_vendor);
#endif

#if defined(CONFIG_AI_RSOC_M20)
static ssize_t fg1_rsoc_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_RSOC);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_RSOC]);
}
static CLASS_ATTR_RO(fg1_rsoc);

static ssize_t fg1_ai_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG1_AI);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_FG1_AI]);
}
static CLASS_ATTR_RO(fg1_ai);
#endif
#endif

static struct attribute *xiaomi_battery_class_attrs[] = {
	&class_attr_wireless_register.attr,
	&class_attr_wireless_input_curr.attr,
	&class_attr_real_type.attr,
	&class_attr_resistance_id.attr,
	&class_attr_verify_digest.attr,
	&class_attr_verify_slave_flag.attr,
	&class_attr_connector_temp.attr,
	&class_attr_authentic.attr,
	&class_attr_chip_ok.attr,
#if defined(CONFIG_DUAL_FUEL_GAUGE)
	&class_attr_slave_chip_ok.attr,
	&class_attr_slave_authentic.attr,
	&class_attr_fg1_vol.attr,
	&class_attr_fg1_soc.attr,
	&class_attr_fg1_temp.attr,
	&class_attr_fg1_ibatt.attr,
	&class_attr_fg2_vol.attr,
	&class_attr_fg2_soc.attr,
	&class_attr_fg2_temp.attr,
	&class_attr_fg2_ibatt.attr,
	&class_attr_fg2_qmax.attr,
	&class_attr_fg2_rm.attr,
	&class_attr_fg2_fcc.attr,
	&class_attr_fg2_soh.attr,
	&class_attr_fg2_fcc_soh.attr,
	&class_attr_fg2_cycle.attr,
	&class_attr_fg2_fastcharge.attr,
	&class_attr_fg2_current_max.attr,
	&class_attr_fg2_vol_max.attr,
	&class_attr_fg2_tsim.attr,
	&class_attr_fg2_tambient.attr,
	&class_attr_fg2_tremq.attr,
	&class_attr_fg2_tfullq.attr,
	&class_attr_is_old_hw.attr,
#endif
	&class_attr_vbus_disable.attr,
	&class_attr_cc_orientation.attr,
	&class_attr_slave_batt_present.attr,
#if defined(CONFIG_BQ2597X)
	&class_attr_bq2597x_chip_ok.attr,
	&class_attr_bq2597x_slave_chip_ok.attr,
	&class_attr_bq2597x_bus_current.attr,
	&class_attr_bq2597x_slave_bus_current.attr,
	&class_attr_bq2597x_bus_delta.attr,
	&class_attr_bq2597x_bus_voltage.attr,
	&class_attr_bq2597x_battery_present.attr,
	&class_attr_bq2597x_slave_battery_present.attr,
	&class_attr_bq2597x_battery_voltage.attr,
	&class_attr_cool_mode.attr,
#endif
#if defined(CONFIG_REDWOOD_FOR_BUILD)
	&class_attr_bq2597x_slave_connector.attr,
#endif
#if !defined(CONFIG_VENUS_FOR_BUILD)
	&class_attr_bt_transfer_start.attr,
#endif
	&class_attr_master_smb1396_online.attr,
	&class_attr_master_smb1396_iin.attr,
	&class_attr_slave_smb1396_online.attr,
	&class_attr_slave_smb1396_iin.attr,
	&class_attr_smb_iin_diff.attr,
	&class_attr_soc_decimal.attr,
	&class_attr_shutdown_delay.attr,
	&class_attr_soc_decimal_rate.attr,
#if defined(CONFIG_MI_WIRELESS)
	/* wireless charge attrs */
	&class_attr_tx_mac.attr,
	&class_attr_rx_cr.attr,
	&class_attr_rx_cep.attr,
	&class_attr_bt_state.attr,
	&class_attr_reverse_chg_mode.attr,
	&class_attr_reverse_chg_state.attr,
	//&class_attr_wls_fw_state.attr,
	&class_attr_wireless_chip_fw.attr,
	&class_attr_wls_bin.attr,
	&class_attr_rx_vout.attr,
	&class_attr_rx_vrect.attr,
	&class_attr_rx_iout.attr,
	&class_attr_tx_adapter.attr,
	&class_attr_op_mode.attr,
	&class_attr_wls_die_temp.attr,
	&class_attr_wls_car_adapter.attr,
#if !defined(CONFIG_VENUS_FOR_BUILD)
	&class_attr_wls_tx_speed.attr,
#endif
#endif
	/*****************************/
	&class_attr_input_suspend.attr,
	&class_attr_verify_process.attr,
	&class_attr_request_vdm_cmd.attr,
	&class_attr_current_state.attr,
	&class_attr_adapter_id.attr,
	&class_attr_adapter_id_override.attr,
	&class_attr_adapter_svid.attr,
	&class_attr_pd_verifed.attr,
	&class_attr_xiaomi_pd_auth_debug.attr,
	&class_attr_pdo2.attr,
	&class_attr_fastchg_mode.attr,
	&class_attr_apdo_max.attr,
	&class_attr_thermal_remove.attr,
	&class_attr_voter_debug.attr,
	&class_attr_fg_rm.attr,
	&class_attr_wlscharge_control_limit.attr,
	&class_attr_mtbf_current.attr,
#if defined(CONFIG_BQ_FUEL_GAUGE)
	&class_attr_fake_temp.attr,
#endif
	&class_attr_qbg_vbat.attr,
	&class_attr_vph_pwr.attr,
	&class_attr_qbg_temp.attr,
	&class_attr_typec_mode.attr,
	&class_attr_night_charging.attr,
#if !defined(CONFIG_VENUS_FOR_BUILD)
	&class_attr_smart_batt.attr,
	&class_attr_fg1_qmax.attr,
	&class_attr_fg1_rm.attr,
	&class_attr_fg1_fcc.attr,
	&class_attr_fg1_soh.attr,
#if defined(CONFIG_AI_RSOC_M20)
	&class_attr_fg1_rsoc.attr,
	&class_attr_fg1_ai.attr,
#endif
	&class_attr_fg1_fcc_soh.attr,
	&class_attr_fg1_cycle.attr,
	&class_attr_fg1_fastcharge.attr,
	&class_attr_fg1_current_max.attr,
	&class_attr_fg1_vol_max.attr,
	&class_attr_fg1_tsim.attr,
	&class_attr_fg1_tambient.attr,
	&class_attr_fg1_tremq.attr,
	&class_attr_fg1_tfullq.attr,
#if defined(CONFIG_REDWOOD_FOR_BUILD)
	&class_attr_fg1_seal_state.attr,
	&class_attr_fg1_seal_set.attr,
	&class_attr_fg1_df_check.attr,
	&class_attr_fg1_voltage_max.attr,
	&class_attr_fg1_charge_current_max.attr,
	&class_attr_fg1_discharge_current_max.attr,
	&class_attr_fg1_temp_max.attr,
	&class_attr_fg1_temp_min.attr,
	&class_attr_fg1_time_ht.attr,
	&class_attr_fg1_time_ot.attr,
	&class_attr_fg1_time_ut.attr,
	&class_attr_fg1_time_lt.attr,
#endif
#if defined(CONFIG_BQ_CLOUD_AUTHENTICATION)
	&class_attr_server_sn.attr,
	&class_attr_server_result.attr,
	&class_attr_adsp_result.attr,
#endif
#if defined(CONFIG_REDWOOD_FOR_BUILD)
	&class_attr_shipmode_count_reset.attr,
	&class_attr_sport_mode.attr,
	&class_attr_cell1_volt.attr,
	&class_attr_cell2_volt.attr,
	&class_attr_fg_vendor.attr,
#endif
#endif
	&class_attr_power_max.attr,
	NULL,
};

const struct attribute_group xiaomi_battery_class_group = {
	.attrs = xiaomi_battery_class_attrs,
};

#define MAX_UEVENT_LENGTH 50
void generate_xm_charge_uvent(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(
		work, struct battery_chg_dev, xm_prop_change_work.work);

	static char uevent_string[][MAX_UEVENT_LENGTH + 1] = {
#if defined(CONFIG_MI_WIRELESS)
		"POWER_SUPPLY_REVERSE_CHG_STATE=\n", //length=31+1
		"POWER_SUPPLY_REVERSE_CHG_MODE=\n", //length=30+1
		"POWER_SUPPLY_TX_MAC=\n", //length=20+16
		"POWER_SUPPLY_RX_CEP=\n", //length=20+16
		"POWER_SUPPLY_RX_CR=\n", //length=19+8
		//"POWER_SUPPLY_WLS_FW_STATE=\n",	//length=26+1
		"POWER_SUPPLY_WLS_CAR_ADAPTER=\n", //length=29+1
#endif
		"POWER_SUPPLY_SOC_DECIMAL=\n", //length=31+8
		"POWER_SUPPLY_SOC_DECIMAL_RATE=\n", //length=31+8
		"POWER_SUPPLY_SHUTDOWN_DELAY=\n", //28+8
		"POWER_SUPPLY_VBUS_DISABLE=\n", //length=26+1
	};
	static char *envp[] = {
		uevent_string[0],
		uevent_string[1],
		uevent_string[2],
		uevent_string[3],
#if defined(CONFIG_MI_WIRELESS)
		uevent_string[4],
		uevent_string[5],
		uevent_string[6],
		uevent_string[7],
		uevent_string[8],
		uevent_string[9],
#endif
		NULL,

	};
	char *prop_buf = NULL;

	prop_buf = (char *)get_zeroed_page(GFP_KERNEL);
	if (!prop_buf)
		return;

#if defined(CONFIG_MI_WIRELESS)
	/*add our prop start*/
	reverse_chg_state_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[0] + 31, prop_buf, MAX_UEVENT_LENGTH - 31);

	reverse_chg_mode_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[1] + 30, prop_buf, MAX_UEVENT_LENGTH - 30);

	tx_mac_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[2] + 20, prop_buf, MAX_UEVENT_LENGTH - 20);

	rx_cep_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[3] + 20, prop_buf, MAX_UEVENT_LENGTH - 20);

	rx_cr_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[4] + 19, prop_buf, MAX_UEVENT_LENGTH - 19);

	wls_car_adapter_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[5] + 29, prop_buf, MAX_UEVENT_LENGTH - 29);

	soc_decimal_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[6] + 25, prop_buf, MAX_UEVENT_LENGTH - 25);

	soc_decimal_rate_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[7] + 30, prop_buf, MAX_UEVENT_LENGTH - 30);

	shutdown_delay_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[8] + 28, prop_buf, MAX_UEVENT_LENGTH - 28);

	vbus_disable_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[9] + 26, prop_buf, MAX_UEVENT_LENGTH - 26);

	dev_err(bcdev->dev,
		"uevent test : %s\n %s\n %s\n %s\n %s\n %s\n %s\n %s\n %s\n %s\n",
		envp[0], envp[1], envp[2], envp[3], envp[4], envp[5], envp[6],
		envp[7], envp[8], envp[9]);
#else
	soc_decimal_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[0] + 25, prop_buf, MAX_UEVENT_LENGTH - 25);

	soc_decimal_rate_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[1] + 30, prop_buf, MAX_UEVENT_LENGTH - 30);

	shutdown_delay_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[2] + 28, prop_buf, MAX_UEVENT_LENGTH - 28);

	vbus_disable_show(&(bcdev->battery_class), NULL, prop_buf);
	strncpy(uevent_string[3] + 26, prop_buf, MAX_UEVENT_LENGTH - 26);

	dev_err(bcdev->dev, "uevent test : %s\n %s\n %s\n %s\n", envp[0],
		envp[1], envp[2], envp[3]);
#endif

	/*add our prop end*/

	kobject_uevent_env(&bcdev->dev->kobj, KOBJ_CHANGE, envp);

	free_page((unsigned long)prop_buf);
	return;
}

#define CHARGING_PERIOD_S 60
#define DISCHARGE_PERIOD_S 300
void xm_charger_debug_info_print_work(struct work_struct *work)
{
	struct battery_chg_dev *bcdev =
		container_of(work, struct battery_chg_dev,
			     charger_debug_info_print_work.work);
	struct power_supply *usb_psy = NULL;
	int rc, usb_present = 0;
	int vbus_vol_uv, ibus_ua;
	int interval = DISCHARGE_PERIOD_S;
	union power_supply_propval val = {
		0,
	};

	usb_psy = bcdev->psy_list[PSY_TYPE_USB].psy;
	if (usb_psy != NULL) {
		rc = usb_psy_get_prop(usb_psy, POWER_SUPPLY_PROP_ONLINE, &val);
		if (!rc)
			usb_present = val.intval;
		else
			usb_present = 0;
		pr_err("usb_present: %d\n", usb_present);
	} else {
		return;
	}

	if (usb_present == 1) {
		rc = usb_psy_get_prop(usb_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
				      &val);
		if (!rc)
			vbus_vol_uv = val.intval;
		else
			vbus_vol_uv = 0;

		rc = usb_psy_get_prop(usb_psy, POWER_SUPPLY_PROP_CURRENT_NOW,
				      &val);
		if (!rc)
			ibus_ua = val.intval;
		else
			ibus_ua = 0;

		pr_err("vbus_vol_uv: %d, ibus_ua: %d\n", vbus_vol_uv, ibus_ua);
		interval = CHARGING_PERIOD_S;
	} else {
		interval = DISCHARGE_PERIOD_S;
	}

	schedule_delayed_work(&bcdev->charger_debug_info_print_work,
			      interval * HZ);
}
