/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-of.h>
#include <media/v4l2-subdev.h>

#define OV13855_MEDIA_BUS_FMT MEDIA_BUS_FMT_SBGGR10_1X10

#define OV13855_REG_VALUE_08BIT		1
#define OV13855_REG_VALUE_16BIT		2
#define OV13855_REG_VALUE_24BIT		3

#define OV13855_REG_MODE_SELECT		0x0100
#define OV13855_MODE_STANDBY		0x00
#define OV13855_MODE_STREAMING		0x01

#define OV13855_REG_SOFTWARE_RST	0x0103
#define OV13855_SOFTWARE_RST		0x01

/* PLL1 generates PCLK and MIPI_PHY_CLK */
#define OV13855_REG_PLL1_CTRL_0		0x0300
#define OV13855_REG_PLL1_CTRL_1		0x0301
#define OV13855_REG_PLL1_CTRL_2		0x0302
#define OV13855_REG_PLL1_CTRL_3		0x0303
#define OV13855_REG_PLL1_CTRL_4		0x0304
#define OV13855_REG_PLL1_CTRL_5		0x0305

/* PLL2 generates DAC_CLK, SCLK and SRAM_CLK */
#define OV13855_REG_PLL2_CTRL_B		0x030b
#define OV13855_REG_PLL2_CTRL_C		0x030c
#define OV13855_REG_PLL2_CTRL_D		0x030d
#define OV13855_REG_PLL2_CTRL_E		0x030e
#define OV13855_REG_PLL2_CTRL_F		0x030f
#define OV13855_REG_PLL2_CTRL_12	0x0312
#define OV13855_REG_MIPI_SC_CTRL0	0x3016
#define OV13855_REG_MIPI_SC_CTRL1	0x3022

/* Chip ID */
#define OV13855_REG_CHIP_ID		0x300a
#define OV13855_CHIP_ID			0x00d855

/* V_TIMING internal */
#define OV13855_REG_VTS			0x380e
#define OV13855_VTS_30FPS		0x0c8e /* 30 fps */
#define OV13855_VTS_60FPS		0x0648 /* 60 fps */
#define OV13855_VTS_MAX			0x7fff
#define OV13855_VBLANK_MIN		56

/* HBLANK control - read only */
#define OV13855_PPL_540MHZ		2244
#define OV13855_PPL_1080MHZ		4488

/* Exposure control */
#define OV13855_REG_EXPOSURE		0x3500
#define OV13855_EXPOSURE_MIN		4
#define OV13855_EXPOSURE_STEP		1
#define OV13855_EXPOSURE_DEFAULT	0x640

/* Analog gain control */
#define OV13855_REG_ANALOG_GAIN		0x3508
#define OV13855_ANA_GAIN_MIN		0
#define OV13855_ANA_GAIN_MAX		0x1fff
#define OV13855_ANA_GAIN_STEP		1
#define OV13855_ANA_GAIN_DEFAULT	0x80

/* Digital gain control */
#define OV13855_REG_B_MWB_GAIN		0x5100
#define OV13855_REG_G_MWB_GAIN		0x5102
#define OV13855_REG_R_MWB_GAIN		0x5104
#define OV13855_DGTL_GAIN_MIN		0
#define OV13855_DGTL_GAIN_MAX		16384	/* Max = 16 X */
#define OV13855_DGTL_GAIN_DEFAULT	1024	/* Default gain = 1 X */
#define OV13855_DGTL_GAIN_STEP		1	/* Each step = 1/1024 */

/* Test Pattern Control */
#define OV13855_REG_TEST_PATTERN	0x4503
#define OV13855_TEST_PATTERN_ENABLE	BIT(7)
#define OV13855_TEST_PATTERN_MASK	0xfc

/* Number of frames to skip */
#define OV13855_NUM_OF_SKIP_FRAMES	2

struct ov13855_reg {
	u16 address;
	u8 val;
};

struct ov13855_reg_list {
	u32 num_of_regs;
	const struct ov13855_reg *regs;
};

/* Link frequency config */
struct ov13855_link_freq_config {
	u32 pixel_rate;
	u32 pixels_per_line;

	/* PLL registers for this link frequency */
	struct ov13855_reg_list reg_list;
};

/* Mode : resolution and related config&values */
struct ov13855_mode {
	/* Frame width */
	u32 width;
	/* Frame height */
	u32 height;

	/* V-timing */
	u32 vts;

	/* Index of Link frequency config to be used */
	u32 link_freq_index;
	/* Default register values */
	struct ov13855_reg_list reg_list;
};

