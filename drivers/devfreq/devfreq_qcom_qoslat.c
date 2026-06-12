// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "devfreq-qcom-qoslat: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/devfreq.h>
#include <linux/pm_opp.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/qmp.h>

#define MAX_MSG_LEN	96

struct qoslat_data {
	struct mbox_client		mbox_cl;
	struct mbox_chan		*mbox;
	struct devfreq			*df;
	struct devfreq_dev_profile	profile;
	unsigned int			qos_level;
	struct qmp_pkt			mbox_pkt;
	char				mbox_msg[MAX_MSG_LEN + 1];
	struct list_head		list;
};

#define QOS_LEVEL_OFF	1
#define QOS_LEVEL_ON	2

static LIST_HEAD(qoslat_list);
static DEFINE_SPINLOCK(qoslat_lock);
static DEFINE_MUTEX(qoslat_update_lock);
static unsigned int agg_qos_level = QOS_LEVEL_OFF;

static unsigned int qoslat_aggregate_locked(void)
{
	struct qoslat_data *d;
	unsigned int qos_lvl = QOS_LEVEL_OFF;

	list_for_each_entry(d, &qoslat_list, list)
		qos_lvl = max(d->qos_level, qos_lvl);

	return qos_lvl;
}

static int qoslat_send_level(struct qoslat_data *d, unsigned int qos_lvl)
{
	const char *qos_msg = qos_lvl == QOS_LEVEL_ON ? "on" : "off";
	struct device *dev = d->mbox_cl.dev;
	int ret;

	memset(d->mbox_msg, 0, sizeof(d->mbox_msg));
	snprintf(d->mbox_msg, MAX_MSG_LEN, "{class: ddr, perfmode: %s}",
		 qos_msg);

	d->mbox_pkt.size = MAX_MSG_LEN;
	d->mbox_pkt.data = d->mbox_msg;

	ret = mbox_send_message(d->mbox, &d->mbox_pkt);
	if (ret < 0)
		dev_err(dev, "Failed to send mbox message: %d\n", ret);

	return ret < 0 ? ret : 0;
}

static int update_qos_level(struct device *dev)
{
	struct qoslat_data *d = dev_get_drvdata(dev);
	unsigned int qos_lvl;
	int ret = 0;

	mutex_lock(&qoslat_update_lock);
	spin_lock(&qoslat_lock);
	qos_lvl = qoslat_aggregate_locked();

	if (qos_lvl == agg_qos_level) {
		spin_unlock(&qoslat_lock);
		goto out;
	}
	spin_unlock(&qoslat_lock);

	ret = qoslat_send_level(d, qos_lvl);
	if (ret < 0)
		goto out;

	spin_lock(&qoslat_lock);
	agg_qos_level = qos_lvl;
	spin_unlock(&qoslat_lock);

out:
	mutex_unlock(&qoslat_update_lock);
	return ret;
}

static int dev_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct qoslat_data *d = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned int old_level, new_level;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);
	else
		return PTR_ERR(opp);

	new_level = *freq;

	spin_lock(&qoslat_lock);
	old_level = d->qos_level;
	if (new_level != old_level)
		d->qos_level = new_level;
	spin_unlock(&qoslat_lock);

	ret = update_qos_level(dev);
	if (ret < 0 && new_level != old_level) {
		spin_lock(&qoslat_lock);
		if (d->qos_level == new_level)
			d->qos_level = old_level;
		spin_unlock(&qoslat_lock);
	}

	return ret;
}

static int dev_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct qoslat_data *d = dev_get_drvdata(dev);

	spin_lock(&qoslat_lock);
	*freq = d->qos_level;
	spin_unlock(&qoslat_lock);

	return 0;
}

static int dev_get_dev_status(struct device *dev,
			struct devfreq_dev_status *stat)
{
	struct qoslat_data *d = dev_get_drvdata(dev);

	spin_lock(&qoslat_lock);
	stat->current_frequency = d->qos_level;
	spin_unlock(&qoslat_lock);

	return 0;
}

static int devfreq_qcom_qoslat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qoslat_data *d;
	struct devfreq_dev_profile *p;
	const char *gov_name;
	int ret = 0;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	dev_set_drvdata(dev, d);
	INIT_LIST_HEAD(&d->list);

	if (!of_find_property(dev->of_node, "mboxes", NULL)) {
		dev_err(dev, "Couldn't find AOP mbox\n");
		return -EINVAL;
	}
	d->mbox_cl.dev = dev;
	d->mbox_cl.tx_block = true;
	d->mbox_cl.tx_tout = 1000;
	d->mbox_cl.knows_txdone = false;
	d->mbox = mbox_request_channel(&d->mbox_cl, 0);
	if (IS_ERR(d->mbox)) {
		ret = PTR_ERR(d->mbox);
		dev_err(dev, "Failed to get mailbox channel: %d\n", ret);
		return ret;
	}
	d->qos_level = QOS_LEVEL_OFF;

	p = &d->profile;
	p->target = dev_target;
	p->get_cur_freq = dev_get_cur_freq;
	p->get_dev_status = dev_get_dev_status;
	p->polling_ms = 10;

	ret = dev_pm_opp_of_add_table(dev);
	if (ret < 0) {
		dev_err(dev, "Couldn't parse OPP table: %d\n", ret);
		goto err_mbox;
	}

	if (of_property_read_string(dev->of_node, "governor", &gov_name))
		gov_name = "powersave";

	d->df = devfreq_add_device(dev, p, gov_name, NULL);
	if (IS_ERR(d->df)) {
		ret = PTR_ERR(d->df);
		dev_err(dev, "Failed to add devfreq device: %d\n", ret);
		goto err_opp;
	}

	spin_lock(&qoslat_lock);
	list_add_tail(&d->list, &qoslat_list);
	spin_unlock(&qoslat_lock);

	return 0;

err_opp:
	dev_pm_opp_of_remove_table(dev);
err_mbox:
	mbox_free_channel(d->mbox);
	return ret;
}

static int devfreq_qcom_qoslat_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qoslat_data *d = dev_get_drvdata(dev);
	int ret;

	if (!d)
		return 0;

	if (!IS_ERR_OR_NULL(d->df))
		devfreq_remove_device(d->df);

	spin_lock(&qoslat_lock);
	if (!list_empty(&d->list))
		list_del_init(&d->list);
	d->qos_level = QOS_LEVEL_OFF;
	spin_unlock(&qoslat_lock);

	ret = update_qos_level(dev);
	if (ret < 0)
		dev_warn(dev, "Failed to update qos level on remove: %d\n",
			 ret);

	dev_pm_opp_of_remove_table(dev);

	if (!IS_ERR_OR_NULL(d->mbox))
		mbox_free_channel(d->mbox);

	return 0;
}

static const struct of_device_id devfreq_qoslat_match_table[] = {
	{ .compatible = "qcom,devfreq-qoslat" },
	{}
};

static struct platform_driver devfreq_qcom_qoslat_driver = {
	.probe = devfreq_qcom_qoslat_probe,
	.remove = devfreq_qcom_qoslat_remove,
	.driver = {
		.name		= "devfreq-qcom-qoslat",
		.of_match_table = devfreq_qoslat_match_table,
		.suppress_bind_attrs = true,
	},
};

static int __init register_devfreq_qcom_qoslat_driver(void)
{
	return platform_driver_register(&devfreq_qcom_qoslat_driver);
}

late_initcall(register_devfreq_qcom_qoslat_driver);
MODULE_DESCRIPTION("Device driver for setting memory latency qos level");
MODULE_LICENSE("GPL v2");
