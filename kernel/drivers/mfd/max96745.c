// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96745 MFD driver
 *
 * Copyright (C) 2022 Rockchip Electronics Co. Ltd.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/gpio/consumer.h>
#include <linux/regmap.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max96745.h>

struct max96745 {
	struct device *dev;
	struct regmap *regmap;
	struct i2c_mux_core *muxc;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *lock_gpio;
};

static const struct mfd_cell max96745_devs[] = {
	{
		.name = "max96745-pinctrl",
		.of_compatible = "maxim,max96745-pinctrl",
	}, {
		.name = "max96745-bridge",
		.of_compatible = "maxim,max96745-bridge",
	},
};

static const struct regmap_config max96745_regmap_config = {
	.name = "max96745",
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x8000,
};

static int max96745_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct max96745 *max96745 = dev_get_drvdata(muxc->dev);

	if (chan == 0) {
		regmap_update_bits(max96745->regmap, 0x0028, LINK_EN,
				   FIELD_PREP(LINK_EN, 1));
	} else if (chan == 1) {
		regmap_update_bits(max96745->regmap, 0x0032, LINK_EN,
				   FIELD_PREP(LINK_EN, 1));
	} else {
		regmap_update_bits(max96745->regmap, 0x0028, LINK_EN,
				   FIELD_PREP(LINK_EN, 1));
		regmap_update_bits(max96745->regmap, 0x0032, LINK_EN,
				   FIELD_PREP(LINK_EN, 1));
	}

	return 0;
}

static void max96745_power_off(void *data)
{
	struct max96745 *max96745 = data;

	if (max96745->enable_gpio)
		gpiod_direction_output(max96745->enable_gpio, 0);
}

static int max96745_power_on(struct max96745 *max96745)
{
	u32 val;
	int ret;

	ret = regmap_read(max96745->regmap, 0x0107, &val);
	if (!ret && FIELD_GET(VID_TX_ACTIVE_A | VID_TX_ACTIVE_B, val))
		return 0;

	if (max96745->enable_gpio)
		gpiod_direction_output(max96745->enable_gpio, 1);

	msleep(100);

	ret = regmap_update_bits(max96745->regmap, 0x0010, RESET_ALL,
				 FIELD_PREP(RESET_ALL, 1));
	if (ret < 0)
		return ret;

	msleep(100);

	return 0;
}

static int max96745_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *child;
	struct max96745 *max96745;
	unsigned int nr = 0;
	int ret;

	for_each_available_child_of_node(dev->of_node, child) {
		if (!of_find_property(child, "reg", NULL))
			continue;

		nr++;
	}

	max96745 = devm_kzalloc(dev, sizeof(*max96745), GFP_KERNEL);
	if (!max96745)
		return -ENOMEM;

	max96745->muxc = i2c_mux_alloc(client->adapter, dev, nr, 0,
				       I2C_MUX_LOCKED, max96745_select, NULL);
	if (!max96745->muxc)
		return -ENOMEM;

	max96745->dev = dev;
	i2c_set_clientdata(client, max96745);

	max96745->regmap = devm_regmap_init_i2c(client, &max96745_regmap_config);
	if (IS_ERR(max96745->regmap))
		return dev_err_probe(dev, PTR_ERR(max96745->regmap),
				     "failed to initialize regmap");

	max96745->enable_gpio = devm_gpiod_get_optional(dev, "enable",
							GPIOD_ASIS);
	if (IS_ERR(max96745->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(max96745->enable_gpio),
				     "failed to get enable GPIO\n");

	max96745->lock_gpio = devm_gpiod_get_optional(dev, "lock", GPIOD_IN);
	if (IS_ERR(max96745->lock_gpio))
		return dev_err_probe(dev, PTR_ERR(max96745->lock_gpio),
				     "failed to get lock GPIO\n");

	ret = max96745_power_on(max96745);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, max96745_power_off, max96745);
	if (ret)
		return ret;

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, max96745_devs,
				   ARRAY_SIZE(max96745_devs), NULL, 0, NULL);
	if (ret)
		return ret;

	for_each_available_child_of_node(dev->of_node, child) {
		if (of_property_read_u32(child, "reg", &nr))
			continue;

		ret = i2c_mux_add_adapter(max96745->muxc, 0, nr, 0);
		if (ret) {
			i2c_mux_del_adapters(max96745->muxc);
			return ret;
		}
	}

	return 0;
}

static int max96745_i2c_remove(struct i2c_client *client)
{
	struct max96745 *max96745 = i2c_get_clientdata(client);

	i2c_mux_del_adapters(max96745->muxc);

	return 0;
}

static void max96745_i2c_shutdown(struct i2c_client *client)
{
	struct max96745 *max96745 = i2c_get_clientdata(client);

	max96745_power_off(max96745);
}

static const struct of_device_id max96745_of_match[] = {
	{ .compatible = "maxim,max96745", },
	{}
};
MODULE_DEVICE_TABLE(of, max96745_of_match);

static struct i2c_driver max96745_i2c_driver = {
	.driver = {
		.name = "max96745",
		.of_match_table = max96745_of_match,
	},
	.probe_new = max96745_i2c_probe,
	.remove = max96745_i2c_remove,
	.shutdown = max96745_i2c_shutdown,
};

module_i2c_driver(max96745_i2c_driver);

MODULE_AUTHOR("Wyon Bi <bivvy.bi@rock-chips.com>");
MODULE_DESCRIPTION("Maxim MAX96745 MFD driver");
MODULE_LICENSE("GPL");
