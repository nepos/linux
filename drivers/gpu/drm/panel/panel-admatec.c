/*
 * Copyright (C) 2018 Daniel Mack
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
#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct admatec_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator *digital_supply;
	struct regulator *analog_supply;

	struct gpio_desc *reset_gpio;
	struct backlight_device *backlight;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct admatec_panel *to_admatec_panel(struct drm_panel *panel)
{
	return container_of(panel, struct admatec_panel, base);
}

static int admatec_panel_init(struct admatec_panel *admatec)
{
	struct mipi_dsi_device *dsi = admatec->dsi;
	struct device *dev = &admatec->dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0)
		return ret;

	usleep_range(10000, 20000);

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				 (u8[]){ 0x2c }, 1);
	if (ret < 0) {
		dev_err(dev, "failed to write control display: %d\n", ret);
		return ret;
	}

	/* CABC off */
	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_WRITE_POWER_SAVE,
				 (u8[]){ 0x00 }, 1);
	if (ret < 0) {
		dev_err(dev, "failed to set cabc off: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "failed to set exit sleep mode: %d\n", ret);
		return ret;
	}

	usleep_range(20000, 40000);

	return 0;
}

static int admatec_panel_on(struct admatec_panel *admatec)
{
	struct mipi_dsi_device *dsi = admatec->dsi;
	struct device *dev = &admatec->dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display on: %d\n", ret);

	return ret;
}

static void admatec_panel_off(struct admatec_panel *admatec)
{
	struct mipi_dsi_device *dsi = admatec->dsi;
	struct device *dev = &admatec->dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_err(dev, "failed to enter sleep mode: %d\n", ret);

	msleep(100);
}

static int admatec_panel_disable(struct drm_panel *panel)
{
	struct admatec_panel *admatec = to_admatec_panel(panel);

	if (!admatec->enabled)
		return 0;

	admatec->backlight->props.power = FB_BLANK_POWERDOWN;
	admatec->backlight->props.state |= BL_CORE_FBBLANK;
	backlight_update_status(admatec->backlight);

	admatec->enabled = false;

	return 0;
}

static int admatec_panel_unprepare(struct drm_panel *panel)
{
	struct admatec_panel *admatec = to_admatec_panel(panel);

	if (!admatec->prepared)
		return 0;

	admatec_panel_off(admatec);
	gpiod_set_value(admatec->reset_gpio, 0);

	regulator_disable(admatec->digital_supply);
	regulator_disable(admatec->analog_supply);

	admatec->prepared = false;

	return 0;
}

static void admatec_panel_reset(struct admatec_panel *admatec)
{
	gpiod_set_value(admatec->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value(admatec->reset_gpio, 1);
}

static int admatec_panel_prepare(struct drm_panel *panel)
{
	struct admatec_panel *admatec = to_admatec_panel(panel);
	struct device *dev = &admatec->dsi->dev;
	int ret;

	if (admatec->prepared)
		return 0;

	ret = regulator_enable(admatec->digital_supply);
	if (ret < 0)
		goto out;

	usleep_range(1000, 2000);
	admatec_panel_reset(admatec);

	ret = regulator_enable(admatec->analog_supply);
	if (ret < 0)
		goto out_disable_digital;

	usleep_range(1000, 2000);
	admatec_panel_reset(admatec);

	ret = admatec_panel_init(admatec);
	if (ret < 0) {
		dev_err(dev, "failed to init panel: %d\n", ret);
		goto out_disable_analog;
	}

	ret = admatec_panel_on(admatec);
	if (ret < 0) {
		dev_err(dev, "failed to set panel on: %d\n", ret);
		goto out_disable_analog;
	}

	admatec->prepared = true;

	return 0;

out_disable_analog:
	regulator_disable(admatec->analog_supply);

out_disable_digital:
	regulator_disable(admatec->digital_supply);
out:
	gpiod_set_value(admatec->reset_gpio, 0);

	return ret;
}

static int admatec_panel_enable(struct drm_panel *panel)
{
	struct admatec_panel *admatec = to_admatec_panel(panel);

	if (admatec->enabled)
		return 0;

	admatec->backlight->props.state &= ~BL_CORE_FBBLANK;
	admatec->backlight->props.power = FB_BLANK_UNBLANK;
	backlight_update_status(admatec->backlight);

	admatec->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 75000,
	.hdisplay = 800,
	.hsync_start = 800 + 24,
	.hsync_end = 800 + 24 + 4,
	.htotal = 960,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8 + 4,
	.vtotal = 1300,
	.vrefresh = 60,
};

static int admatec_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct admatec_panel *admatec = to_admatec_panel(panel);
	struct device *dev = &admatec->dsi->dev;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 125;
	panel->connector->display_info.height_mm = 216;

	return 1;
}