static const struct ov13855_reg init_setting[] = {
	{ 0x0103, 0x01 },
	{ 0x0300, 0x02 },
	{ 0x0301, 0x00 },
	{ 0x0302, 0x5a },
	{ 0x0303, 0x00 },
	{ 0x0304, 0x00 },
	{ 0x0305, 0x01 },
	{ 0x030b, 0x06 },
	{ 0x030c, 0x02 },
	{ 0x030d, 0x88 },
	{ 0x0312, 0x11 },
	{ 0x3022, 0x01 },
	{ 0x3013, 0x32 },
	{ 0x3016, 0x72 },
	{ 0x301b, 0xf0 },
	{ 0x301f, 0xd0 },
	{ 0x3106, 0x15 },
	{ 0x3107, 0x23 },
	{ 0x3500, 0x00 },
	{ 0x3501, 0x80 },
	{ 0x3502, 0x00 },
	{ 0x3508, 0x02 },
	{ 0x3509, 0x00 },
	{ 0x350a, 0x00 },
	{ 0x350e, 0x00 },
	{ 0x3510, 0x00 },
	{ 0x3511, 0x02 },
	{ 0x3512, 0x00 },
	{ 0x3600, 0x2b },
	{ 0x3601, 0x52 },
	{ 0x3602, 0x60 },
	{ 0x3612, 0x05 },
	{ 0x3613, 0xa4 },
	{ 0x3620, 0x80 },
	{ 0x3621, 0x10 },
	{ 0x3622, 0x30 },
	{ 0x3624, 0x1c },
	{ 0x3640, 0x10 },
	{ 0x3661, 0x70 },
	{ 0x3661, 0x80 },
	{ 0x3662, 0x12 },
	{ 0x3664, 0x73 },
	{ 0x3665, 0xa7 },
	{ 0x366e, 0xff },
	{ 0x366f, 0xf4 },
	{ 0x3674, 0x00 },
	{ 0x3679, 0x0c },
	{ 0x367f, 0x01 },
	{ 0x3680, 0x0c },
	{ 0x3681, 0x50 },
	{ 0x3682, 0x50 },
	{ 0x3683, 0xa9 },
	{ 0x3684, 0xa9 },
	{ 0x3709, 0x5f },
	{ 0x3714, 0x24 },
	{ 0x371a, 0x3e },
	{ 0x3737, 0x04 },
	{ 0x3738, 0xcc },
	{ 0x3739, 0x12 },
	{ 0x373d, 0x26 },
	{ 0x3764, 0x20 },
	{ 0x3765, 0x20 },
	{ 0x37a1, 0x36 },
	{ 0x37a8, 0x3b },
	{ 0x37ab, 0x31 },
	{ 0x37c2, 0x04 },
	{ 0x37c3, 0xf1 },
	{ 0x37c5, 0x00 },
	{ 0x37d8, 0x03 },
	{ 0x37d9, 0x0c },
	{ 0x37da, 0xc2 },
	{ 0x37dc, 0x02 },
	{ 0x37e0, 0x00 },
	{ 0x37e1, 0x0a },
	{ 0x37e2, 0x14 },
	{ 0x37e3, 0x04 },
	{ 0x37e4, 0x2a },
	{ 0x37e5, 0x03 },
	{ 0x37e6, 0x04 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x08 },
	{ 0x3804, 0x10 },
	{ 0x3805, 0x9f },
	{ 0x3806, 0x0c },
	{ 0x3807, 0x57 },
	{ 0x3808, 0x10 },
	{ 0x3809, 0x80 },
	{ 0x380a, 0x0c },
	{ 0x380b, 0x40 },
	{ 0x380c, 0x04 },
	{ 0x380d, 0x62 },
	{ 0x380e, 0x0c },
	{ 0x380f, 0x8e },
	{ 0x3811, 0x10 },
	//{ 0x3812, 0x00 },
	{ 0x3813, 0x08 },
	{ 0x3814, 0x01 },
	{ 0x3815, 0x01 },
	{ 0x3816, 0x01 },
	{ 0x3817, 0x01 },
	{ 0x3820, 0xa8 },
	{ 0x3821, 0x00 },
	{ 0x3822, 0xc2 },
	{ 0x3823, 0x18 },
	{ 0x3826, 0x11 },
	{ 0x3827, 0x1c },
	{ 0x3829, 0x03 },
	{ 0x3832, 0x00 },
	{ 0x3c80, 0x00 },
	{ 0x3c87, 0x01 },
	{ 0x3c8c, 0x19 },
	{ 0x3c8d, 0x1c },
	{ 0x3c90, 0x00 },
	{ 0x3c91, 0x00 },
	{ 0x3c92, 0x00 },
	{ 0x3c93, 0x00 },
	{ 0x3c94, 0x40 },
	{ 0x3c95, 0x54 },
	{ 0x3c96, 0x34 },
	{ 0x3c97, 0x04 },
	{ 0x3c98, 0x00 },
	{ 0x3d8c, 0x73 },
	{ 0x3d8d, 0xc0 },
	{ 0x3f00, 0x0b },
	{ 0x3f03, 0x00 },
	{ 0x4001, 0xe0 },
	{ 0x4008, 0x00 },
	{ 0x4009, 0x0f },
	{ 0x4011, 0xf0 },
	{ 0x4050, 0x04 },
	{ 0x4051, 0x0b },
	{ 0x4052, 0x00 },
	{ 0x4053, 0x80 },
	{ 0x4054, 0x00 },
	{ 0x4055, 0x80 },
	{ 0x4056, 0x00 },
	{ 0x4057, 0x80 },
	{ 0x4058, 0x00 },
	{ 0x4059, 0x80 },
	{ 0x405e, 0x00 },
	{ 0x4500, 0x07 },
	{ 0x4503, 0x00 },
	{ 0x450a, 0x04 },
	{ 0x4809, 0x04 },
	{ 0x480c, 0x12 },
	{ 0x481f, 0x30 },
	{ 0x4833, 0x10 },
	{ 0x4837, 0x0e },
	{ 0x4902, 0x01 },
	{ 0x4d00, 0x03 },
	{ 0x4d01, 0xc9 },
	{ 0x4d02, 0xbc },
	{ 0x4d03, 0xd7 },
	{ 0x4d04, 0xf0 },
	{ 0x4d05, 0xa2 },
	{ 0x5000, 0xff },
	{ 0x5001, 0x07 },
	{ 0x5040, 0x39 },
	{ 0x5041, 0x10 },
	{ 0x5042, 0x10 },
	{ 0x5043, 0x84 },
	{ 0x5044, 0x62 },
	{ 0x5180, 0x00 },
	{ 0x5181, 0x10 },
	{ 0x5182, 0x02 },
	{ 0x5183, 0x0f },
	{ 0x5200, 0x1b },
	{ 0x520b, 0x07 },
	{ 0x520c, 0x0f },
	{ 0x5300, 0x04 },
	{ 0x5301, 0x0C },
	{ 0x5302, 0x0C },
	{ 0x5303, 0x0f },
	{ 0x5304, 0x00 },
	{ 0x5305, 0x70 },
	{ 0x5306, 0x00 },
	{ 0x5307, 0x80 },
	{ 0x5308, 0x00 },
	{ 0x5309, 0xa5 },
	{ 0x530a, 0x00 },
	{ 0x530b, 0xd3 },
	{ 0x530c, 0x00 },
	{ 0x530d, 0xf0 },
	{ 0x530e, 0x01 },
	{ 0x530f, 0x10 },
	{ 0x5310, 0x01 },
	{ 0x5311, 0x20 },
	{ 0x5312, 0x01 },
	{ 0x5313, 0x20 },
	{ 0x5314, 0x01 },
	{ 0x5315, 0x20 },
	{ 0x5316, 0x08 },
	{ 0x5317, 0x08 },
	{ 0x5318, 0x10 },
	{ 0x5319, 0x88 },
	{ 0x531a, 0x88 },
	{ 0x531b, 0xa9 },
	{ 0x531c, 0xaa },
	{ 0x531d, 0x0a },
	{ 0x5405, 0x02 },
	{ 0x5406, 0x67 },
	{ 0x5407, 0x01 },
	{ 0x5408, 0x4a }
};

static const struct ov13855_reg mode_4224x3136_regs[] = {
	{ 0x0300, 0x02 },
	{ 0x0301, 0x00 },
	{ 0x0302, 0x5a },
	{ 0x0303, 0x00 },
	{ 0x0304, 0x00 },
	{ 0x0305, 0x01 },
	{ 0x3022, 0x01 },
	{ 0x3013, 0x32 },
	{ 0x3016, 0x72 },
	{ 0x301b, 0xf0 },
	{ 0x301f, 0xd0 },
	{ 0x3106, 0x15 },
	{ 0x3107, 0x23 },
	{ 0x3500, 0x00 },
	{ 0x3501, 0xc8 },
	{ 0x3502, 0x60 },
	{ 0x3622, 0x30 },
	{ 0x3624, 0x1c },
	{ 0x3662, 0x12 },
	{ 0x3709, 0x5f },
	{ 0x3714, 0x24 },
	{ 0x3737, 0x04 },
	{ 0x3739, 0x12 },
	{ 0x37a1, 0x36 },
	{ 0x37a8, 0x3b },
	{ 0x37ab, 0x31 },
	{ 0x37c2, 0x04 },
	{ 0x37d9, 0x0c },
	{ 0x37e1, 0x0a },
	{ 0x37e2, 0x14 },
	{ 0x37e3, 0x04 },
	{ 0x37e4, 0x2a },
	{ 0x37e5, 0x03 },
	{ 0x37e6, 0x04 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x08 },
	{ 0x3804, 0x10 },
	{ 0x3805, 0x9f },
	{ 0x3806, 0x0c },
	{ 0x3807, 0x57 },
	{ 0x3808, 0x10 },
	{ 0x3809, 0x80 },
	{ 0x380a, 0x0c },
	{ 0x380b, 0x40 },
	{ 0x380c, 0x04 },
	{ 0x380d, 0x62 },
	{ 0x380e, 0x0c },
	{ 0x380f, 0x8e },
	{ 0x3811, 0x10 },
	{ 0x3812, 0x00 },
	{ 0x3813, 0x08 },
	{ 0x3814, 0x01 },
	{ 0x3815, 0x01 },
	{ 0x3816, 0x01 },
	{ 0x3817, 0x01 },
	{ 0x3820, 0xa8 },
	{ 0x3821, 0x00 },
	{ 0x3826, 0x11 },
	{ 0x3827, 0x1c },
	{ 0x3829, 0x03 },
	{ 0x3f03, 0x00 },
	{ 0x4009, 0x0f },
	{ 0x4011, 0xf0 },
	{ 0x4050, 0x04 },
	{ 0x4051, 0x0b },
	{ 0x4500, 0x07 },
	{ 0x4837, 0x0e },
	{ 0x4902, 0x01 },
	{ 0x4d00, 0x03 },
	{ 0x4d01, 0xc9 },
	{ 0x4d02, 0xbc },
	{ 0x4d03, 0xd7 },
	{ 0x4d04, 0xf0 },
	{ 0x4d05, 0xa2 },
	{ 0x5000, 0xff },
	{ 0x5041, 0x10 },
	{ 0x5042, 0x10 },
	{ 0x5043, 0x84 },
	{ 0x5044, 0x62 },
	{ 0x5300, 0x04 },
	{ 0x5301, 0x0c },
	{ 0x5302, 0x0c },
	{ 0x5303, 0x0f },
	{ 0x5305, 0x70 },
	{ 0x5307, 0x80 },
	{ 0x5309, 0xa5 },
	{ 0x530b, 0xd3 },
	{ 0x5319, 0x88 },
	{ 0x531a, 0x88 },
	{ 0x531b, 0xa9 },
	{ 0x531c, 0xaa },
	{ 0x531d, 0x0a },
	{ 0x5405, 0x02 },
	{ 0x5406, 0x67 },
	{ 0x5407, 0x01 },
	{ 0x5408, 0x4a }
};

