// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2011-2014, 2017-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include "msm-pcm-routing-v2.h"

#define DRV_NAME "msm-pcm-hostless"

static int msm_pcm_hostless_prepare(struct snd_pcm_substream *substream)
{
	if (!substream) {
		pr_err("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	if (pm_qos_request_active(&substream->latency_pm_qos_req))
		pm_qos_remove_request(&substream->latency_pm_qos_req);
	return 0;
}

static const struct snd_pcm_ops msm_pcm_hostless_ops = {
	.prepare = msm_pcm_hostless_prepare
};

#if IS_ENABLED(CONFIG_AUDIO_QGKI)
static int msm_pcm_playback_app_type_cfg_ctl_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	int session_type = SESSION_TYPE_RX;
	int be_id = ucontrol->value.integer.value[3];
	struct msm_pcm_stream_app_type_cfg cfg_data = {0, 0, 48000};
	int ret;

	cfg_data.app_type = ucontrol->value.integer.value[0];
	cfg_data.acdb_dev_id = ucontrol->value.integer.value[1];
	if (ucontrol->value.integer.value[2] != 0)
		cfg_data.sample_rate = ucontrol->value.integer.value[2];

	ret = msm_pcm_routing_reg_stream_app_type_cfg(fe_id, session_type,
						      be_id, &cfg_data);
	if (ret < 0)
		pr_err("%s: app type cfg put failed %d\n", __func__, ret);

	return ret;
}

static int msm_pcm_playback_app_type_cfg_ctl_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	int session_type = SESSION_TYPE_RX;
	int be_id = 0;
	struct msm_pcm_stream_app_type_cfg cfg_data = {0};
	int ret;

	ret = msm_pcm_routing_get_stream_app_type_cfg(fe_id, session_type,
						      &be_id, &cfg_data);
	if (ret < 0) {
		pr_err("%s: app type cfg get failed %d\n", __func__, ret);
		return ret;
	}

	ucontrol->value.integer.value[0] = cfg_data.app_type;
	ucontrol->value.integer.value[1] = cfg_data.acdb_dev_id;
	ucontrol->value.integer.value[2] = cfg_data.sample_rate;
	ucontrol->value.integer.value[3] = be_id;

	return 0;
}

static int msm_pcm_capture_app_type_cfg_ctl_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	int session_type = SESSION_TYPE_TX;
	int be_id = ucontrol->value.integer.value[3];
	struct msm_pcm_stream_app_type_cfg cfg_data = {0, 0, 48000};
	int ret;

	cfg_data.app_type = ucontrol->value.integer.value[0];
	cfg_data.acdb_dev_id = ucontrol->value.integer.value[1];
	if (ucontrol->value.integer.value[2] != 0)
		cfg_data.sample_rate = ucontrol->value.integer.value[2];

	ret = msm_pcm_routing_reg_stream_app_type_cfg(fe_id, session_type,
						      be_id, &cfg_data);
	if (ret < 0)
		pr_err("%s: app type cfg put failed %d\n", __func__, ret);

	return ret;
}

static int msm_pcm_capture_app_type_cfg_ctl_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u64 fe_id = kcontrol->private_value;
	int session_type = SESSION_TYPE_TX;
	int be_id = 0;
	struct msm_pcm_stream_app_type_cfg cfg_data = {0};
	int ret;

	ret = msm_pcm_routing_get_stream_app_type_cfg(fe_id, session_type,
						      &be_id, &cfg_data);
	if (ret < 0) {
		pr_err("%s: app type cfg get failed %d\n", __func__, ret);
		return ret;
	}

	ucontrol->value.integer.value[0] = cfg_data.app_type;
	ucontrol->value.integer.value[1] = cfg_data.acdb_dev_id;
	ucontrol->value.integer.value[2] = cfg_data.sample_rate;
	ucontrol->value.integer.value[3] = be_id;

	return 0;
}

static int msm_pcm_hostless_add_app_type_controls(
		struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;
	struct snd_pcm_usr *app_type_info;
	struct snd_kcontrol *kctl;
	const char *playback_mixer_ctl_name = "Audio Stream";
	const char *capture_mixer_ctl_name = "Audio Stream Capture";
	const char *device_no = "NN";
	const char *suffix = "App Type Cfg";
	int ctl_len;
	int ret;

	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		ctl_len = strlen(playback_mixer_ctl_name) + 1 +
			  strlen(device_no) + 1 + strlen(suffix) + 1;
		ret = snd_pcm_add_usr_ctls(pcm, SNDRV_PCM_STREAM_PLAYBACK,
					   NULL, 1, ctl_len,
					   rtd->dai_link->id, &app_type_info);
		if (ret < 0)
			return ret;

		kctl = app_type_info->kctl;
		snprintf(kctl->id.name, ctl_len, "%s %d %s",
			 playback_mixer_ctl_name, rtd->pcm->device, suffix);
		kctl->put = msm_pcm_playback_app_type_cfg_ctl_put;
		kctl->get = msm_pcm_playback_app_type_cfg_ctl_get;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		ctl_len = strlen(capture_mixer_ctl_name) + 1 +
			  strlen(device_no) + 1 + strlen(suffix) + 1;
		ret = snd_pcm_add_usr_ctls(pcm, SNDRV_PCM_STREAM_CAPTURE,
					   NULL, 1, ctl_len,
					   rtd->dai_link->id, &app_type_info);
		if (ret < 0)
			return ret;

		kctl = app_type_info->kctl;
		snprintf(kctl->id.name, ctl_len, "%s %d %s",
			 capture_mixer_ctl_name, rtd->pcm->device, suffix);
		kctl->put = msm_pcm_capture_app_type_cfg_ctl_put;
		kctl->get = msm_pcm_capture_app_type_cfg_ctl_get;
	}

	return 0;
}

static int msm_pcm_hostless_new(struct snd_soc_pcm_runtime *rtd)
{
	return msm_pcm_hostless_add_app_type_controls(rtd);
}
#else
static int msm_pcm_hostless_new(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}
#endif

static struct snd_soc_component_driver msm_soc_hostless_component = {
	.name		= DRV_NAME,
	.ops		= &msm_pcm_hostless_ops,
	.pcm_new	= msm_pcm_hostless_new,
};

static int msm_pcm_hostless_probe(struct platform_device *pdev)
{

	pr_debug("%s: dev name %s\n", __func__, dev_name(&pdev->dev));
	return snd_soc_register_component(&pdev->dev,
				&msm_soc_hostless_component,
				NULL, 0);
}

static int msm_pcm_hostless_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id msm_pcm_hostless_dt_match[] = {
	{.compatible = "qcom,msm-pcm-hostless"},
	{}
};

static struct platform_driver msm_pcm_hostless_driver = {
	.driver = {
		.name = "msm-pcm-hostless",
		.owner = THIS_MODULE,
		.of_match_table = msm_pcm_hostless_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_pcm_hostless_probe,
	.remove = msm_pcm_hostless_remove,
};

int __init msm_pcm_hostless_init(void)
{
	return platform_driver_register(&msm_pcm_hostless_driver);
}

void msm_pcm_hostless_exit(void)
{
	platform_driver_unregister(&msm_pcm_hostless_driver);
}

MODULE_DESCRIPTION("Hostless platform driver");
MODULE_LICENSE("GPL v2");
