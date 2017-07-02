/*
 * Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt)	"sde-hdmi:[%s] " fmt, __func__

#include <linux/list.h>
#include <linux/of.h>
#include <linux/gpio.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/irqdomain.h>

#include "sde_kms.h"
#include "sde_connector.h"
#include "msm_drv.h"
#include "sde_hdmi.h"

static DEFINE_MUTEX(sde_hdmi_list_lock);
static LIST_HEAD(sde_hdmi_list);

/* HDMI SCDC register offsets */
#define HDMI_SCDC_UPDATE_0              0x10
#define HDMI_SCDC_UPDATE_1              0x11
#define HDMI_SCDC_TMDS_CONFIG           0x20
#define HDMI_SCDC_SCRAMBLER_STATUS      0x21
#define HDMI_SCDC_CONFIG_0              0x30
#define HDMI_SCDC_STATUS_FLAGS_0        0x40
#define HDMI_SCDC_STATUS_FLAGS_1        0x41
#define HDMI_SCDC_ERR_DET_0_L           0x50
#define HDMI_SCDC_ERR_DET_0_H           0x51
#define HDMI_SCDC_ERR_DET_1_L           0x52
#define HDMI_SCDC_ERR_DET_1_H           0x53
#define HDMI_SCDC_ERR_DET_2_L           0x54
#define HDMI_SCDC_ERR_DET_2_H           0x55
#define HDMI_SCDC_ERR_DET_CHECKSUM      0x56

#define HDMI_DISPLAY_MAX_WIDTH          4096
#define HDMI_DISPLAY_MAX_HEIGHT         2160

static const struct of_device_id sde_hdmi_dt_match[] = {
	{.compatible = "qcom,hdmi-display"},
	{}
};

static ssize_t _sde_hdmi_debugfs_dump_info_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char *buf;
	u32 len = 0;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(SZ_1K, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf, SZ_4K, "name = %s\n", display->name);

	if (copy_to_user(buff, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos += len;

	kfree(buf);
	return len;
}

static ssize_t _sde_hdmi_debugfs_edid_modes_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char *buf;
	u32 len = 0;
	struct drm_connector *connector;
	u32 mode_count = 0;
	struct drm_display_mode *mode;

	if (!display)
		return -ENODEV;

	if (!display->ctrl.ctrl ||
		!display->ctrl.ctrl->connector) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				  display);
		return -ENOMEM;
	}

	if (*ppos)
		return 0;

	connector = display->ctrl.ctrl->connector;

	list_for_each_entry(mode, &connector->modes, head) {
		mode_count++;
	}

	/* Adding one more to store title */
	mode_count++;

	buf = kzalloc((mode_count * sizeof(*mode)), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf + len, PAGE_SIZE - len,
					"name refresh (Hz) hdisp hss hse htot vdisp");

	len += snprintf(buf + len, PAGE_SIZE - len,
					" vss vse vtot flags\n");

	list_for_each_entry(mode, &connector->modes, head) {
		len += snprintf(buf + len, SZ_4K - len,
		"%s %d %d %d %d %d %d %d %d %d 0x%x\n",
		mode->name, mode->vrefresh, mode->hdisplay,
		mode->hsync_start, mode->hsync_end, mode->htotal,
		mode->vdisplay, mode->vsync_start, mode->vsync_end,
		mode->vtotal, mode->flags);
	}

	if (copy_to_user(buff, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos += len;

	kfree(buf);
	return len;
}

static ssize_t _sde_hdmi_debugfs_edid_vsdb_info_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char buf[200];
	u32 len = 0;
	struct drm_connector *connector;

	if (!display)
		return -ENODEV;

	if (!display->ctrl.ctrl ||
		!display->ctrl.ctrl->connector) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				  display);
		return -ENOMEM;
	}

	SDE_HDMI_DEBUG("%s +", __func__);
	if (*ppos)
		return 0;

	connector = display->ctrl.ctrl->connector;
	len += snprintf(buf + len, sizeof(buf) - len,
					"max_tmds_clock = %d\n",
					connector->max_tmds_clock);
	len += snprintf(buf + len, sizeof(buf) - len,
					"latency_present %d %d\n",
					connector->latency_present[0],
					connector->latency_present[1]);
	len += snprintf(buf + len, sizeof(buf) - len,
					"video_latency %d %d\n",
					connector->video_latency[0],
					connector->video_latency[1]);
	len += snprintf(buf + len, sizeof(buf) - len,
					"audio_latency %d %d\n",
					connector->audio_latency[0],
					connector->audio_latency[1]);
	len += snprintf(buf + len, sizeof(buf) - len,
					"dvi_dual %d\n",
					(int)connector->dvi_dual);

	if (copy_to_user(buff, buf, len))
		return -EFAULT;

	*ppos += len;
	SDE_HDMI_DEBUG("%s - ", __func__);
	return len;
}

static ssize_t _sde_hdmi_debugfs_edid_hdr_info_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char buf[200];
	u32 len = 0;
	struct drm_connector *connector;

	if (!display)
		return -ENODEV;

	if (!display->ctrl.ctrl ||
		!display->ctrl.ctrl->connector) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				  display);
		return -ENOMEM;
	}

	SDE_HDMI_DEBUG("%s +", __func__);
	if (*ppos)
		return 0;

	connector = display->ctrl.ctrl->connector;
	len += snprintf(buf, sizeof(buf), "hdr_eotf = %d\n"
					"hdr_metadata_type_one %d\n"
					"hdr_max_luminance %d\n"
					"hdr_avg_luminance %d\n"
					"hdr_min_luminance %d\n"
					"hdr_supported %d\n",
					connector->hdr_eotf,
					connector->hdr_metadata_type_one,
					connector->hdr_max_luminance,
					connector->hdr_avg_luminance,
					connector->hdr_min_luminance,
					(int)connector->hdr_supported);

	if (copy_to_user(buff, buf, len))
		return -EFAULT;

	*ppos += len;
	SDE_HDMI_DEBUG("%s - ", __func__);
	return len;
}

static ssize_t _sde_hdmi_debugfs_edid_hfvsdb_info_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char buf[200];
	u32 len = 0;
	struct drm_connector *connector;

	if (!display)
		return -ENODEV;

	if (!display->ctrl.ctrl ||
		!display->ctrl.ctrl->connector) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				  display);
		return -ENOMEM;
	}

	SDE_HDMI_DEBUG("%s +", __func__);
	if (*ppos)
		return 0;

	connector = display->ctrl.ctrl->connector;
	len += snprintf(buf, PAGE_SIZE - len, "max_tmds_char = %d\n"
					"scdc_present %d\n"
					"rr_capable %d\n"
					"supports_scramble %d\n"
					"flags_3d %d\n",
					connector->max_tmds_char,
					(int)connector->scdc_present,
					(int)connector->rr_capable,
					(int)connector->supports_scramble,
					connector->flags_3d);

	if (copy_to_user(buff, buf, len))
		return -EFAULT;

	*ppos += len;
	return len;
}

static ssize_t _sde_hdmi_debugfs_edid_vcdb_info_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char buf[100];
	u32 len = 0;
	struct drm_connector *connector;

	if (!display)
		return -ENODEV;

	if (!display->ctrl.ctrl ||
		!display->ctrl.ctrl->connector) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				  display);
		return -ENOMEM;
	}

	SDE_HDMI_DEBUG("%s +", __func__);
	if (*ppos)
		return 0;

	connector = display->ctrl.ctrl->connector;
	len += snprintf(buf, PAGE_SIZE - len, "pt_scan_info = %d\n"
					"it_scan_info = %d\n"
					"ce_scan_info = %d\n",
					(int)connector->pt_scan_info,
					(int)connector->it_scan_info,
					(int)connector->ce_scan_info);

	if (copy_to_user(buff, buf, len))
		return -EFAULT;

	*ppos += len;
	SDE_HDMI_DEBUG("%s - ", __func__);
	return len;
}

static ssize_t _sde_hdmi_edid_vendor_name_read(struct file *file,
						char __user *buff,
						size_t count,
						loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char buf[100];
	u32 len = 0;
	struct drm_connector *connector;

	if (!display)
		return -ENODEV;

	if (!display->ctrl.ctrl ||
		!display->ctrl.ctrl->connector) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				  display);
		return -ENOMEM;
	}

	SDE_HDMI_DEBUG("%s +", __func__);
	if (*ppos)
		return 0;

	connector = display->ctrl.ctrl->connector;
	len += snprintf(buf, PAGE_SIZE - len, "Vendor ID is %s\n",
					display->edid_ctrl->vendor_id);

	if (copy_to_user(buff, buf, len))
		return -EFAULT;

	*ppos += len;
	SDE_HDMI_DEBUG("%s - ", __func__);
	return len;
}