static const struct ov13855_reg mode_4224x3136_zsl_regs[] = {
	{ 0x0103, 0x01 },
	{ 0x0300, 0x02 },
	{ 0x0301, 0x00 },
	{ 0x0302, 0x5a },
	{ 0x0303, 0x00 },
	{ 0x0304, 0x00 },
	{ 0x0305, 0x01 },
	{ 0x030b, 0x06 },
	{ 0x030c, 0x02 },
	{ 0x030d, 0x88 },
	{ 0x0312, 0x11 },
	{ 0x3022, 0x01 },
	{ 0x3013, 0x32 },
	{ 0x3016, 0x72 },
	{ 0x301b, 0xF0 },
	{ 0x301f, 0xd0 },
	{ 0x3106, 0x15 },
	{ 0x3107, 0x23 },
	{ 0x3500, 0x00 },
	{ 0x3501, 0x80 },
	{ 0x3502, 0x00 },
	{ 0x3508, 0x02 },
	{ 0x3509, 0x00 },
	{ 0x350a, 0x00 },
	{ 0x350e, 0x00 },
	{ 0x3510, 0x00 },
	{ 0x3511, 0x02 },
	{ 0x3512, 0x00 },
	{ 0x3600, 0x2b },
	{ 0x3601, 0x52 },
	{ 0x3602, 0x60 },
	{ 0x3612, 0x05 },
	{ 0x3613, 0xa4 },
	{ 0x3620, 0x80 },
	{ 0x3621, 0x10 },
	{ 0x3622, 0x30 },
	{ 0x3624, 0x1c },
	{ 0x3640, 0x10 },
	{ 0x3661, 0x70 },
	{ 0x3661, 0x80 },
	{ 0x3662, 0x12 },
	{ 0x3664, 0x73 },
	{ 0x3665, 0xa7 },
	{ 0x366e, 0xff },
	{ 0x366f, 0xf4 },
	{ 0x3674, 0x00 },
	{ 0x3679, 0x0c },
	{ 0x367f, 0x01 },
	{ 0x3680, 0x0c },
	{ 0x3681, 0x50 },
	{ 0x3682, 0x50 },
	{ 0x3683, 0xa9 },
	{ 0x3684, 0xa9 },
	{ 0x3709, 0x5f },
	{ 0x3714, 0x24 },
	{ 0x371a, 0x3e },
	{ 0x3737, 0x04 },
	{ 0x3738, 0xcc },
	{ 0x3739, 0x12 },
	{ 0x373d, 0x26 },
	{ 0x3764, 0x20 },
	{ 0x3765, 0x20 },
	{ 0x37a1, 0x36 },
	{ 0x37a8, 0x3b },
	{ 0x37ab, 0x31 },
	{ 0x37c2, 0x04 },
	{ 0x37c3, 0xf1 },
	{ 0x37c5, 0x00 },
	{ 0x37d8, 0x03 },
	{ 0x37d9, 0x0c },
	{ 0x37da, 0xc2 },
	{ 0x37dc, 0x02 },
	{ 0x37e0, 0x00 },
	{ 0x37e1, 0x0a },
	{ 0x37e2, 0x14 },
	{ 0x37e3, 0x04 },
	{ 0x37e4, 0x2a },
	{ 0x37e5, 0x03 },
	{ 0x37e6, 0x04 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x08 },
	{ 0x3804, 0x10 },
	{ 0x3805, 0x9f },
	{ 0x3806, 0x0c },
	{ 0x3807, 0x57 },
	{ 0x3808, 0x10 },
	{ 0x3809, 0x80 },
	{ 0x380a, 0x0c },
	{ 0x380b, 0x40 },
	{ 0x380c, 0x04 },
	{ 0x380d, 0x62 },
	{ 0x380e, 0x0c },
	{ 0x380f, 0x8e },
	{ 0x3811, 0x10 },
	{ 0x3813, 0x08 },
	{ 0x3814, 0x01 },
	{ 0x3815, 0x01 },
	{ 0x3816, 0x01 },
	{ 0x3817, 0x01 },
	{ 0x3820, 0xa8 },
	{ 0x3821, 0x00 },
	{ 0x3822, 0xc2 },
	{ 0x3823, 0x18 },
	{ 0x3826, 0x11 },
	{ 0x3827, 0x1c },
	{ 0x3829, 0x03 },
	{ 0x3832, 0x00 },
	{ 0x3c80, 0x00 },
	{ 0x3c87, 0x01 },
	{ 0x3c8c, 0x19 },
	{ 0x3c8d, 0x1c },
	{ 0x3c90, 0x00 },
	{ 0x3c91, 0x00 },
	{ 0x3c92, 0x00 },
	{ 0x3c93, 0x00 },
	{ 0x3c94, 0x40 },
	{ 0x3c95, 0x54 },
	{ 0x3c96, 0x34 },
	{ 0x3c97, 0x04 },
	{ 0x3c98, 0x00 },
	{ 0x3d8c, 0x73 },
	{ 0x3d8d, 0xc0 },
	{ 0x3f00, 0x0b },
	{ 0x3f03, 0x00 },
	{ 0x4001, 0xe0 },
	{ 0x4008, 0x00 },
	{ 0x4009, 0x0f },
	{ 0x4011, 0xf0 },
	{ 0x4016, 0x00 },
	{ 0x4017, 0x08 },
	{ 0x4050, 0x04 },
	{ 0x4051, 0x0b },
	{ 0x4052, 0x00 },
	{ 0x4053, 0x80 },
	{ 0x4054, 0x00 },
	{ 0x4055, 0x80 },
	{ 0x4056, 0x00 },
	{ 0x4057, 0x80 },
	{ 0x4058, 0x00 },
	{ 0x4059, 0x80 },
	{ 0x405e, 0x20 },
	{ 0x4500, 0x07 },
	{ 0x4503, 0x00 },
	{ 0x450a, 0x04 },
	{ 0x4809, 0x04 },
	{ 0x480c, 0x12 },
	{ 0x481f, 0x30 },
	{ 0x4833, 0x10 },
	{ 0x4837, 0x0e },
	{ 0x4902, 0x01 },
	{ 0x4d00, 0x03 },
	{ 0x4d01, 0xc9 },
	{ 0x4d02, 0xbc },
	{ 0x4d03, 0xd7 },
	{ 0x4d04, 0xf0 },
	{ 0x4d05, 0xa2 },
	{ 0x5000, 0xff },
	{ 0x5001, 0x07 },
	{ 0x5040, 0x39 },
	{ 0x5041, 0x10 },
	{ 0x5042, 0x10 },
	{ 0x5043, 0x84 },
	{ 0x5044, 0x62 },
	{ 0x5180, 0x00 },
	{ 0x5181, 0x10 },
	{ 0x5182, 0x02 },
	{ 0x5183, 0x0f },
	{ 0x5200, 0x1b },
	{ 0x520b, 0x07 },
	{ 0x520c, 0x0f },
	{ 0x5300, 0x04 },
	{ 0x5301, 0x0C },
	{ 0x5302, 0x0C },
	{ 0x5303, 0x0f },
	{ 0x5304, 0x00 },
	{ 0x5305, 0x70 },
	{ 0x5306, 0x00 },
	{ 0x5307, 0x80 },
	{ 0x5308, 0x00 },
	{ 0x5309, 0xa5 },
	{ 0x530a, 0x00 },
	{ 0x530b, 0xd3 },
	{ 0x530c, 0x00 },
	{ 0x530d, 0xf0 },
	{ 0x530e, 0x01 },
	{ 0x530f, 0x10 },
	{ 0x5310, 0x01 },
	{ 0x5311, 0x20 },
	{ 0x5312, 0x01 },
	{ 0x5313, 0x20 },
	{ 0x5314, 0x01 },
	{ 0x5315, 0x20 },
	{ 0x5316, 0x08 },
	{ 0x5317, 0x08 },
	{ 0x5318, 0x10 },
	{ 0x5319, 0x88 },
	{ 0x531a, 0x88 },
	{ 0x531b, 0xa9 },
	{ 0x531c, 0xaa },
	{ 0x531d, 0x0a },
	{ 0x5405, 0x02 },
	{ 0x5406, 0x67 },
	{ 0x5407, 0x01 },
	{ 0x5408, 0x4a },
};