static int dsi_dcs_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u8 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
				&brightness, sizeof(brightness));
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret < 0 ? ret : brightness;
}

static int dsi_dcs_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u8 brightness = bl->props.brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 &brightness, sizeof(brightness));
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret;
}

static const struct backlight_ops dsi_bl_ops = {
	.update_status = dsi_dcs_bl_update_status,
	.get_brightness = dsi_dcs_bl_get_brightness,
};

static struct backlight_device *
drm_panel_create_dsi_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.brightness = 255;
	props.max_brightness = 255;

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &dsi_bl_ops, &props);
}

static const struct drm_panel_funcs admatec_panel_funcs = {
	.disable = admatec_panel_disable,
	.unprepare = admatec_panel_unprepare,
	.prepare = admatec_panel_prepare,
	.enable = admatec_panel_enable,
	.get_modes = admatec_panel_get_modes,
};

static const struct of_device_id admatec_of_match[] = {
	{ .compatible = "admatec,lccm03r0009a-c00102", },
	{ }
};
MODULE_DEVICE_TABLE(of, admatec_of_match);

static int admatec_panel_add(struct admatec_panel *admatec)
{
	struct device *dev = &admatec->dsi->dev;
	int ret;

	admatec->mode = &default_mode;

	admatec->digital_supply = devm_regulator_get(dev, "digital");
	if (IS_ERR(admatec->digital_supply)) {
		ret = PTR_ERR(admatec->digital_supply);
		dev_err(dev, "unable to get digital supply: %d\n", ret);
		return ret;
	}

	admatec->analog_supply = devm_regulator_get(dev, "analog");
	if (IS_ERR(admatec->analog_supply)) {
		ret = PTR_ERR(admatec->analog_supply);
		dev_err(dev, "unable to get analog supply: %d\n", ret);
		return ret;
	}

	admatec->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(admatec->reset_gpio)) {
		ret = PTR_ERR(admatec->reset_gpio);
		dev_err(dev, "cannot get reset-gpios: %d\n", ret);
		return ret;
	}

	admatec->backlight = drm_panel_create_dsi_backlight(admatec->dsi);
	if (IS_ERR(admatec->backlight)) {
		ret = PTR_ERR(admatec->backlight);
		dev_err(dev, "failed to register backlight: %d\n", ret);
		return ret;
	}

	drm_panel_init(&admatec->base);
	admatec->base.funcs = &admatec_panel_funcs;
	admatec->base.dev = &admatec->dsi->dev;

	return drm_panel_add(&admatec->base);
}

static void admatec_panel_del(struct admatec_panel *admatec)
{
	if (admatec->base.dev)
		drm_panel_remove(&admatec->base);
}

static int admatec_panel_probe(struct mipi_dsi_device *dsi)
{
	struct admatec_panel *admatec;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_CLOCK_NON_CONTINUOUS;

	admatec = devm_kzalloc(&dsi->dev, sizeof(*admatec), GFP_KERNEL);
	if (!admatec)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, admatec);

	admatec->dsi = dsi;

	ret = admatec_panel_add(admatec);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static int admatec_panel_remove(struct mipi_dsi_device *dsi)
{
	struct admatec_panel *admatec = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = admatec_panel_disable(&admatec->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n",
			ret);

	drm_panel_detach(&admatec->base);
	admatec_panel_del(admatec);

	return 0;
}

static void admatec_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct admatec_panel *admatec = mipi_dsi_get_drvdata(dsi);

	admatec_panel_disable(&admatec->base);
}

static struct mipi_dsi_driver admatec_panel_driver = {
	.driver = {
		.name = "panel-admatec-lt070me05000",
		.of_match_table = admatec_of_match,
	},
	.probe = admatec_panel_probe,
	.remove = admatec_panel_remove,
	.shutdown = admatec_panel_shutdown,
};
module_mipi_dsi_driver(admatec_panel_driver);

MODULE_AUTHOR("Daniel Mack <daniel@nepos.io>");
MODULE_DESCRIPTION("Admatec MIPI DSI panel driver");
MODULE_LICENSE("GPL v2");