static const struct file_operations dump_info_fops = {
	.open = simple_open,
	.read = _sde_hdmi_debugfs_dump_info_read,
};

static const struct file_operations edid_modes_fops = {
	.open = simple_open,
	.read = _sde_hdmi_debugfs_edid_modes_read,
};

static const struct file_operations edid_vsdb_info_fops = {
	.open = simple_open,
	.read = _sde_hdmi_debugfs_edid_vsdb_info_read,
};

static const struct file_operations edid_hdr_info_fops = {
	.open = simple_open,
	.read = _sde_hdmi_debugfs_edid_hdr_info_read,
};

static const struct file_operations edid_hfvsdb_info_fops = {
	.open = simple_open,
	.read = _sde_hdmi_debugfs_edid_hfvsdb_info_read,
};

static const struct file_operations edid_vcdb_info_fops = {
	.open = simple_open,
	.read = _sde_hdmi_debugfs_edid_vcdb_info_read,
};

static const struct file_operations edid_vendor_name_fops = {
	.open = simple_open,
	.read = _sde_hdmi_edid_vendor_name_read,
};

static u64 _sde_hdmi_clip_valid_pclk(struct drm_display_mode *mode, u64 pclk_in)
{
	u32 pclk_delta, pclk;
	u64 pclk_clip = pclk_in;

	/* as per standard, 0.5% of deviation is allowed */
	pclk = mode->clock * HDMI_KHZ_TO_HZ;
	pclk_delta = pclk * 5 / 1000;

	if (pclk_in < (pclk - pclk_delta))
		pclk_clip = pclk - pclk_delta;
	else if (pclk_in > (pclk + pclk_delta))
		pclk_clip = pclk + pclk_delta;

	if (pclk_in != pclk_clip)
		pr_warn("clip pclk from %lld to %lld\n", pclk_in, pclk_clip);

	return pclk_clip;
}

/**
 * _sde_hdmi_update_pll_delta() - Update the HDMI pixel clock as per input ppm
 *
 * @ppm: ppm is parts per million multiplied by 1000.
 * return: 0 on success, non-zero in case of failure.
 *
 * The input ppm will be clipped if it's more than or less than 5% of the TMDS
 * clock rate defined by HDMI spec.
 */
static int _sde_hdmi_update_pll_delta(struct sde_hdmi *display, s32 ppm)
{
	struct hdmi *hdmi = display->ctrl.ctrl;
	struct drm_display_mode *current_mode = &display->mode;
	u64 cur_pclk, dst_pclk;
	u64 clip_pclk;
	int rc = 0;

	if (!hdmi->power_on || !display->connected) {
		SDE_ERROR("HDMI display is not ready\n");
		return -EINVAL;
	}

	/* get current pclk */
	cur_pclk = hdmi->pixclock;
	/* get desired pclk */
	dst_pclk = cur_pclk * (1000000000 + ppm);
	do_div(dst_pclk, 1000000000);

	clip_pclk = _sde_hdmi_clip_valid_pclk(current_mode, dst_pclk);

	/* update pclk */
	if (clip_pclk != cur_pclk) {
		SDE_DEBUG("PCLK changes from %llu to %llu when delta is %d\n",
				cur_pclk, clip_pclk, ppm);

		rc = clk_set_rate(hdmi->pwr_clks[0], clip_pclk);
		if (rc < 0) {
			SDE_ERROR("PLL update failed, reset clock rate\n");
			return rc;
		}

		hdmi->pixclock = clip_pclk;
	}

	return rc;
}

static ssize_t _sde_hdmi_debugfs_pll_delta_write(struct file *file,
		    const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct sde_hdmi *display = file->private_data;
	char buf[10];
	int ppm = 0;

	if (!display)
		return -ENODEV;

	if (count >= sizeof(buf))
		return -EFAULT;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = 0;	/* end of string */

	if (kstrtoint(buf, 0, &ppm))
		return -EFAULT;

	if (ppm)
		_sde_hdmi_update_pll_delta(display, ppm);

	return count;
}

static const struct file_operations pll_delta_fops = {
	.open = simple_open,
	.write = _sde_hdmi_debugfs_pll_delta_write,
};