static const struct ov13855_reg mode_2112x1568_regs[] = {
	{ 0x0300, 0x02 },
	{ 0x0301, 0x00 },
	{ 0x0302, 0x5a },
	{ 0x0303, 0x01 },
	{ 0x0304, 0x00 },
	{ 0x0305, 0x01 },
	{ 0x3022, 0x01 },
	{ 0x3013, 0x32 },
	{ 0x3016, 0x72 },
	{ 0x301b, 0xf0 },
	{ 0x301f, 0xd0 },
	{ 0x3106, 0x15 },
	{ 0x3107, 0x23 },
	{ 0x3500, 0x00 },
	{ 0x3501, 0x64 },
	{ 0x3502, 0x00 },
	{ 0x3622, 0x30 },
	{ 0x3624, 0x1c },
	{ 0x3662, 0x10 },
	{ 0x3709, 0x5f },
	{ 0x3714, 0x28 },
	{ 0x3737, 0x08 },
	{ 0x3739, 0x20 },
	{ 0x37a1, 0x36 },
	{ 0x37a8, 0x3b },
	{ 0x37ab, 0x31 },
	{ 0x37c2, 0x14 },
	{ 0x37d9, 0x0c },
	{ 0x37e1, 0x0a },
	{ 0x37e2, 0x14 },
	{ 0x37e3, 0x08 },
	{ 0x37e4, 0x38 },
	{ 0x37e5, 0x03 },
	{ 0x37e6, 0x08 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x08 },
	{ 0x3804, 0x10 },
	{ 0x3805, 0x9f },
	{ 0x3806, 0x0c },
	{ 0x3807, 0x4f },
	{ 0x3808, 0x08 },
	{ 0x3809, 0x40 },
	{ 0x380a, 0x06 },
	{ 0x380b, 0x20 },
	{ 0x380c, 0x04 },
	{ 0x380d, 0x62 },
	{ 0x380e, 0x06 },
	{ 0x380f, 0x48 },
	{ 0x3811, 0x08 },
	{ 0x3812, 0x00 },
	{ 0x3813, 0x02 },
	{ 0x3814, 0x03 },
	{ 0x3815, 0x01 },
	{ 0x3816, 0x03 },
	{ 0x3817, 0x01 },
	{ 0x3820, 0xab },
	{ 0x3821, 0x00 },
	{ 0x3826, 0x04 },
	{ 0x3827, 0x90 },
	{ 0x3829, 0x07 },
	{ 0x3f03, 0x00 },
	{ 0x4009, 0x0d },
	{ 0x4011, 0xf0 },
	{ 0x4050, 0x04 },
	{ 0x4051, 0x0b },
	{ 0x4500, 0x07 },
	{ 0x4837, 0x1c },
	{ 0x4902, 0x01 },
	{ 0x4d00, 0x03 },
	{ 0x4d01, 0xc9 },
	{ 0x4d02, 0xbc },
	{ 0x4d03, 0xd7 },
	{ 0x4d04, 0xf0 },
	{ 0x4d05, 0xa2 },
	{ 0x5000, 0xff },
	{ 0x5041, 0x10 },
	{ 0x5042, 0x10 },
	{ 0x5043, 0x84 },
	{ 0x5044, 0x62 },
	{ 0x5300, 0x04 },
	{ 0x5301, 0x0c },
	{ 0x5302, 0x0c },
	{ 0x5303, 0x0f },
	{ 0x5305, 0x70 },
	{ 0x5307, 0x80 },
	{ 0x5309, 0xa5 },
	{ 0x530b, 0xd3 },
	{ 0x5319, 0x88 },
	{ 0x531a, 0x88 },
	{ 0x531b, 0xa9 },
	{ 0x531c, 0xaa },
	{ 0x531d, 0x0a },
	{ 0x5405, 0x02 },
	{ 0x5406, 0x67 },
	{ 0x5407, 0x01 },
	{ 0x5408, 0x4a },
};

