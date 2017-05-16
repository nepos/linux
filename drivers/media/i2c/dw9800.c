/*
 * drivers/media/i2c/dw9800.c
 *
 * DW9800W VCM driver
 *
 * Copyright (C) 2017 Daniel Mack
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define DW9800_REG_IC_INFO	0x00
#define DW9800_REG_IC_VERSION	0x01
#define DW9800_REG_CONTROL	0x02
#define DW9800_REG_VCM_MSB	0x03
#define DW9800_REG_VCM_LSB	0x04
#define DW9800_REG_STATUS	0x05
#define DW9800_REG_MODE		0x06
#define DW9800_REG_RESONANCE	0x07

static const struct reg_default dw9800_regmap_defaults[] = {
	{ DW9800_REG_CONTROL,	0x00 },
	{ DW9800_REG_VCM_MSB,	0x02 },
	{ DW9800_REG_VCM_LSB,	0x00 },
	{ DW9800_REG_STATUS,	0x00 },
	{ DW9800_REG_MODE,	0x00 },
	{ DW9800_REG_RESONANCE,	0x60 },
};

static bool dw9800_reg_is_volatile(struct device *dev, unsigned int reg)
{
	return reg < 2 || reg == 5;
}

static const struct regmap_config dw9800_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.max_register		= 0x07,

	.volatile_reg =		dw9800_reg_is_volatile,

	.reg_defaults		= dw9800_regmap_defaults,
	.num_reg_defaults	= ARRAY_SIZE(dw9800_regmap_defaults),
	.cache_type		= REGCACHE_RBTREE,
};

struct dw9800_device {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct regmap *regmap;
	struct regulator *supply;
};

static inline struct dw9800_device *to_dw9800_device(struct v4l2_subdev *sd)
{
	return container_of(sd, struct dw9800_device, sd);
}

/*
 * Power handling
 */
static int dw9800_set_power(struct dw9800_device *dw9800, bool enabled)
{
	int ret;

	ret = regmap_update_bits(dw9800->regmap, DW9800_REG_CONTROL,
				 0x01, !enabled);
	if (ret < 0)
		return ret;

	if (enabled)
		return regulator_enable(dw9800->supply);

	return regulator_disable(dw9800->supply);
}

/*
 * V4L2 controls
 */
static int dw9800_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dw9800_device *dw9800 =
		container_of(ctrl->handler, struct dw9800_device, ctrls);
	unsigned int reg;
	int ret;
	s32 val;

	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		ret = regmap_read(dw9800->regmap, DW9800_REG_CONTROL, &reg);
		if (ret < 0)
			return ret;

		if (reg & 0x01)
			return -EBUSY;

		val = ctrl->val;

		ret = regmap_write(dw9800->regmap, DW9800_REG_VCM_MSB,
				   (abs(val >> 8) & 0x01) |
				   ((val > 0) ? 0x02 : 0x00));
		if (ret < 0)
			return ret;

		ret = regmap_write(dw9800->regmap, DW9800_REG_VCM_LSB,
				   abs(val) & 0xff);
		if (ret < 0)
			return ret;

		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops dw9800_ctrl_ops = {
	.s_ctrl = dw9800_set_ctrl,
};

static int dw9800_init_controls(struct dw9800_device *dw9800)
{
	v4l2_ctrl_handler_init(&dw9800->ctrls, 1);

	v4l2_ctrl_new_std(&dw9800->ctrls, &dw9800_ctrl_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, -512, 511, 1, 0);

	if (dw9800->ctrls.error)
		return dw9800->ctrls.error;

	dw9800->sd.ctrl_handler = &dw9800->ctrls;

	return 0;
}

/*
 * V4L2 subdevice operations
 */
static int
dw9800_s_power(struct v4l2_subdev *sd, int on)
{
	struct dw9800_device *dw9800 = to_dw9800_device(sd);

	return dw9800_set_power(dw9800, on);
}

static int dw9800_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	int ret;

	ret = pm_runtime_get_sync(sd->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(sd->dev);
		return ret;
	}

	return 0;
}

static int dw9800_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_core_ops dw9800_core_ops = {
	.s_power = dw9800_s_power,
};

static const struct v4l2_subdev_ops dw9800_ops = {
	.core = &dw9800_core_ops,
};