static int _sde_hdmi_debugfs_init(struct sde_hdmi *display)
{
	int rc = 0;
	struct dentry *dir, *dump_file, *edid_modes;
	struct dentry *edid_vsdb_info, *edid_hdr_info, *edid_hfvsdb_info;
	struct dentry *edid_vcdb_info, *edid_vendor_name, *pll_file;

	dir = debugfs_create_dir(display->name, NULL);
	if (!dir) {
		rc = -ENOMEM;
		SDE_ERROR("[%s]debugfs create dir failed, rc = %d\n",
			display->name, rc);
		goto error;
	}

	dump_file = debugfs_create_file("dump_info",
					0444,
					dir,
					display,
					&dump_info_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		SDE_ERROR("[%s]debugfs create dump_info file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	pll_file = debugfs_create_file("pll_delta",
					0644,
					dir,
					display,
					&pll_delta_fops);
	if (IS_ERR_OR_NULL(pll_file)) {
		rc = PTR_ERR(pll_file);
		SDE_ERROR("[%s]debugfs create pll_delta file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	edid_modes = debugfs_create_file("edid_modes",
					0444,
					dir,
					display,
					&edid_modes_fops);

	if (IS_ERR_OR_NULL(edid_modes)) {
		rc = PTR_ERR(edid_modes);
		SDE_ERROR("[%s]debugfs create file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	edid_vsdb_info = debugfs_create_file("edid_vsdb_info",
						0444,
						dir,
						display,
						&edid_vsdb_info_fops);

	if (IS_ERR_OR_NULL(edid_vsdb_info)) {
		rc = PTR_ERR(edid_vsdb_info);
		SDE_ERROR("[%s]debugfs create file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	edid_hdr_info = debugfs_create_file("edid_hdr_info",
						0444,
						dir,
						display,
						&edid_hdr_info_fops);
	if (IS_ERR_OR_NULL(edid_hdr_info)) {
		rc = PTR_ERR(edid_hdr_info);
		SDE_ERROR("[%s]debugfs create file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	edid_hfvsdb_info = debugfs_create_file("edid_hfvsdb_info",
						0444,
						dir,
						display,
						&edid_hfvsdb_info_fops);

	if (IS_ERR_OR_NULL(edid_hfvsdb_info)) {
		rc = PTR_ERR(edid_hfvsdb_info);
		SDE_ERROR("[%s]debugfs create file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	edid_vcdb_info = debugfs_create_file("edid_vcdb_info",
						0444,
						dir,
						display,
						&edid_vcdb_info_fops);

	if (IS_ERR_OR_NULL(edid_vcdb_info)) {
		rc = PTR_ERR(edid_vcdb_info);
		SDE_ERROR("[%s]debugfs create file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	edid_vendor_name = debugfs_create_file("edid_vendor_name",
						0444,
						dir,
						display,
						&edid_vendor_name_fops);

	if (IS_ERR_OR_NULL(edid_vendor_name)) {
		rc = PTR_ERR(edid_vendor_name);
		SDE_ERROR("[%s]debugfs create file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	display->root = dir;
	return rc;
error_remove_dir:
	debugfs_remove(dir);
error:
	return rc;
}

static void _sde_hdmi_debugfs_deinit(struct sde_hdmi *display)
{
	debugfs_remove(display->root);
}

static void _sde_hdmi_phy_reset(struct hdmi *hdmi)
{
	unsigned int val;

	val = hdmi_read(hdmi, REG_HDMI_PHY_CTRL);

	if (val & HDMI_PHY_CTRL_SW_RESET_LOW)
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val & ~HDMI_PHY_CTRL_SW_RESET);
	 else
		 hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val | HDMI_PHY_CTRL_SW_RESET);

	if (val & HDMI_PHY_CTRL_SW_RESET_PLL_LOW)
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val & ~HDMI_PHY_CTRL_SW_RESET_PLL);
	else
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val | HDMI_PHY_CTRL_SW_RESET_PLL);

	if (val & HDMI_PHY_CTRL_SW_RESET_LOW)
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val | HDMI_PHY_CTRL_SW_RESET);
	 else
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val & ~HDMI_PHY_CTRL_SW_RESET);

	if (val & HDMI_PHY_CTRL_SW_RESET_PLL_LOW)
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val | HDMI_PHY_CTRL_SW_RESET_PLL);
	else
		hdmi_write(hdmi, REG_HDMI_PHY_CTRL,
				val & ~HDMI_PHY_CTRL_SW_RESET_PLL);
}

static int _sde_hdmi_gpio_config(struct hdmi *hdmi, bool on)
{
	const struct hdmi_platform_config *config = hdmi->config;
	int ret;

	if (on) {
		if (config->ddc_clk_gpio != -1) {
			ret = gpio_request(config->ddc_clk_gpio,
				"HDMI_DDC_CLK");
			if (ret) {
				SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
					"HDMI_DDC_CLK", config->ddc_clk_gpio,
					ret);
				goto error_ddc_clk_gpio;
			}
			gpio_set_value_cansleep(config->ddc_clk_gpio, 1);
		}

		if (config->ddc_data_gpio != -1) {
			ret = gpio_request(config->ddc_data_gpio,
				"HDMI_DDC_DATA");
			if (ret) {
				SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
					"HDMI_DDC_DATA", config->ddc_data_gpio,
					ret);
				goto error_ddc_data_gpio;
			}
			gpio_set_value_cansleep(config->ddc_data_gpio, 1);
		}

		ret = gpio_request(config->hpd_gpio, "HDMI_HPD");
		if (ret) {
			SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
				"HDMI_HPD", config->hpd_gpio, ret);
			goto error_hpd_gpio;
		}
		gpio_direction_output(config->hpd_gpio, 1);
		if (config->hpd5v_gpio != -1) {
			ret = gpio_request(config->hpd5v_gpio, "HDMI_HPD_5V");
			if (ret) {
				SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
						  "HDMI_HPD_5V",
						  config->hpd5v_gpio,
						  ret);
				goto error_hpd5v_gpio;
			}
			gpio_set_value_cansleep(config->hpd5v_gpio, 1);
		}

		if (config->mux_en_gpio != -1) {
			ret = gpio_request(config->mux_en_gpio, "HDMI_MUX_EN");
			if (ret) {
				SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
					"HDMI_MUX_EN", config->mux_en_gpio,
					ret);
				goto error_en_gpio;
			}
			gpio_set_value_cansleep(config->mux_en_gpio, 1);
		}

		if (config->mux_sel_gpio != -1) {
			ret = gpio_request(config->mux_sel_gpio,
				"HDMI_MUX_SEL");
			if (ret) {
				SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
					"HDMI_MUX_SEL", config->mux_sel_gpio,
					ret);
				goto error_sel_gpio;
			}
			gpio_set_value_cansleep(config->mux_sel_gpio, 0);
		}

		if (config->mux_lpm_gpio != -1) {
			ret = gpio_request(config->mux_lpm_gpio,
					"HDMI_MUX_LPM");
			if (ret) {
				SDE_ERROR("'%s'(%d) gpio_request failed: %d\n",
					"HDMI_MUX_LPM",
					config->mux_lpm_gpio, ret);
				goto error_lpm_gpio;
			}
			gpio_set_value_cansleep(config->mux_lpm_gpio, 1);
		}
		SDE_DEBUG("gpio on");
	} else {
		if (config->ddc_clk_gpio != -1)
			gpio_free(config->ddc_clk_gpio);

		if (config->ddc_data_gpio != -1)
			gpio_free(config->ddc_data_gpio);

		gpio_free(config->hpd_gpio);

		if (config->mux_en_gpio != -1) {
			gpio_set_value_cansleep(config->mux_en_gpio, 0);
			gpio_free(config->mux_en_gpio);
		}

		if (config->mux_sel_gpio != -1) {
			gpio_set_value_cansleep(config->mux_sel_gpio, 1);
			gpio_free(config->mux_sel_gpio);
		}

		if (config->mux_lpm_gpio != -1) {
			gpio_set_value_cansleep(config->mux_lpm_gpio, 0);
			gpio_free(config->mux_lpm_gpio);
		}
		SDE_DEBUG("gpio off");
	}

	return 0;

error_lpm_gpio:
	if (config->mux_sel_gpio != -1)
		gpio_free(config->mux_sel_gpio);
error_sel_gpio:
	if (config->mux_en_gpio != -1)
		gpio_free(config->mux_en_gpio);
error_en_gpio:
	gpio_free(config->hpd5v_gpio);
error_hpd5v_gpio:
	gpio_free(config->hpd_gpio);
error_hpd_gpio:
	if (config->ddc_data_gpio != -1)
		gpio_free(config->ddc_data_gpio);
error_ddc_data_gpio:
	if (config->ddc_clk_gpio != -1)
		gpio_free(config->ddc_clk_gpio);
error_ddc_clk_gpio:
	return ret;
}

static int _sde_hdmi_hpd_enable(struct sde_hdmi *sde_hdmi)
{
	struct hdmi *hdmi = sde_hdmi->ctrl.ctrl;
	const struct hdmi_platform_config *config = hdmi->config;
	struct device *dev = &hdmi->pdev->dev;
	uint32_t hpd_ctrl;
	int i, ret;
	unsigned long flags;
	struct drm_connector *connector;
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;

	connector = hdmi->connector;
	priv = connector->dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	for (i = 0; i < config->hpd_reg_cnt; i++) {
		ret = regulator_enable(hdmi->hpd_regs[i]);
		if (ret) {
			SDE_ERROR("failed to enable hpd regulator: %s (%d)\n",
					config->hpd_reg_names[i], ret);
			goto fail;
		}
	}

	ret = pinctrl_pm_select_default_state(dev);
	if (ret) {
		SDE_ERROR("pinctrl state chg failed: %d\n", ret);
		goto fail;
	}

	ret = _sde_hdmi_gpio_config(hdmi, true);
	if (ret) {
		SDE_ERROR("failed to configure GPIOs: %d\n", ret);
		goto fail;
	}

	for (i = 0; i < config->hpd_clk_cnt; i++) {
		if (config->hpd_freq && config->hpd_freq[i]) {
			ret = clk_set_rate(hdmi->hpd_clks[i],
					config->hpd_freq[i]);
			if (ret)
				pr_warn("failed to set clk %s (%d)\n",
						config->hpd_clk_names[i], ret);
		}

		ret = clk_prepare_enable(hdmi->hpd_clks[i]);
		if (ret) {
			SDE_ERROR("failed to enable hpd clk: %s (%d)\n",
					config->hpd_clk_names[i], ret);
			goto fail;
		}
	}

	if (!sde_kms->splash_info.handoff) {
		sde_hdmi_set_mode(hdmi, false);
		_sde_hdmi_phy_reset(hdmi);
		sde_hdmi_set_mode(hdmi, true);
	}

	hdmi_write(hdmi, REG_HDMI_USEC_REFTIMER, 0x0001001b);

	/* set timeout to 4.1ms (max) for hardware debounce */
	spin_lock_irqsave(&hdmi->reg_lock, flags);
	hpd_ctrl = hdmi_read(hdmi, REG_HDMI_HPD_CTRL);
	hpd_ctrl |= HDMI_HPD_CTRL_TIMEOUT(0x1fff);

	hdmi_write(hdmi, REG_HDMI_HPD_CTRL,
			HDMI_HPD_CTRL_ENABLE | hpd_ctrl);

	/* enable HPD events: */
	hdmi_write(hdmi, REG_HDMI_HPD_INT_CTRL,
			HDMI_HPD_INT_CTRL_INT_CONNECT |
			HDMI_HPD_INT_CTRL_INT_EN);

	/* Toggle HPD circuit to trigger HPD sense */
	hdmi_write(hdmi, REG_HDMI_HPD_CTRL,
			~HDMI_HPD_CTRL_ENABLE & hpd_ctrl);
	hdmi_write(hdmi, REG_HDMI_HPD_CTRL,
			HDMI_HPD_CTRL_ENABLE | hpd_ctrl);
	spin_unlock_irqrestore(&hdmi->reg_lock, flags);

	return 0;

fail:
	return ret;
}