static const struct ov13855_reg mode_1056x784_regs[] = {
	{ 0x3013, 0x32 },
	{ 0x301b, 0xf0 },
	{ 0x301f, 0xd0 },
	{ 0x3106, 0x15 },
	{ 0x3107, 0x23 },
	{ 0x350a, 0x00 },
	{ 0x350e, 0x00 },
	{ 0x3510, 0x00 },
	{ 0x3511, 0x02 },
	{ 0x3512, 0x00 },
	{ 0x3600, 0x2b },
	{ 0x3601, 0x52 },
	{ 0x3602, 0x60 },
	{ 0x3612, 0x05 },
	{ 0x3613, 0xa4 },
	{ 0x3620, 0x80 },
	{ 0x3621, 0x10 },
	{ 0x3622, 0x30 },
	{ 0x3624, 0x1c },
	{ 0x3640, 0x10 },
	{ 0x3641, 0x70 },
	{ 0x3661, 0x80 },
	{ 0x3662, 0x08 },
	{ 0x3664, 0x73 },
	{ 0x3665, 0xa7 },
	{ 0x366e, 0xff },
	{ 0x366f, 0xf4 },
	{ 0x3674, 0x00 },
	{ 0x3679, 0x0c },
	{ 0x367f, 0x01 },
	{ 0x3680, 0x0c },
	{ 0x3681, 0x50 },
	{ 0x3682, 0x50 },
	{ 0x3683, 0xa9 },
	{ 0x3684, 0xa9 },
	{ 0x3709, 0x5f },
	{ 0x3714, 0x30 },
	{ 0x371a, 0x3e },
	{ 0x3737, 0x08 },
	{ 0x3738, 0xcc },
	{ 0x3739, 0x20 },
	{ 0x373d, 0x26 },
	{ 0x3764, 0x20 },
	{ 0x3765, 0x20 },
	{ 0x37a1, 0x36 },
	{ 0x37a8, 0x3b },
	{ 0x37ab, 0x31 },
	{ 0x37c2, 0x2c },
	{ 0x37c3, 0xf1 },
	{ 0x37c5, 0x00 },
	{ 0x37d8, 0x03 },
	{ 0x37d9, 0x06 },
	{ 0x37da, 0xc2 },
	{ 0x37dc, 0x02 },
	{ 0x37e0, 0x00 },
	{ 0x37e1, 0x0a },
	{ 0x37e2, 0x14 },
	{ 0x37e3, 0x08 },
	{ 0x37e4, 0x36 },
	{ 0x37e5, 0x03 },
	{ 0x37e6, 0x08 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x00 },
	{ 0x3804, 0x10 },
	{ 0x3805, 0x9f },
	{ 0x3806, 0x0c },
	{ 0x3807, 0x5f },
	{ 0x3808, 0x04 },
	{ 0x3809, 0x20 },
	{ 0x380a, 0x03 },
	{ 0x380b, 0x10 },
	{ 0x380c, 0x04 },
	{ 0x380d, 0x62 },
	{ 0x380e, 0x0c },
	{ 0x380f, 0x8e },
	{ 0x3811, 0x04 },
	{ 0x3813, 0x05 },
	{ 0x3814, 0x07 },
	{ 0x3815, 0x01 },
	{ 0x3816, 0x07 },
	{ 0x3817, 0x01 },
	{ 0x3820, 0xac },
	{ 0x3821, 0x00 },
	{ 0x3822, 0xc2 },
	{ 0x3823, 0x18 },
	{ 0x3826, 0x04 },
	{ 0x3827, 0x48 },
	{ 0x3829, 0x03 },
	{ 0x3832, 0x00 },
	{ 0x3c80, 0x00 },
	{ 0x3c87, 0x01 },
	{ 0x3c8c, 0x19 },
	{ 0x3c8d, 0x1c },
	{ 0x3c90, 0x00 },
	{ 0x3c91, 0x00 },
	{ 0x3c92, 0x00 },
	{ 0x3c93, 0x00 },
	{ 0x3c94, 0x40 },
	{ 0x3c95, 0x54 },
	{ 0x3c96, 0x34 },
	{ 0x3c97, 0x04 },
	{ 0x3c98, 0x00 },
	{ 0x3d8c, 0x73 },
	{ 0x3d8d, 0xc0 },
	{ 0x3f00, 0x0b },
	{ 0x3f03, 0x00 },
	{ 0x4001, 0xe0 },
	{ 0x4008, 0x00 },
	{ 0x4009, 0x05 },
	{ 0x4011, 0xf0 },
	{ 0x4017, 0x08 },
	{ 0x4050, 0x02 },
	{ 0x4051, 0x05 },
	{ 0x4052, 0x00 },
	{ 0x4053, 0x80 },
	{ 0x4054, 0x00 },
	{ 0x4055, 0x80 },
	{ 0x4056, 0x00 },
	{ 0x4057, 0x80 },
	{ 0x4058, 0x00 },
	{ 0x4059, 0x80 },
	{ 0x405e, 0x20 },
	{ 0x4500, 0x07 },
	{ 0x4503, 0x00 },
	{ 0x450a, 0x04 },
	{ 0x4809, 0x04 },
	{ 0x480c, 0x12 },
	{ 0x481f, 0x30 },
	{ 0x4833, 0x10 },
	{ 0x4837, 0x1e },
	{ 0x4902, 0x02 },
	{ 0x4d00, 0x03 },
	{ 0x4d01, 0xc9 },
	{ 0x4d02, 0xbc },
	{ 0x4d03, 0xd7 },
	{ 0x4d04, 0xf0 },
	{ 0x4d05, 0xa2 },
	{ 0x5000, 0xfd },
	{ 0x5001, 0x01 },
	{ 0x5040, 0x39 },
	{ 0x5041, 0x10 },
	{ 0x5042, 0x10 },
	{ 0x5043, 0x84 },
	{ 0x5044, 0x62 },
	{ 0x5180, 0x00 },
	{ 0x5181, 0x10 },
	{ 0x5182, 0x02 },
	{ 0x5183, 0x0f },
	{ 0x5200, 0x1b },
	{ 0x520b, 0x07 },
	{ 0x520c, 0x0f },
	{ 0x5300, 0x04 },
	{ 0x5301, 0x0c },
	{ 0x5302, 0x0c },
	{ 0x5303, 0x0f },
	{ 0x5304, 0x00 },
	{ 0x5305, 0x70 },
	{ 0x5306, 0x00 },
	{ 0x5307, 0x80 },
	{ 0x5308, 0x00 },
	{ 0x5309, 0xa5 },
	{ 0x530a, 0x00 },
	{ 0x530b, 0xd3 },
	{ 0x530c, 0x00 },
	{ 0x530d, 0xf0 },
	{ 0x530e, 0x01 },
	{ 0x530f, 0x10 },
	{ 0x5310, 0x01 },
	{ 0x5311, 0x20 },
	{ 0x5312, 0x01 },
	{ 0x5313, 0x20 },
	{ 0x5314, 0x01 },
	{ 0x5315, 0x20 },
	{ 0x5316, 0x08 },
	{ 0x5317, 0x08 },
	{ 0x5318, 0x10 },
	{ 0x5319, 0x88 },
	{ 0x531a, 0x88 },
	{ 0x531b, 0xa9 },
	{ 0x531c, 0xaa },
	{ 0x531d, 0x0a },
	{ 0x5405, 0x02 },
	{ 0x5406, 0x67 },
	{ 0x5407, 0x01 },
	{ 0x5408, 0x4a }
};

static const char * const ov13855_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

/* Configurations for supported link frequencies */
#define OV13855_NUM_OF_LINK_FREQS	2
#define OV13855_LINK_FREQ_1080MBPS	1080000000
#define OV13855_LINK_FREQ_540MBPS	540000000
#define OV13855_LINK_FREQ_INDEX_0	0
#define OV13855_LINK_FREQ_INDEX_1	1

/* Menu items for LINK_FREQ V4L2 control */
static const const s64 link_freq_menu_items[OV13855_NUM_OF_LINK_FREQS] = {
	OV13855_LINK_FREQ_1080MBPS,
	OV13855_LINK_FREQ_540MBPS
};

static const struct ov13855_reg mipi_data_rate_1080mbps[] = {
};

static const struct ov13855_reg mipi_data_rate_540mbps[] = {
};

/* Link frequency configs */
static const struct ov13855_link_freq_config
			link_freq_configs[OV13855_NUM_OF_LINK_FREQS] = {
	{
		.pixel_rate = 4224*3136*30,
		.pixels_per_line = OV13855_PPL_1080MHZ,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mipi_data_rate_1080mbps),
			.regs = mipi_data_rate_1080mbps,
		}
	},
	{
		.pixel_rate = 2112*1568*60,
		.pixels_per_line = OV13855_PPL_540MHZ,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mipi_data_rate_540mbps),
			.regs = mipi_data_rate_540mbps,
		}
	}
};
enum ov13855_frame_rate {
	OV13855_30_FPS = 0,
	OV13855_60_FPS,
	OV13855_120_FPS,
	OV13855_NUM_FRAMERATES,
};

static const int ov13855_framerates[] = {
	[OV13855_30_FPS] = 30,
	[OV13855_60_FPS] = 60,
	[OV13855_120_FPS] = 120,
};

/* Mode configs */
static const struct ov13855_mode supported_modes[] = {
	{
		.width = 4224,
		.height = 3136,
		.vts = OV13855_VTS_30FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4224x3136_regs),
			.regs = mode_4224x3136_regs,
		},
		.link_freq_index = OV13855_LINK_FREQ_INDEX_0,
	},
	{
		.width = 2112,
		.height = 1568,
		.vts = OV13855_VTS_60FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2112x1568_regs),
			.regs = mode_2112x1568_regs,
		},
		.link_freq_index = OV13855_LINK_FREQ_INDEX_1,
	},
	{
		.width = 1056,
		.height = 784,
		.vts = OV13855_VTS_30FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1056x784_regs),
			.regs = mode_1056x784_regs,
		},
		.link_freq_index = OV13855_LINK_FREQ_INDEX_1,
	}
};

/* regulator supplies */
static const char * const ov13855_supply_name[] = {
	"DOVDD", /* Digital I/O (1.8V) suppply */
	"DVDD",  /* Digital Core (1.5V) supply */
	"AVDD",  /* Analog (2.8V) supply */
};

#define OV13855_NUM_SUPPLIES ARRAY_SIZE(ov13855_supply_name)