static const struct v4l2_subdev_internal_ops dw9800_internal_ops = {
	.open = dw9800_open,
	.close = dw9800_close,
};

/*
 * I2C driver
 */
static int __maybe_unused dw9800_suspend(struct device *dev)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9800_device *dw9800 = to_dw9800_device(sd);

	return dw9800_set_power(dw9800, false);
}

static int __maybe_unused dw9800_resume(struct device *dev)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9800_device *dw9800 = to_dw9800_device(sd);

	return dw9800_set_power(dw9800, true);
}

static int dw9800_probe(struct i2c_client *client,
			const struct i2c_device_id *devid)
{
	struct device *dev = &client->dev;
	struct dw9800_device *dw9800;
	unsigned int val;
	int ret;

	dw9800 = devm_kzalloc(dev, sizeof(*dw9800), GFP_KERNEL);
	if (!dw9800)
		return -ENOMEM;

	dw9800->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(dw9800->supply)) {
		ret = PTR_ERR(dw9800->supply);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "could not get regulator for power supply\n");
		return ret;
	}

	dw9800->regmap = devm_regmap_init_i2c(client, &dw9800_regmap_config);
	if (IS_ERR(dw9800->regmap))
		return PTR_ERR(dw9800->regmap);

	ret = regmap_read(dw9800->regmap, DW9800_REG_IC_INFO, &val);
	if (ret < 0) {
		dev_err(dev, "Unable to read IC info register\n");
		return ret;
	}

	if (val != 0xf2) {
		dev_err(dev, "Failed to detect hardware\n");
		return -ENODEV;
	}

	ret = regmap_read(dw9800->regmap, DW9800_REG_IC_VERSION, &val);
	if (ret < 0) {
		dev_err(dev, "Unable to read IC version register\n");
		return ret;
	}

	dev_info(dev, "Detected DW9800W, hardware revision 0x%x\n", val & 0xf);

	v4l2_i2c_subdev_init(&dw9800->sd, client, &dw9800_ops);
	dw9800->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dw9800->sd.internal_ops = &dw9800_internal_ops;
	strcpy(dw9800->sd.name, "dw9800 focus");

	ret = dw9800_init_controls(dw9800);
	if (ret < 0)
		goto err_media_entity_cleanup;

	ret = media_entity_pads_init(&dw9800->sd.entity, 0, NULL);
	if (ret < 0)
		goto err_ctrl_handler_free;

	dw9800->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = v4l2_async_register_subdev(&dw9800->sd);
	if (ret < 0)
		goto err_ctrl_handler_free;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;

err_ctrl_handler_free:
	v4l2_ctrl_handler_free(&dw9800->ctrls);

err_media_entity_cleanup:
	media_entity_cleanup(&dw9800->sd.entity);
	return ret;
}

static int __exit dw9800_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9800_device *dw9800 = to_dw9800_device(sd);

	pm_runtime_disable(&client->dev);
	v4l2_async_unregister_subdev(&dw9800->sd);
	v4l2_ctrl_handler_free(&dw9800->ctrls);
	media_entity_cleanup(&dw9800->sd.entity);

	return 0;
}

static const struct i2c_device_id dw9800_i2c_table[] = {
	{ "dw9800", 0 },
	{ { 0 } }
};
MODULE_DEVICE_TABLE(i2c, dw9800_i2c_table);

static const struct of_device_id dw9800_of_table[] = {
	{ .compatible = "dongwoon,dw9800" },
	{ { 0 } }
};
MODULE_DEVICE_TABLE(of, dw9800_of_table);

static SIMPLE_DEV_PM_OPS(dw9800_pm, dw9800_suspend, dw9800_resume);

static struct i2c_driver dw9800_i2c_driver = {
	.driver = {
		.name = "dw9800",
		.pm = &dw9800_pm,
		.of_match_table = dw9800_of_table,
	},
	.probe = dw9800_probe,
	.remove = __exit_p(dw9800_remove),
	.id_table = dw9800_i2c_table,
};

module_i2c_driver(dw9800_i2c_driver);

MODULE_AUTHOR("Daniel Mack <linux@zonque.org");
MODULE_DESCRIPTION("DW9800W VCM driver");
MODULE_LICENSE("GPL");