static void _sde_hdmi_hdp_disable(struct sde_hdmi *sde_hdmi)
{
	struct hdmi *hdmi = sde_hdmi->ctrl.ctrl;
	const struct hdmi_platform_config *config = hdmi->config;
	struct device *dev = &hdmi->pdev->dev;
	int i, ret = 0;

	/* Disable HPD interrupt */
	hdmi_write(hdmi, REG_HDMI_HPD_INT_CTRL, 0);

	sde_hdmi_set_mode(hdmi, false);

	for (i = 0; i < config->hpd_clk_cnt; i++)
		clk_disable_unprepare(hdmi->hpd_clks[i]);

	ret = _sde_hdmi_gpio_config(hdmi, false);
	if (ret)
		pr_warn("failed to unconfigure GPIOs: %d\n", ret);

	ret = pinctrl_pm_select_sleep_state(dev);
	if (ret)
		pr_warn("pinctrl state chg failed: %d\n", ret);

	for (i = 0; i < config->hpd_reg_cnt; i++) {
		ret = regulator_disable(hdmi->hpd_regs[i]);
		if (ret)
			pr_warn("failed to disable hpd regulator: %s (%d)\n",
					config->hpd_reg_names[i], ret);
	}
}

static void _sde_hdmi_cec_update_phys_addr(struct sde_hdmi *display)
{
	struct edid *edid = display->edid_ctrl->edid;

	if (edid)
		cec_notifier_set_phys_addr_from_edid(display->notifier, edid);
	else
		cec_notifier_set_phys_addr(display->notifier,
			CEC_PHYS_ADDR_INVALID);
}

static void _sde_hdmi_hotplug_work(struct work_struct *work)
{
	struct sde_hdmi *sde_hdmi =
		container_of(work, struct sde_hdmi, hpd_work);
	struct drm_connector *connector;
	struct hdmi *hdmi = NULL;
	u32 hdmi_ctrl;

	if (!sde_hdmi || !sde_hdmi->ctrl.ctrl ||
		!sde_hdmi->ctrl.ctrl->connector ||
		!sde_hdmi->edid_ctrl) {
		SDE_ERROR("sde_hdmi=%p or hdmi or connector is NULL\n",
				sde_hdmi);
		return;
	}
	hdmi = sde_hdmi->ctrl.ctrl;
	connector = sde_hdmi->ctrl.ctrl->connector;

	if (sde_hdmi->connected) {
		hdmi_ctrl = hdmi_read(hdmi, REG_HDMI_CTRL);
		hdmi_write(hdmi, REG_HDMI_CTRL, hdmi_ctrl | HDMI_CTRL_ENABLE);
		sde_get_edid(connector, hdmi->i2c,
		(void **)&sde_hdmi->edid_ctrl);
		hdmi_write(hdmi, REG_HDMI_CTRL, hdmi_ctrl);
		hdmi->hdmi_mode = sde_detect_hdmi_monitor(sde_hdmi->edid_ctrl);
	} else
		sde_free_edid((void **)&sde_hdmi->edid_ctrl);

	drm_helper_hpd_irq_event(connector->dev);
	_sde_hdmi_cec_update_phys_addr(sde_hdmi);
}

static void _sde_hdmi_connector_irq(struct sde_hdmi *sde_hdmi)
{
	struct hdmi *hdmi = sde_hdmi->ctrl.ctrl;
	uint32_t hpd_int_status, hpd_int_ctrl;

	/* Process HPD: */
	hpd_int_status = hdmi_read(hdmi, REG_HDMI_HPD_INT_STATUS);
	hpd_int_ctrl   = hdmi_read(hdmi, REG_HDMI_HPD_INT_CTRL);

	if ((hpd_int_ctrl & HDMI_HPD_INT_CTRL_INT_EN) &&
			(hpd_int_status & HDMI_HPD_INT_STATUS_INT)) {
		sde_hdmi->connected = !!(hpd_int_status &
					HDMI_HPD_INT_STATUS_CABLE_DETECTED);
		/* ack & disable (temporarily) HPD events: */
		hdmi_write(hdmi, REG_HDMI_HPD_INT_CTRL,
			HDMI_HPD_INT_CTRL_INT_ACK);

		SDE_HDMI_DEBUG("status=%04x, ctrl=%04x", hpd_int_status,
				hpd_int_ctrl);

		/* detect disconnect if we are connected or visa versa: */
		hpd_int_ctrl = HDMI_HPD_INT_CTRL_INT_EN;
		if (!sde_hdmi->connected)
			hpd_int_ctrl |= HDMI_HPD_INT_CTRL_INT_CONNECT;
		hdmi_write(hdmi, REG_HDMI_HPD_INT_CTRL, hpd_int_ctrl);

		queue_work(hdmi->workq, &sde_hdmi->hpd_work);
	}
}

static void _sde_hdmi_cec_irq(struct sde_hdmi *sde_hdmi)
{
	struct hdmi *hdmi = sde_hdmi->ctrl.ctrl;
	u32 cec_intr = hdmi_read(hdmi, REG_HDMI_CEC_INT);

	/* Routing interrupt to external CEC drivers */
	if (cec_intr)
		generic_handle_irq(irq_find_mapping(
				sde_hdmi->irq_domain, 1));
}


static irqreturn_t _sde_hdmi_irq(int irq, void *dev_id)
{
	struct sde_hdmi *sde_hdmi = dev_id;
	struct hdmi *hdmi;

	if (!sde_hdmi || !sde_hdmi->ctrl.ctrl) {
		SDE_ERROR("sde_hdmi=%p or hdmi is NULL\n", sde_hdmi);
		return IRQ_NONE;
	}
	hdmi = sde_hdmi->ctrl.ctrl;
	/* Process HPD: */
	_sde_hdmi_connector_irq(sde_hdmi);

	/* Process DDC: */
	hdmi_i2c_irq(hdmi->i2c);

	/* Process HDCP: */
	if (hdmi->hdcp_ctrl && hdmi->is_hdcp_supported)
		hdmi_hdcp_ctrl_irq(hdmi->hdcp_ctrl);

	/* Process CEC: */
	_sde_hdmi_cec_irq(sde_hdmi);

	return IRQ_HANDLED;
}

static int _sde_hdmi_audio_info_setup(struct platform_device *pdev,
	struct msm_ext_disp_audio_setup_params *params)
{
	int rc = -EPERM;
	struct sde_hdmi *display = NULL;
	struct hdmi *hdmi = NULL;

	display = platform_get_drvdata(pdev);

	if (!display || !params) {
		SDE_ERROR("invalid param(s), display %pK, params %pK\n",
				display, params);
		return -ENODEV;
	}

	hdmi = display->ctrl.ctrl;

	if (hdmi->hdmi_mode)
		rc = sde_hdmi_audio_on(hdmi, params);

	return rc;
}

static int _sde_hdmi_get_audio_edid_blk(struct platform_device *pdev,
	struct msm_ext_disp_audio_edid_blk *blk)
{
	struct sde_hdmi *display = platform_get_drvdata(pdev);

	if (!display || !blk) {
		SDE_ERROR("invalid param(s), display %pK, blk %pK\n",
			display, blk);
		return -ENODEV;
	}

	blk->audio_data_blk = display->edid_ctrl->audio_data_block;
	blk->audio_data_blk_size = display->edid_ctrl->adb_size;

	blk->spk_alloc_data_blk = display->edid_ctrl->spkr_alloc_data_block;
	blk->spk_alloc_data_blk_size = display->edid_ctrl->sadb_size;

	return 0;
}

static int _sde_hdmi_get_cable_status(struct platform_device *pdev, u32 vote)
{
	struct sde_hdmi *display = NULL;
	struct hdmi *hdmi = NULL;

	display = platform_get_drvdata(pdev);

	if (!display) {
		SDE_ERROR("invalid param(s), display %pK\n", display);
		return -ENODEV;
	}

	hdmi = display->ctrl.ctrl;

	return hdmi->power_on && display->connected;
}

static void _sde_hdmi_audio_codec_ready(struct platform_device *pdev)
{
	struct sde_hdmi *display = platform_get_drvdata(pdev);

	if (!display) {
		SDE_ERROR("invalid param(s), display %pK\n", display);
		return;
	}

	mutex_lock(&display->display_lock);
	if (!display->codec_ready) {
		display->codec_ready = true;

		if (display->client_notify_pending)
			sde_hdmi_notify_clients(display, display->connected);
	}
	mutex_unlock(&display->display_lock);
}