struct ov13855 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;

	/* Current mode */
	const struct ov13855_mode *cur_mode;

	struct v4l2_fract frame_interval;
	struct regulator_bulk_data supplies[OV13855_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct v4l2_of_endpoint ep; /* the parsed DT endpoint info */
	struct clk *xclk;
	int power_count;

	/* Mutex for serialized access */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;
};

#define to_ov13855(_sd)	container_of(_sd, struct ov13855, sd)

/* Read registers up to 4 at a time */
static int ov13855_read_reg(struct ov13855 *ov13855, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	int ret;
	u32 data_be = 0;
	u16 reg_addr_be = cpu_to_be16(reg);

	if (len > 4)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD | I2C_M_NOSTART;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

/* Write registers up to 4 at a time */
static int ov13855_write_reg(struct ov13855 *ov13855, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	int buf_i, val_i;
	u8 buf[6], *val_p;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val = cpu_to_be32(val);
	val_p = (u8 *)&val;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int ov13855_write_regs(struct ov13855 *ov13855,
			      const struct ov13855_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	int ret;
	u32 i;

	for (i = 0; i < len; i++) {
		ret = ov13855_write_reg(ov13855, regs[i].address, 1,
					regs[i].val);
		if (ret) {
			dev_err_ratelimited(
				&client->dev,
				"Failed to write reg 0x%4.4x. error = %d\n",
				regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

static int ov13855_write_reg_list(struct ov13855 *ov13855,
				  const struct ov13855_reg_list *r_list)
{
	return ov13855_write_regs(ov13855, r_list->regs, r_list->num_of_regs);
}

/* Open sub-device */
static int ov13855_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, fh->pad, 0);

	mutex_lock(&ov13855->mutex);

	/* Initialize try_fmt */
	try_fmt->width = ov13855->cur_mode->width;
	try_fmt->height = ov13855->cur_mode->height;
	try_fmt->code = OV13855_MEDIA_BUS_FMT;
	try_fmt->colorspace = 0; //V4L2_COLORSPACE_SRGB;
	try_fmt->field = V4L2_FIELD_NONE;

printk(KERN_ERR "%s() :%d -- %dx%d\n", __func__, __LINE__, try_fmt->width, try_fmt->height);

	/* No crop or compose */
	mutex_unlock(&ov13855->mutex);

	return 0;
}

static int ov13855_update_digital_gain(struct ov13855 *ov13855, u32 d_gain)
{
	int ret;

	ret = ov13855_write_reg(ov13855, OV13855_REG_B_MWB_GAIN,
				OV13855_REG_VALUE_16BIT, d_gain);
	if (ret)
		goto out;

	ret = ov13855_write_reg(ov13855, OV13855_REG_G_MWB_GAIN,
				OV13855_REG_VALUE_16BIT, d_gain);
	if (ret)
		goto out;

	ret = ov13855_write_reg(ov13855, OV13855_REG_R_MWB_GAIN,
				OV13855_REG_VALUE_16BIT, d_gain);

out:
printk(KERN_ERR "%s() :%d ret %d\n", __func__, __LINE__, ret);
	return ret;
}

static int ov13855_enable_test_pattern(struct ov13855 *ov13855, u32 pattern)
{
	int ret;
	u32 val;

pattern = 1;

	ret = ov13855_read_reg(ov13855, OV13855_REG_TEST_PATTERN,
			       OV13855_REG_VALUE_08BIT, &val);
	if (ret)
		goto out;

	if (pattern) {
		val &= OV13855_TEST_PATTERN_MASK;
		val |= (pattern - 1) | OV13855_TEST_PATTERN_ENABLE;
	} else {
		val &= ~OV13855_TEST_PATTERN_ENABLE;
	}

	ret = ov13855_write_reg(ov13855, OV13855_REG_TEST_PATTERN,
				 OV13855_REG_VALUE_08BIT, val);

out:
printk(KERN_ERR "%s() :%d ret %d\n", __func__, __LINE__, ret);

	return ret;
}

static int ov13855_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov13855 *ov13855 = container_of(ctrl->handler,
					       struct ov13855, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	s64 max;
	int ret;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = ov13855->cur_mode->height + ctrl->val - 8;
		__v4l2_ctrl_modify_range(ov13855->exposure,
					 ov13855->exposure->minimum,
					 max, ov13855->exposure->step, max);
		break;
	};

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) <= 0)
		return 0;

	ret = 0;
	switch (ctrl->id) {
#if 0
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov13855_write_reg(ov13855, OV13855_REG_ANALOG_GAIN,
					OV13855_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = ov13855_update_digital_gain(ov13855, ctrl->val);
		break;
#endif
	case V4L2_CID_EXPOSURE:
		ret = ov13855_write_reg(ov13855, OV13855_REG_EXPOSURE,
					OV13855_REG_VALUE_24BIT,
					ctrl->val << 4);
		break;
	case V4L2_CID_VBLANK:
		/* Update VTS that meets expected vertical blanking */
		ret = ov13855_write_reg(ov13855, OV13855_REG_VTS,
					OV13855_REG_VALUE_16BIT,
					ov13855->cur_mode->height
					  + ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov13855_enable_test_pattern(ov13855, ctrl->val);
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		break;
	};

	pm_runtime_put(&client->dev);
printk(KERN_ERR "%s() :%d ctrl id %08x ret %d\n", __func__, __LINE__, ctrl->id, ret);

	return ret;
}

static const struct v4l2_ctrl_ops ov13855_ctrl_ops = {
	.s_ctrl = ov13855_set_ctrl,
};

static int ov13855_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	/* Only one bayer order(GRBG) is supported */
	if (code->index > 0)
		return -EINVAL;

	code->code = OV13855_MEDIA_BUS_FMT;

	return 0;
}

static int ov13855_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != OV13855_MEDIA_BUS_FMT)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

printk(KERN_ERR "%s() :: %d %d %d %d\n", __func__, fse->min_width, fse->max_width, fse->min_height, fse->max_height);

	return 0;
}

static void ov13855_update_pad_format(const struct ov13855_mode *mode,
				      struct v4l2_subdev_format *fmt)
{
printk(KERN_ERR "%s() :%d\n", __func__, __LINE__);
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = OV13855_MEDIA_BUS_FMT;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = 0; //V4L2_COLORSPACE_SRGB;
}

static int ov13855_do_get_pad_format(struct ov13855 *ov13855,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt;
	struct v4l2_subdev *sd = &ov13855->sd;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
		fmt->format = *framefmt;
	} else {
		ov13855_update_pad_format(ov13855->cur_mode, fmt);
	}

	return 0;
}

static int ov13855_get_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_format *fmt)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	int ret;

	mutex_lock(&ov13855->mutex);
	ret = ov13855_do_get_pad_format(ov13855, cfg, fmt);
	mutex_unlock(&ov13855->mutex);
printk(KERN_ERR "%s() :%d   ret %d\n", __func__, __LINE__, ret);

	return ret;
}

/*
 * Calculate resolution distance
 */
static int
ov13855_get_resolution_dist(const struct ov13855_mode *mode,
			    struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

/*
 * Find the closest supported resolution to the requested resolution
 */
static const struct ov13855_mode *
ov13855_find_best_fit(struct ov13855 *ov13855,
		      struct v4l2_subdev_format *fmt)
{
	int i, dist, cur_best_fit = 0, cur_best_fit_dist = -1;
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = ov13855_get_resolution_dist(&supported_modes[i],
						   framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int
ov13855_set_pad_format(struct v4l2_subdev *sd,
		       struct v4l2_subdev_pad_config *cfg,
		       struct v4l2_subdev_format *fmt)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	const struct ov13855_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;
	s32 vblank_def;
	s64 h_blank;

	mutex_lock(&ov13855->mutex);

	/* Only one raw bayer(GRBG) order is supported */
	//if (fmt->format.code != OV13855_MEDIA_BUS_FMT)
		fmt->format.code = OV13855_MEDIA_BUS_FMT;

	mode = ov13855_find_best_fit(ov13855, fmt);
	ov13855_update_pad_format(mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
		*framefmt = fmt->format;
	} else {
		ov13855->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(ov13855->link_freq, mode->link_freq_index);
		__v4l2_ctrl_s_ctrl_int64(
			ov13855->pixel_rate,
			link_freq_configs[mode->link_freq_index].pixel_rate);
		/* Update limits and set FPS to default */
		vblank_def = ov13855->cur_mode->vts - ov13855->cur_mode->height;
		__v4l2_ctrl_modify_range(
			ov13855->vblank, OV13855_VBLANK_MIN,
			OV13855_VTS_MAX - ov13855->cur_mode->height, 1,
			vblank_def);
		__v4l2_ctrl_s_ctrl(ov13855->vblank, vblank_def);
		h_blank =
			link_freq_configs[mode->link_freq_index].pixels_per_line
			 - ov13855->cur_mode->width;
		__v4l2_ctrl_modify_range(ov13855->hblank, h_blank,
					 h_blank, 1, h_blank);
	}

	mutex_unlock(&ov13855->mutex);

	return 0;
}

static int ov13855_get_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	*frames = OV13855_NUM_OF_SKIP_FRAMES;

	return 0;
}

/* Start streaming */
static int ov13855_start_streaming(struct ov13855 *ov13855)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	const struct ov13855_reg_list *reg_list;
	int ret, link_freq_index;

printk(KERN_ERR "%s() :%d\n", __func__, __LINE__);

	/* Get out of software reset */
	ret = ov13855_write_reg(ov13855, OV13855_REG_SOFTWARE_RST,
				OV13855_REG_VALUE_08BIT, OV13855_SOFTWARE_RST);
	if (ret) {
		dev_err(&client->dev, "%s failed to issue software reset: %d\n",
			__func__, ret);
		return ret;
	}

#if 1
	ret = ov13855_write_regs(ov13855, init_setting,
				 ARRAY_SIZE(init_setting));
	if (ret) {
		dev_err(&client->dev, "%s failed to send init sequence: %d\n",
			__func__, ret);
		return ret;
	}
#endif

#if 0
	/* Setup PLL */
	link_freq_index = ov13855->cur_mode->link_freq_index;
	reg_list = &link_freq_configs[link_freq_index].reg_list;
	ret = ov13855_write_reg_list(ov13855, reg_list);
	if (ret) {
		dev_err(&client->dev, "%s failed to set plls\n", __func__);
		return ret;
	}
#endif

	/* Apply default values of current mode */
	reg_list = &ov13855->cur_mode->reg_list;
	ret = ov13855_write_reg_list(ov13855, reg_list);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

printk(KERN_ERR "%s(): %d regs, [0] %04x -> %02x\n", __func__,
		reg_list->num_of_regs,
		reg_list->regs[0].address,
		reg_list->regs[0].val);

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(ov13855->sd.ctrl_handler);
	if (ret)
		return ret;

	return ov13855_write_reg(ov13855, OV13855_REG_MODE_SELECT,
				 OV13855_REG_VALUE_08BIT,
				 OV13855_MODE_STREAMING);
}

/* Stop streaming */
static int ov13855_stop_streaming(struct ov13855 *ov13855)
{
	return ov13855_write_reg(ov13855, OV13855_REG_MODE_SELECT,
				 OV13855_REG_VALUE_08BIT, OV13855_MODE_STANDBY);
}

static int ov13855_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

printk(KERN_ERR "%s() :: %d\n", __func__, enable);

	mutex_lock(&ov13855->mutex);
	if (ov13855->streaming == enable) {
		mutex_unlock(&ov13855->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto err_unlock;
		}

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = ov13855_start_streaming(ov13855);
		if (ret)
			goto err_rpm_put;
	} else {
		ov13855_stop_streaming(ov13855);
		pm_runtime_put(&client->dev);
	}

	ov13855->streaming = enable;
	mutex_unlock(&ov13855->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&ov13855->mutex);

	return ret;
}

static int __maybe_unused ov13855_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13855 *ov13855 = to_ov13855(sd);

	if (ov13855->streaming)
		ov13855_stop_streaming(ov13855);

	return 0;
}

static int __maybe_unused ov13855_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13855 *ov13855 = to_ov13855(sd);
	int ret;

	if (ov13855->streaming) {
		ret = ov13855_start_streaming(ov13855);
		if (ret)
			goto error;
	}

	return 0;

error:
	ov13855_stop_streaming(ov13855);
	ov13855->streaming = 0;
	return ret;
}

/* Verify chip ID */
static int ov13855_identify_module(struct ov13855 *ov13855)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	int ret;
	u32 val;

	ret = ov13855_read_reg(ov13855, OV13855_REG_CHIP_ID,
			       OV13855_REG_VALUE_24BIT, &val);
	if (ret)
		return ret;

	if (val != OV13855_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			OV13855_CHIP_ID, val);
		return -EIO;
	}

	return 0;
}

static int ov13855_set_power(struct ov13855 *ov13855, bool on)
{
	int ret = 0;

	if (ov13855->reset_gpio)
		gpiod_set_value(ov13855->reset_gpio, 0);

	if (on) {
		ret = regulator_bulk_enable(OV13855_NUM_SUPPLIES,
					    ov13855->supplies);
		if (ret)
			goto xclk_off;

		if (ov13855->reset_gpio)
			gpiod_set_value(ov13855->reset_gpio, 1);

		udelay(5000);

		ret = clk_prepare_enable(ov13855->xclk);
		if (ret) {
			printk(KERN_ERR "%s(): clk_prepare_enable() failed with %d\n", __func__, ret);
			return ret;
		}

		usleep_range(10000, 20000);

#if 0
		ret = ov13855_restore_mode(ov13855);
		if (ret)
			goto power_off;

		/*
		 * start streaming briefly followed by stream off in
		 * order to coax the clock lane into LP-11 state.
		 */
		ret = ov13855_start_streaming(ov13855);
		if (ret)
			goto power_off;
		usleep_range(1000, 2000);

		ret = ov13855_stop_streaming(ov13855);
		if (ret)
			goto power_off;
#endif

		return 0;
	}

power_off:
	regulator_bulk_disable(OV13855_NUM_SUPPLIES, ov13855->supplies);
xclk_off:

	clk_disable_unprepare(ov13855->xclk);
	return ret;
}

static int ov13855_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	int ret = 0;

	mutex_lock(&ov13855->mutex);

	printk(KERN_ERR "XXXX %s() -- on %d, power_count %d\n", __func__, on, ov13855->power_count);

	/*
	 * If the power count is modified from 0 to != 0 or from != 0 to 0,
	 * update the power state.
	 */
	if (ov13855->power_count == !on) {
		ret = ov13855_set_power(ov13855, !!on);
		if (ret)
			goto out;
	}

	/* Update the power count. */
	ov13855->power_count += on ? 1 : -1;
	WARN_ON(ov13855->power_count < 0);
out:
	mutex_unlock(&ov13855->mutex);

#if 0
	if (on && !ret && ov13855->power_count == 1) {
		/* restore controls */
		ret = v4l2_ctrl_handler_setup(&ov13855->ctrls.handler);
	}
#endif

	return ret;
}

static int ov13855_get_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_frame_interval *fi)
{
	struct ov13855 *ov13855 = to_ov13855(sd);

	mutex_lock(&ov13855->mutex);
	fi->interval = ov13855->frame_interval;
	mutex_unlock(&ov13855->mutex);

	return 0;
}

static const struct v4l2_subdev_core_ops ov13855_core_ops = {
	.s_power = ov13855_s_power,
};

static const struct v4l2_subdev_video_ops ov13855_video_ops = {
	.g_frame_interval = ov13855_get_frame_interval,
	.s_stream = ov13855_set_stream,
};