static int _sde_hdmi_ext_disp_init(struct sde_hdmi *display)
{
	int rc = 0;
	struct device_node *pd_np;
	const char *phandle = "qcom,msm_ext_disp";

	if (!display) {
		SDE_ERROR("[%s]Invalid params\n", display->name);
		return -EINVAL;
	}

	display->ext_audio_data.type = EXT_DISPLAY_TYPE_HDMI;
	display->ext_audio_data.pdev = display->pdev;
	display->ext_audio_data.codec_ops.audio_info_setup =
		_sde_hdmi_audio_info_setup;
	display->ext_audio_data.codec_ops.get_audio_edid_blk =
		_sde_hdmi_get_audio_edid_blk;
	display->ext_audio_data.codec_ops.cable_status =
		_sde_hdmi_get_cable_status;
	display->ext_audio_data.codec_ops.codec_ready =
		_sde_hdmi_audio_codec_ready;

	if (!display->pdev->dev.of_node) {
		SDE_ERROR("[%s]cannot find sde_hdmi of_node\n", display->name);
		return -ENODEV;
	}

	pd_np = of_parse_phandle(display->pdev->dev.of_node, phandle, 0);
	if (!pd_np) {
		SDE_ERROR("[%s]cannot find %s device node\n",
			display->name, phandle);
		return -ENODEV;
	}

	display->ext_pdev = of_find_device_by_node(pd_np);
	if (!display->ext_pdev) {
		SDE_ERROR("[%s]cannot find %s platform device\n",
			display->name, phandle);
		return -ENODEV;
	}

	rc = msm_ext_disp_register_intf(display->ext_pdev,
			&display->ext_audio_data);
	if (rc)
		SDE_ERROR("[%s]failed to register disp\n", display->name);

	return rc;
}

void sde_hdmi_notify_clients(struct sde_hdmi *display, bool connected)
{
	int state = connected ?
		EXT_DISPLAY_CABLE_CONNECT : EXT_DISPLAY_CABLE_DISCONNECT;

	if (display && display->ext_audio_data.intf_ops.hpd) {
		struct hdmi *hdmi = display->ctrl.ctrl;
		u32 flags = MSM_EXT_DISP_HPD_ASYNC_VIDEO;

		if (hdmi->hdmi_mode)
			flags |= MSM_EXT_DISP_HPD_AUDIO;

		display->ext_audio_data.intf_ops.hpd(display->ext_pdev,
				display->ext_audio_data.type, state, flags);
	}
}

void sde_hdmi_set_mode(struct hdmi *hdmi, bool power_on)
{
	uint32_t ctrl = 0;
	unsigned long flags;

	spin_lock_irqsave(&hdmi->reg_lock, flags);
	ctrl = hdmi_read(hdmi, REG_HDMI_CTRL);
	if (power_on) {
		ctrl |= HDMI_CTRL_ENABLE;
		if (!hdmi->hdmi_mode) {
			ctrl |= HDMI_CTRL_HDMI;
			hdmi_write(hdmi, REG_HDMI_CTRL, ctrl);
			ctrl &= ~HDMI_CTRL_HDMI;
		} else {
			ctrl |= HDMI_CTRL_HDMI;
		}
	} else {
		ctrl &= ~HDMI_CTRL_HDMI;
	}

	hdmi_write(hdmi, REG_HDMI_CTRL, ctrl);
	spin_unlock_irqrestore(&hdmi->reg_lock, flags);
	SDE_HDMI_DEBUG("HDMI Core: %s, HDMI_CTRL=0x%08x\n",
			power_on ? "Enable" : "Disable", ctrl);
}

int sde_hdmi_ddc_read(struct hdmi *hdmi, u16 addr, u8 offset,
					  u8 *data, u16 data_len)
{
	int rc;
	int retry = 5;
	struct i2c_msg msgs[] = {
		{
			.addr   = addr >> 1,
			.flags  = 0,
			.len    = 1,
			.buf    = &offset,
		}, {
			.addr   = addr >> 1,
			.flags  = I2C_M_RD,
			.len    = data_len,
			.buf    = data,
		}
	};

	SDE_HDMI_DEBUG("Start DDC read");
 retry:
	rc = i2c_transfer(hdmi->i2c, msgs, 2);

	retry--;
	if (rc == 2)
		rc = 0;
	else if (retry > 0)
		goto retry;
	else
		rc = -EIO;

	SDE_HDMI_DEBUG("End DDC read %d", rc);

	return rc;
}

#define DDC_WRITE_MAX_BYTE_NUM 32

int sde_hdmi_ddc_write(struct hdmi *hdmi, u16 addr, u8 offset,
					   u8 *data, u16 data_len)
{
	int rc;
	int retry = 10;
	u8 buf[DDC_WRITE_MAX_BYTE_NUM];
	struct i2c_msg msgs[] = {
		{
			.addr   = addr >> 1,
			.flags  = 0,
			.len    = 1,
		}
	};

	SDE_HDMI_DEBUG("Start DDC write");
	if (data_len > (DDC_WRITE_MAX_BYTE_NUM - 1)) {
		SDE_ERROR("%s: write size too big\n", __func__);
		return -ERANGE;
	}

	buf[0] = offset;
	memcpy(&buf[1], data, data_len);
	msgs[0].buf = buf;
	msgs[0].len = data_len + 1;
 retry:
	rc = i2c_transfer(hdmi->i2c, msgs, 1);

	retry--;
	if (rc == 1)
		rc = 0;
	else if (retry > 0)
		goto retry;
	else
		rc = -EIO;

	SDE_HDMI_DEBUG("End DDC write %d", rc);

	return rc;
}