static const struct v4l2_subdev_pad_ops ov13855_pad_ops = {
	.enum_mbus_code = ov13855_enum_mbus_code,
	.get_fmt = ov13855_get_pad_format,
	.set_fmt = ov13855_set_pad_format,
	.enum_frame_size = ov13855_enum_frame_size,
};

static const struct v4l2_subdev_sensor_ops ov13855_sensor_ops = {
	.g_skip_frames = ov13855_get_skip_frames,
};

static const struct v4l2_subdev_ops ov13855_subdev_ops = {
	.core = &ov13855_core_ops,
	.video = &ov13855_video_ops,
	.pad = &ov13855_pad_ops,
	.sensor = &ov13855_sensor_ops,
};

static const struct media_entity_operations ov13855_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops ov13855_internal_ops = {
	.open = ov13855_open,
};

/* Initialize control handlers */
static int ov13855_init_controls(struct ov13855 *ov13855)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov13855->sd);
	struct v4l2_ctrl_handler *ctrl_hdlr;
	s64 exposure_max;
	int ret;

	ctrl_hdlr = &ov13855->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	mutex_init(&ov13855->mutex);
	ctrl_hdlr->lock = &ov13855->mutex;

	ov13855->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
				&ov13855_ctrl_ops,
				V4L2_CID_LINK_FREQ,
				OV13855_NUM_OF_LINK_FREQS - 1,
				0,
				link_freq_menu_items);
	ov13855->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* By default, PIXEL_RATE is read only */
	ov13855->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &ov13855_ctrl_ops,
					V4L2_CID_PIXEL_RATE, 0,
					link_freq_configs[0].pixel_rate, 1,
					link_freq_configs[0].pixel_rate);

	ov13855->vblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &ov13855_ctrl_ops, V4L2_CID_VBLANK,
				OV13855_VBLANK_MIN,
				OV13855_VTS_MAX - ov13855->cur_mode->height, 1,
				ov13855->cur_mode->vts
				  - ov13855->cur_mode->height);

	ov13855->hblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &ov13855_ctrl_ops, V4L2_CID_HBLANK,
				OV13855_PPL_1080MHZ - ov13855->cur_mode->width,
				OV13855_PPL_1080MHZ - ov13855->cur_mode->width,
				1,
				OV13855_PPL_1080MHZ - ov13855->cur_mode->width);
	ov13855->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	exposure_max = ov13855->cur_mode->vts - 8;
	ov13855->exposure = v4l2_ctrl_new_std(
				ctrl_hdlr, &ov13855_ctrl_ops,
				V4L2_CID_EXPOSURE, OV13855_EXPOSURE_MIN,
				exposure_max, OV13855_EXPOSURE_STEP,
				OV13855_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &ov13855_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OV13855_ANA_GAIN_MIN, OV13855_ANA_GAIN_MAX,
			  OV13855_ANA_GAIN_STEP, OV13855_ANA_GAIN_DEFAULT);

#if 0
	/* Digital gain */
	v4l2_ctrl_new_std(ctrl_hdlr, &ov13855_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  OV13855_DGTL_GAIN_MIN, OV13855_DGTL_GAIN_MAX,
			  OV13855_DGTL_GAIN_STEP, OV13855_DGTL_GAIN_DEFAULT);
#endif

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &ov13855_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov13855_test_pattern_menu) - 1,
				     0, 0, ov13855_test_pattern_menu);
	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ov13855->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&ov13855->mutex);

	return ret;
}

static void ov13855_free_controls(struct ov13855 *ov13855)
{
	v4l2_ctrl_handler_free(ov13855->sd.ctrl_handler);
	mutex_destroy(&ov13855->mutex);
}

static int ov13855_get_regulators(struct ov13855 *ov13855)
{
	int i;

	for (i = 0; i < OV13855_NUM_SUPPLIES; i++)
		ov13855->supplies[i].supply = ov13855_supply_name[i];

	return devm_regulator_bulk_get(ov13855->sd.dev,
				       OV13855_NUM_SUPPLIES,
				       ov13855->supplies);
}


static int ov13855_probe(struct i2c_client *client,
			 const struct i2c_device_id *devid)
{
	struct ov13855 *ov13855;
	struct device_node *endpoint;
	struct device *dev = &client->dev;
	int ret;
	u32 val = 0;

	ov13855 = devm_kzalloc(dev, sizeof(*ov13855), GFP_KERNEL);
	if (!ov13855)
		return -ENOMEM;

	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_of_parse_endpoint(endpoint, &ov13855->ep);
	of_node_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	if (ov13855->ep.bus_type != V4L2_MBUS_CSI2) {
		dev_err(dev, "invalid bus type, must be MIPI CSI2\n");
		return -EINVAL;
	}

	/* get system clock (xclk) */
	ov13855->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(ov13855->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(ov13855->xclk);
	}

	ret = clk_set_rate(ov13855->xclk, 23800000ULL);
	if (ret) {
		dev_err(dev, "could not set xclk frequency\n");
		return ret;
	}

	dev_err(dev, "XCLK RATE :%lu\n", clk_get_rate(ov13855->xclk));

	/* request optional reset pin */
	ov13855->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_LOW);

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&ov13855->sd, client, &ov13855_subdev_ops);

	ret = ov13855_get_regulators(ov13855);
	if (ret)
		return ret;

	ov13855->frame_interval.numerator = 1;
	ov13855->frame_interval.denominator =
		ov13855_framerates[OV13855_30_FPS];

	/* Check module identity */
	ret = ov13855_identify_module(ov13855);
	if (ret) {
		dev_err(&client->dev, "failed to find sensor: %d\n", ret);
		//return ret;
	}

	/* Set default mode to max resolution */
	ov13855->cur_mode = &supported_modes[0];

	ret = ov13855_init_controls(ov13855);
	if (ret)
		return ret;

	/* Initialize subdev */
	ov13855->sd.internal_ops = &ov13855_internal_ops;
	ov13855->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov13855->sd.entity.ops = &ov13855_subdev_entity_ops;
	ov13855->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	ov13855->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov13855->sd.entity, 1, &ov13855->pad);
	if (ret) {
		dev_err(&client->dev, "%s failed:%d\n", __func__, ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev(&ov13855->sd);
	if (ret < 0)
		goto error_media_entity;

	/*
	 * Device is already turned on by i2c-core with ACPI domain PM.
	 * Enable runtime PM and turn off the device.
	 */
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_put(dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&ov13855->sd.entity);

error_handler_free:
	ov13855_free_controls(ov13855);
	dev_err(&client->dev, "%s failed:%d\n", __func__, ret);

	return ret;
}

static int ov13855_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13855 *ov13855 = to_ov13855(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	ov13855_free_controls(ov13855);

	/*
	 * Disable runtime PM but keep the device turned on.
	 * i2c-core with ACPI domain PM will turn off the device.
	 */
	pm_runtime_get_sync(&client->dev);
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);

	return 0;
}

static const struct i2c_device_id ov13855_id_table[] = {
	{"ov13855", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, ov13855_id_table);

static const struct of_device_id ov13855_dt_ids[] = {
	{ .compatible = "ovti,ov13855" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov13855_dt_ids);

static const struct dev_pm_ops ov13855_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ov13855_suspend, ov13855_resume)
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id ov13855_acpi_ids[] = {
	{"OVTID855"},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(acpi, ov13855_acpi_ids);
#endif

static struct i2c_driver ov13855_i2c_driver = {
	.driver = {
		.name = "ov13855",
		.owner = THIS_MODULE,
		.pm = &ov13855_pm_ops,
		.acpi_match_table = ACPI_PTR(ov13855_acpi_ids),
		.of_match_table	= ov13855_dt_ids,
	},
	.probe = ov13855_probe,
	.remove = ov13855_remove,
	.id_table = ov13855_id_table,
};

module_i2c_driver(ov13855_i2c_driver);

MODULE_DESCRIPTION("Omnivision ov13855 sensor driver");
MODULE_LICENSE("GPL v2");