int sde_hdmi_scdc_read(struct hdmi *hdmi, u32 data_type, u32 *val)
{
	int rc = 0;
	u8 data_buf[2] = {0};
	u16 dev_addr, data_len;
	u8 offset;

	if (!hdmi || !hdmi->i2c || !val) {
		SDE_ERROR("Bad Parameters\n");
		return -EINVAL;
	}

	if (data_type >= HDMI_TX_SCDC_MAX) {
		SDE_ERROR("Unsupported data type\n");
		return -EINVAL;
	}

	dev_addr = 0xA8;

	switch (data_type) {
	case HDMI_TX_SCDC_SCRAMBLING_STATUS:
		data_len = 1;
		offset = HDMI_SCDC_SCRAMBLER_STATUS;
		break;
	case HDMI_TX_SCDC_SCRAMBLING_ENABLE:
	case HDMI_TX_SCDC_TMDS_BIT_CLOCK_RATIO_UPDATE:
		data_len = 1;
		offset = HDMI_SCDC_TMDS_CONFIG;
		break;
	case HDMI_TX_SCDC_CLOCK_DET_STATUS:
	case HDMI_TX_SCDC_CH0_LOCK_STATUS:
	case HDMI_TX_SCDC_CH1_LOCK_STATUS:
	case HDMI_TX_SCDC_CH2_LOCK_STATUS:
		data_len = 1;
		offset = HDMI_SCDC_STATUS_FLAGS_0;
		break;
	case HDMI_TX_SCDC_CH0_ERROR_COUNT:
		data_len = 2;
		offset = HDMI_SCDC_ERR_DET_0_L;
		break;
	case HDMI_TX_SCDC_CH1_ERROR_COUNT:
		data_len = 2;
		offset = HDMI_SCDC_ERR_DET_1_L;
		break;
	case HDMI_TX_SCDC_CH2_ERROR_COUNT:
		data_len = 2;
		offset = HDMI_SCDC_ERR_DET_2_L;
		break;
	case HDMI_TX_SCDC_READ_ENABLE:
		data_len = 1;
		offset = HDMI_SCDC_CONFIG_0;
		break;
	default:
		break;
	}

	rc = sde_hdmi_ddc_read(hdmi, dev_addr, offset, data_buf, data_len);
	if (rc) {
		SDE_ERROR("DDC Read failed for %d\n", data_type);
		return rc;
	}

	switch (data_type) {
	case HDMI_TX_SCDC_SCRAMBLING_STATUS:
		*val = (data_buf[0] & BIT(0)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_SCRAMBLING_ENABLE:
		*val = (data_buf[0] & BIT(0)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_TMDS_BIT_CLOCK_RATIO_UPDATE:
		*val = (data_buf[0] & BIT(1)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_CLOCK_DET_STATUS:
		*val = (data_buf[0] & BIT(0)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_CH0_LOCK_STATUS:
		*val = (data_buf[0] & BIT(1)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_CH1_LOCK_STATUS:
		*val = (data_buf[0] & BIT(2)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_CH2_LOCK_STATUS:
		*val = (data_buf[0] & BIT(3)) ? 1 : 0;
		break;
	case HDMI_TX_SCDC_CH0_ERROR_COUNT:
	case HDMI_TX_SCDC_CH1_ERROR_COUNT:
	case HDMI_TX_SCDC_CH2_ERROR_COUNT:
		if (data_buf[1] & BIT(7))
			*val = (data_buf[0] | ((data_buf[1] & 0x7F) << 8));
		else
			*val = 0;
		break;
	case HDMI_TX_SCDC_READ_ENABLE:
		*val = (data_buf[0] & BIT(0)) ? 1 : 0;
		break;
	default:
		break;
	}

	return 0;
}

int sde_hdmi_scdc_write(struct hdmi *hdmi, u32 data_type, u32 val)
{
	int rc = 0;
	u8 data_buf[2] = {0};
	u8 read_val = 0;
	u16 dev_addr, data_len;
	u8 offset;

	if (!hdmi || !hdmi->i2c) {
		SDE_ERROR("Bad Parameters\n");
		return -EINVAL;
	}

	if (data_type >= HDMI_TX_SCDC_MAX) {
		SDE_ERROR("Unsupported data type\n");
		return -EINVAL;
	}

	dev_addr = 0xA8;

	switch (data_type) {
	case HDMI_TX_SCDC_SCRAMBLING_ENABLE:
	case HDMI_TX_SCDC_TMDS_BIT_CLOCK_RATIO_UPDATE:
		dev_addr = 0xA8;
		data_len = 1;
		offset = HDMI_SCDC_TMDS_CONFIG;
		rc = sde_hdmi_ddc_read(hdmi, dev_addr, offset, &read_val,
							   data_len);
		if (rc) {
			SDE_ERROR("scdc read failed\n");
			return rc;
		}
		if (data_type == HDMI_TX_SCDC_SCRAMBLING_ENABLE) {
			data_buf[0] = ((((u8)(read_val & 0xFF)) & (~BIT(0))) |
						   ((u8)(val & BIT(0))));
		} else {
			data_buf[0] = ((((u8)(read_val & 0xFF)) & (~BIT(1))) |
						   (((u8)(val & BIT(0))) << 1));
		}
		break;
	case HDMI_TX_SCDC_READ_ENABLE:
		data_len = 1;
		offset = HDMI_SCDC_CONFIG_0;
		data_buf[0] = (u8)(val & 0x1);
		break;
	default:
		SDE_ERROR("Cannot write to read only reg (%d)\n",
				  data_type);
		return -EINVAL;
	}

	rc = sde_hdmi_ddc_write(hdmi, dev_addr, offset, data_buf, data_len);
	if (rc) {
		SDE_ERROR("DDC Read failed for %d\n", data_type);
		return rc;
	}
	return 0;
}

int sde_hdmi_get_info(struct msm_display_info *info,
				void *display)
{
	int rc = 0;
	struct sde_hdmi *hdmi_display = (struct sde_hdmi *)display;
	struct hdmi *hdmi = hdmi_display->ctrl.ctrl;

	if (!display || !info) {
		SDE_ERROR("display=%p or info=%p is NULL\n", display, info);
		return -EINVAL;
	}

	mutex_lock(&hdmi_display->display_lock);

	info->intf_type = DRM_MODE_CONNECTOR_HDMIA;
	info->num_of_h_tiles = 1;
	info->h_tile_instance[0] = 0;
	if (hdmi_display->non_pluggable) {
		info->capabilities = MSM_DISPLAY_CAP_VID_MODE;
		hdmi_display->connected = true;
		hdmi->hdmi_mode = true;
	} else {
		info->capabilities = MSM_DISPLAY_CAP_HOT_PLUG |
				MSM_DISPLAY_CAP_EDID | MSM_DISPLAY_CAP_VID_MODE;
	}
	info->is_connected = hdmi_display->connected;
	info->max_width = HDMI_DISPLAY_MAX_WIDTH;
	info->max_height = HDMI_DISPLAY_MAX_HEIGHT;
	info->compression = MSM_DISPLAY_COMPRESS_NONE;

	mutex_unlock(&hdmi_display->display_lock);
	return rc;
}

int sde_hdmi_set_property(struct drm_connector *connector,
			struct drm_connector_state *state,
			int property_index,
			uint64_t value,
			void *display)
{
	int rc = 0;

	if (!connector || !display) {
		SDE_ERROR("connector=%pK or display=%pK is NULL\n",
			connector, display);
		return 0;
	}

	SDE_DEBUG("\n");

	if (property_index == CONNECTOR_PROP_PLL_DELTA)
		rc = _sde_hdmi_update_pll_delta(display, value);

	return rc;
}

u32 sde_hdmi_get_num_of_displays(void)
{
	u32 count = 0;
	struct sde_hdmi *display;

	mutex_lock(&sde_hdmi_list_lock);

	list_for_each_entry(display, &sde_hdmi_list, list)
		count++;

	mutex_unlock(&sde_hdmi_list_lock);
	return count;
}

int sde_hdmi_get_displays(void **display_array, u32 max_display_count)
{
	struct sde_hdmi *display;
	int i = 0;

	SDE_DEBUG("\n");

	if (!display_array || !max_display_count) {
		if (!display_array)
			SDE_ERROR("invalid param\n");
		return 0;
	}

	mutex_lock(&sde_hdmi_list_lock);
	list_for_each_entry(display, &sde_hdmi_list, list) {
		if (i >= max_display_count)
			break;
		display_array[i++] = display;
	}
	mutex_unlock(&sde_hdmi_list_lock);

	return i;
}

int sde_hdmi_connector_pre_deinit(struct drm_connector *connector,
		void *display)
{
	struct sde_hdmi *sde_hdmi = (struct sde_hdmi *)display;

	if (!sde_hdmi || !sde_hdmi->ctrl.ctrl) {
		SDE_ERROR("sde_hdmi=%p or hdmi is NULL\n", sde_hdmi);
		return -EINVAL;
	}

	_sde_hdmi_hdp_disable(sde_hdmi);

	return 0;
}

int sde_hdmi_connector_post_init(struct drm_connector *connector,
		void *info,
		void *display)
{
	int rc = 0;
	struct sde_hdmi *sde_hdmi = (struct sde_hdmi *)display;
	struct hdmi *hdmi;

	if (!sde_hdmi) {
		SDE_ERROR("sde_hdmi is NULL\n");
		return -EINVAL;
	}

	hdmi = sde_hdmi->ctrl.ctrl;
	if (!hdmi) {
		SDE_ERROR("hdmi is NULL\n");
		return -EINVAL;
	}

	if (info)
		sde_kms_info_add_keystr(info,
				"display type",
				sde_hdmi->display_type);

	hdmi->connector = connector;
	INIT_WORK(&sde_hdmi->hpd_work, _sde_hdmi_hotplug_work);

	/* Enable HPD detection */
	rc = _sde_hdmi_hpd_enable(sde_hdmi);
	if (rc)
		SDE_ERROR("failed to enable HPD: %d\n", rc);

	return rc;
}

enum drm_connector_status
sde_hdmi_connector_detect(struct drm_connector *connector,
		bool force,
		void *display)
{
	enum drm_connector_status status = connector_status_unknown;
	struct msm_display_info info;
	int rc;

	if (!connector || !display) {
		SDE_ERROR("connector=%p or display=%p is NULL\n",
			connector, display);
		return status;
	}

	/* get display dsi_info */
	memset(&info, 0x0, sizeof(info));
	rc = sde_hdmi_get_info(&info, display);
	if (rc) {
		SDE_ERROR("failed to get display info, rc=%d\n", rc);
		return connector_status_disconnected;
	}

	if (info.capabilities & MSM_DISPLAY_CAP_HOT_PLUG)
		status = (info.is_connected ? connector_status_connected :
					      connector_status_disconnected);
	else
		status = connector_status_connected;

	connector->display_info.width_mm = info.width_mm;
	connector->display_info.height_mm = info.height_mm;

	return status;
}

int sde_hdmi_connector_get_modes(struct drm_connector *connector, void *display)
{
	struct sde_hdmi *hdmi_display = (struct sde_hdmi *)display;
	struct drm_display_mode *mode, *m;
	int ret = 0;

	if (!connector || !display) {
		SDE_ERROR("connector=%p or display=%p is NULL\n",
			connector, display);
		return 0;
	}

	if (hdmi_display->non_pluggable) {
		list_for_each_entry(mode, &hdmi_display->mode_list, head) {
			m = drm_mode_duplicate(connector->dev, mode);
			if (!m) {
				SDE_ERROR("failed to add hdmi mode %dx%d\n",
					mode->hdisplay, mode->vdisplay);
				break;
			}
			drm_mode_probed_add(connector, m);
		}
		ret = hdmi_display->num_of_modes;
	} else {
		/* pluggable case assumes EDID is read when HPD */
		ret = _sde_edid_update_modes(connector,
			hdmi_display->edid_ctrl);
	}

	return ret;
}

enum drm_mode_status sde_hdmi_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *display)
{
	struct sde_hdmi *hdmi_display = (struct sde_hdmi *)display;
	struct hdmi *hdmi;
	struct msm_drm_private *priv;
	struct msm_kms *kms;
	long actual, requested;

	if (!connector || !display || !mode) {
		SDE_ERROR("connector=%p or display=%p or mode=%p is NULL\n",
			connector, display, mode);
		return 0;
	}

	hdmi = hdmi_display->ctrl.ctrl;
	priv = connector->dev->dev_private;
	kms = priv->kms;
	requested = 1000 * mode->clock;
	actual = kms->funcs->round_pixclk(kms,
			requested, hdmi->encoder);

	SDE_HDMI_DEBUG("requested=%ld, actual=%ld", requested, actual);

	if (actual != requested)
		return MODE_CLOCK_RANGE;

	return MODE_OK;
}

int sde_hdmi_dev_init(struct sde_hdmi *display)
{
	if (!display) {
		SDE_ERROR("Invalid params\n");
		return -EINVAL;
	}
	return 0;
}

int sde_hdmi_dev_deinit(struct sde_hdmi *display)
{
	if (!display) {
		SDE_ERROR("Invalid params\n");
		return -EINVAL;
	}
	return 0;
}

static int _sde_hdmi_cec_init(struct sde_hdmi *display)
{
	struct platform_device *pdev = display->pdev;

	display->notifier = cec_notifier_get(&pdev->dev);
	if (!display->notifier) {
		SDE_ERROR("CEC notifier get failed\n");
		return -ENOMEM;
	}

	return 0;
}

static void _sde_hdmi_cec_deinit(struct sde_hdmi *display)
{
	cec_notifier_set_phys_addr(display->notifier, CEC_PHYS_ADDR_INVALID);
	cec_notifier_put(display->notifier);
}

static int sde_hdmi_bind(struct device *dev, struct device *master, void *data)
{
	int rc = 0;
	struct sde_hdmi_ctrl *display_ctrl = NULL;
	struct sde_hdmi *display = NULL;
	struct drm_device *drm = NULL;
	struct msm_drm_private *priv = NULL;
	struct platform_device *pdev = to_platform_device(dev);

	SDE_HDMI_DEBUG(" %s +\n", __func__);
	if (!dev || !pdev || !master) {
		pr_err("invalid param(s), dev %pK, pdev %pK, master %pK\n",
			dev, pdev, master);
		return -EINVAL;
	}

	drm = dev_get_drvdata(master);
	display = platform_get_drvdata(pdev);
	if (!drm || !display) {
		pr_err("invalid param(s), drm %pK, display %pK\n",
			   drm, display);
		return -EINVAL;
	}

	priv = drm->dev_private;
	mutex_lock(&display->display_lock);

	rc = _sde_hdmi_debugfs_init(display);
	if (rc) {
		SDE_ERROR("[%s]Debugfs init failed, rc=%d\n",
				display->name, rc);
		goto debug_error;
	}

	rc = _sde_hdmi_ext_disp_init(display);
	if (rc) {
		SDE_ERROR("[%s]Ext Disp init failed, rc=%d\n",
				display->name, rc);
		goto ext_error;
	}

	rc = _sde_hdmi_cec_init(display);
	if (rc) {
		SDE_ERROR("[%s]CEC init failed, rc=%d\n",
				display->name, rc);
		goto ext_error;
	}

	display->edid_ctrl = sde_edid_init();
	if (!display->edid_ctrl) {
		SDE_ERROR("[%s]sde edid init failed\n",
				display->name);
		rc = -ENOMEM;
		goto cec_error;
	}

	display_ctrl = &display->ctrl;
	display_ctrl->ctrl = priv->hdmi;
	display->drm_dev = drm;

	mutex_unlock(&display->display_lock);
	return rc;

cec_error:
	(void)_sde_hdmi_cec_deinit(display);
ext_error:
	(void)_sde_hdmi_debugfs_deinit(display);
debug_error:
	mutex_unlock(&display->display_lock);
	return rc;
}


static void sde_hdmi_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct sde_hdmi *display = NULL;

	if (!dev) {
		SDE_ERROR("invalid params\n");
		return;
	}

	display = platform_get_drvdata(to_platform_device(dev));
	if (!display) {
		SDE_ERROR("Invalid display device\n");
		return;
	}
	mutex_lock(&display->display_lock);
	(void)_sde_hdmi_debugfs_deinit(display);
	(void)sde_edid_deinit((void **)&display->edid_ctrl);
	(void)_sde_hdmi_cec_deinit(display);
	display->drm_dev = NULL;
	mutex_unlock(&display->display_lock);
}

static const struct component_ops sde_hdmi_comp_ops = {
	.bind = sde_hdmi_bind,
	.unbind = sde_hdmi_unbind,
};

static int _sde_hdmi_parse_dt_modes(struct device_node *np,
					struct list_head *head,
					u32 *num_of_modes)
{
	int rc = 0;
	struct drm_display_mode *mode;
	u32 mode_count = 0;
	struct device_node *node = NULL;
	struct device_node *root_node = NULL;
	const char *name;
	u32 h_front_porch, h_pulse_width, h_back_porch;
	u32 v_front_porch, v_pulse_width, v_back_porch;
	bool h_active_high, v_active_high;
	u32 flags = 0;

	root_node = of_get_child_by_name(np, "qcom,customize-modes");
	if (!root_node) {
		root_node = of_parse_phandle(np, "qcom,customize-modes", 0);
		if (!root_node) {
			DRM_INFO("No entry present for qcom,customize-modes");
			goto end;
		}
	}
	for_each_child_of_node(root_node, node) {
		rc = 0;
		mode = kzalloc(sizeof(*mode), GFP_KERNEL);
		if (!mode) {
			SDE_ERROR("Out of memory\n");
			rc =  -ENOMEM;
			continue;
		}

		rc = of_property_read_string(node, "qcom,mode-name",
						&name);
		if (rc) {
			SDE_ERROR("failed to read qcom,mode-name, rc=%d\n", rc);
			goto fail;
		}
		strlcpy(mode->name, name, DRM_DISPLAY_MODE_LEN);

		rc = of_property_read_u32(node, "qcom,mode-h-active",
						&mode->hdisplay);
		if (rc) {
			SDE_ERROR("failed to read h-active, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-h-front-porch",
						&h_front_porch);
		if (rc) {
			SDE_ERROR("failed to read h-front-porch, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-h-pulse-width",
						&h_pulse_width);
		if (rc) {
			SDE_ERROR("failed to read h-pulse-width, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-h-back-porch",
						&h_back_porch);
		if (rc) {
			SDE_ERROR("failed to read h-back-porch, rc=%d\n", rc);
			goto fail;
		}

		h_active_high = of_property_read_bool(node,
						"qcom,mode-h-active-high");

		rc = of_property_read_u32(node, "qcom,mode-v-active",
						&mode->vdisplay);
		if (rc) {
			SDE_ERROR("failed to read v-active, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-v-front-porch",
						&v_front_porch);
		if (rc) {
			SDE_ERROR("failed to read v-front-porch, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-v-pulse-width",
						&v_pulse_width);
		if (rc) {
			SDE_ERROR("failed to read v-pulse-width, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-v-back-porch",
						&v_back_porch);
		if (rc) {
			SDE_ERROR("failed to read v-back-porch, rc=%d\n", rc);
			goto fail;
		}

		v_active_high = of_property_read_bool(node,
						"qcom,mode-v-active-high");

		rc = of_property_read_u32(node, "qcom,mode-refersh-rate",
						&mode->vrefresh);
		if (rc) {
			SDE_ERROR("failed to read refersh-rate, rc=%d\n", rc);
			goto fail;
		}

		rc = of_property_read_u32(node, "qcom,mode-clock-in-khz",
						&mode->clock);
		if (rc) {
			SDE_ERROR("failed to read clock, rc=%d\n", rc);
			goto fail;
		}

		mode->hsync_start = mode->hdisplay + h_front_porch;
		mode->hsync_end = mode->hsync_start + h_pulse_width;
		mode->htotal = mode->hsync_end + h_back_porch;
		mode->vsync_start = mode->vdisplay + v_front_porch;
		mode->vsync_end = mode->vsync_start + v_pulse_width;
		mode->vtotal = mode->vsync_end + v_back_porch;
		if (h_active_high)
			flags |= DRM_MODE_FLAG_PHSYNC;
		else
			flags |= DRM_MODE_FLAG_NHSYNC;
		if (v_active_high)
			flags |= DRM_MODE_FLAG_PVSYNC;
		else
			flags |= DRM_MODE_FLAG_NVSYNC;
		mode->flags = flags;

		if (!rc) {
			mode_count++;
			list_add_tail(&mode->head, head);
		}

		SDE_DEBUG("mode[%d] h[%d,%d,%d,%d] v[%d,%d,%d,%d] %d %xH %d\n",
			mode_count - 1, mode->hdisplay, mode->hsync_start,
			mode->hsync_end, mode->htotal, mode->vdisplay,
			mode->vsync_start, mode->vsync_end, mode->vtotal,
			mode->vrefresh, mode->flags, mode->clock);
fail:
		if (rc) {
			kfree(mode);
			continue;
		}
	}

	if (num_of_modes)
		*num_of_modes = mode_count;

end:
	return rc;
}

static int _sde_hdmi_parse_dt(struct device_node *node,
				struct sde_hdmi *display)
{
	int rc = 0;

	display->name = of_get_property(node, "label", NULL);

	display->display_type = of_get_property(node,
						"qcom,display-type", NULL);
	if (!display->display_type)
		display->display_type = "unknown";

	display->non_pluggable = of_property_read_bool(node,
						"qcom,non-pluggable");

	rc = _sde_hdmi_parse_dt_modes(node, &display->mode_list,
					&display->num_of_modes);
	if (rc)
		SDE_ERROR("parse_dt_modes failed rc=%d\n", rc);

	return rc;
}

static int _sde_hdmi_dev_probe(struct platform_device *pdev)
{
	int rc;
	struct sde_hdmi *display;
	int ret = 0;


	SDE_DEBUG("\n");

	if (!pdev || !pdev->dev.of_node) {
		SDE_ERROR("pdev not found\n");
		return -ENODEV;
	}

	display = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);
	if (!display)
		return -ENOMEM;

	INIT_LIST_HEAD(&display->mode_list);
	rc = _sde_hdmi_parse_dt(pdev->dev.of_node, display);
	if (rc)
		SDE_ERROR("parse dt failed, rc=%d\n", rc);

	mutex_init(&display->display_lock);
	display->pdev = pdev;
	platform_set_drvdata(pdev, display);
	mutex_lock(&sde_hdmi_list_lock);
	list_add(&display->list, &sde_hdmi_list);
	mutex_unlock(&sde_hdmi_list_lock);
	if (!sde_hdmi_dev_init(display)) {
		ret = component_add(&pdev->dev, &sde_hdmi_comp_ops);
		if (ret) {
			pr_err("component add failed\n");
			goto out;
		}
	}
	return 0;

out:
	if (rc)
		devm_kfree(&pdev->dev, display);
	return rc;
}

static int _sde_hdmi_dev_remove(struct platform_device *pdev)
{
	struct sde_hdmi *display;
	struct sde_hdmi *pos, *tmp;
	struct drm_display_mode *mode, *n;

	if (!pdev) {
		SDE_ERROR("Invalid device\n");
		return -EINVAL;
	}

	display = platform_get_drvdata(pdev);

	mutex_lock(&sde_hdmi_list_lock);
	list_for_each_entry_safe(pos, tmp, &sde_hdmi_list, list) {
		if (pos == display) {
			list_del(&display->list);
			break;
		}
	}
	mutex_unlock(&sde_hdmi_list_lock);

	list_for_each_entry_safe(mode, n, &display->mode_list, head) {
		list_del(&mode->head);
		kfree(mode);
	}

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, display);
	return 0;
}

static struct platform_driver sde_hdmi_driver = {
	.probe = _sde_hdmi_dev_probe,
	.remove = _sde_hdmi_dev_remove,
	.driver = {
		.name = "sde_hdmi",
		.of_match_table = sde_hdmi_dt_match,
	},
};

static int sde_hdmi_irqdomain_map(struct irq_domain *domain,
		unsigned int irq, irq_hw_number_t hwirq)
{
	struct sde_hdmi *display;
	int rc;

	if (!domain || !domain->host_data) {
		pr_err("invalid parameters domain\n");
		return -EINVAL;
	}
	display = domain->host_data;

	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_level_irq);
	rc = irq_set_chip_data(irq, display);

	return rc;
}

static const struct irq_domain_ops sde_hdmi_irqdomain_ops = {
	.map = sde_hdmi_irqdomain_map,
	.xlate = irq_domain_xlate_onecell,
};

int sde_hdmi_drm_init(struct sde_hdmi *display, struct drm_encoder *enc)
{
	int rc = 0;
	struct msm_drm_private *priv = NULL;
	struct hdmi *hdmi;
	struct platform_device *pdev;
	struct sde_kms *sde_kms;

	DBG("");
	if (!display || !display->drm_dev || !enc) {
		SDE_ERROR("display=%p or enc=%p or drm_dev is NULL\n",
			display, enc);
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	priv = display->drm_dev->dev_private;
	hdmi = display->ctrl.ctrl;

	if (!priv || !hdmi) {
		SDE_ERROR("priv=%p or hdmi=%p is NULL\n",
			priv, hdmi);
		mutex_unlock(&display->display_lock);
		return -EINVAL;
	}

	pdev = hdmi->pdev;
	hdmi->dev = display->drm_dev;
	hdmi->encoder = enc;

	hdmi_audio_infoframe_init(&hdmi->audio.infoframe);

	hdmi->bridge = sde_hdmi_bridge_init(hdmi);
	if (IS_ERR(hdmi->bridge)) {
		rc = PTR_ERR(hdmi->bridge);
		SDE_ERROR("failed to create HDMI bridge: %d\n", rc);
		hdmi->bridge = NULL;
		goto error;
	}
	hdmi->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (hdmi->irq < 0) {
		rc = hdmi->irq;
		SDE_ERROR("failed to get irq: %d\n", rc);
		goto error;
	}

	rc = devm_request_irq(&pdev->dev, hdmi->irq,
			_sde_hdmi_irq, IRQF_TRIGGER_HIGH,
			"sde_hdmi_isr", display);
	if (rc < 0) {
		SDE_ERROR("failed to request IRQ%u: %d\n",
				hdmi->irq, rc);
		goto error;
	}

	display->irq_domain = irq_domain_add_linear(pdev->dev.of_node, 8,
				&sde_hdmi_irqdomain_ops, display);
	if (!display->irq_domain) {
		SDE_ERROR("failed to create IRQ domain\n");
		goto error;
	}

	enc->bridge = hdmi->bridge;
	priv->bridges[priv->num_bridges++] = hdmi->bridge;

	/*
	 * After initialising HDMI bridge, we need to check
	 * whether the early display is enabled for HDMI.
	 * If yes, we need to increase refcount of hdmi power
	 * clocks. This can skip the clock disabling operation in
	 * clock_late_init when finding clk.count == 1.
	 */
	sde_kms = to_sde_kms(priv->kms);
	if (sde_kms->splash_info.handoff) {
		sde_hdmi_bridge_power_on(hdmi->bridge);
		hdmi->power_on = true;
	}

	mutex_unlock(&display->display_lock);
	return 0;

error:
	/* bridge is normally destroyed by drm: */
	if (hdmi->bridge) {
		hdmi_bridge_destroy(hdmi->bridge);
		hdmi->bridge = NULL;
	}
	mutex_unlock(&display->display_lock);
	return rc;
}

int sde_hdmi_drm_deinit(struct sde_hdmi *display)
{
	int rc = 0;

	if (!display) {
		SDE_ERROR("Invalid params\n");
		return -EINVAL;
	}

	if (display->irq_domain)
		irq_domain_remove(display->irq_domain);

	return rc;
}

static int __init sde_hdmi_register(void)
{
	int rc = 0;

	DBG("");
	rc = platform_driver_register(&sde_hdmi_driver);
	return rc;
}

static void __exit sde_hdmi_unregister(void)
{
	platform_driver_unregister(&sde_hdmi_driver);
}

module_init(sde_hdmi_register);
module_exit(sde_hdmi_unregister);
