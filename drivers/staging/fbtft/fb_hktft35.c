// SPDX-License-Identifier: GPL-2.0+
/*
 * FB driver for the Hardkernel 3.5 inch TFT LCD
 * that uses the ILI9488 LCD Controller
 *
 * Copyright (C) 2019 Yang Deokgyu
 *
 * Based on fb_ili9340.c by Noralf Tronnes
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/gpio/consumer.h>
#include <linux/backlight.h>
#include <linux/delay.h>

#include "fbtft.h"

#define DRVNAME			"fb_hktft35"
#define WIDTH			320
#define HEIGHT			480

#define	ODROIDXU3_GPX1_REG	0x13400C24
#define	ODROIDXU3_GPX2_REG	0x13400C44
#define	ODROIDXU3_GPA2_REG	0x14010044

#define ODROID_TFT35_MACTL_MV	0x20
#define ODROID_TFT35_MACTL_MX	0x40
#define ODROID_TFT35_MACTL_MY	0x80

union reg_bitfield {
	unsigned int wvalue;
	struct {
		unsigned int bit0 : 1;
		unsigned int bit1 : 1;
		unsigned int bit2 : 1;
		unsigned int bit3 : 1;
		unsigned int bit4 : 1;
		unsigned int bit5 : 1;
		unsigned int bit6 : 1;
		unsigned int bit7 : 1;
		unsigned int bit8_bit31 : 24;
	} bits;
};

volatile void __iomem *reg_gpx1;
volatile void __iomem *reg_gpx2;
volatile void __iomem *reg_gpa2;

/* this init sequence matches Hardkernel 3.5 inch TFT LCD */
static const s16 default_init_sequence[] = {
	-1, 0xB0,0x00,
	-1, 0x11,
	-2, 120,
	-1, 0x3A,0x55,
	-1, 0xC2,0x33,
	-1, 0xC5,0x00,0x1E,0x80,
	-1, 0x36,0x28,
	-1, 0xB1,0xB0,
	-1, 0xE0,0x00,0x04,0x0E,0x08,0x17,0x0A,0x40,0x79,0x4D,0x07,0x0E,0x0A,0x1A,0x1D,0x0F,
	-1, 0xE1,0x00,0x1B,0x1F,0x02,0x10,0x05,0x32,0x34,0x43,0x02,0x0A,0x09,0x33,0x37,0x0F,
	-1, 0x11,
	-1, 0x29,
	-3
};

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	fbtft_par_dbg(DEBUG_SET_ADDR_WIN, par,
		"%s(xs=%d, ys=%d, xe=%d, ye=%d)\n", __func__, xs, ys, xe, ye);

	/* Column address */
	write_reg(par, 0x2A, xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF);

	/* Row adress */
	write_reg(par, 0x2B, ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF);

	/* Memory write */
	write_reg(par, 0x2C);
}

static int set_var(struct fbtft_par *par)
{
	u8 val;

	switch (par->info->var.rotate) {
	case 270:
		val = ODROID_TFT35_MACTL_MV;
		break;
	case 180:
		val = ODROID_TFT35_MACTL_MY;
		break;
	case 90:
		val = ODROID_TFT35_MACTL_MV | ODROID_TFT35_MACTL_MX | ODROID_TFT35_MACTL_MY;
		break;
	default:
		val = ODROID_TFT35_MACTL_MX;
		break;
	}
	/* Memory Access Control  */
	write_reg(par, 0x36, val | (par->bgr << 3));
	return 0;
}

static int fbtft_backlight_get_brightness(struct backlight_device *bd)
{
	return bd->props.brightness;
}

static int fbtft_backlight_update_status(struct backlight_device *bd)
{
	struct fbtft_par *par = bl_get_data(bd);
	bool polarity = par->polarity;

	fbtft_par_dbg(DEBUG_BACKLIGHT, par,
		      "%s: polarity=%d, power=%d, fb_blank=%d\n",
		      __func__, polarity, bd->props.power, bd->props.fb_blank);

	if ((bd->props.power == FB_BLANK_UNBLANK) &&
	    (bd->props.fb_blank == FB_BLANK_UNBLANK))
		gpiod_set_value(par->gpio.led[0], polarity);
	else
		gpiod_set_value(par->gpio.led[0], !polarity);

	return 0;
}

static const struct backlight_ops fbtft_bl_ops = {
	.get_brightness	= fbtft_backlight_get_brightness,
	.update_status	= fbtft_backlight_update_status,
};

static void register_backlight(struct fbtft_par *par)
{
	struct backlight_device *bd;
	struct backlight_properties bl_props = { 0, };

	if (!par->gpio.led[0]) {
		fbtft_par_dbg(DEBUG_BACKLIGHT, par,
			      "%s(): led pin not set, exiting.\n", __func__);
		return;
	}

	bl_props.type = BACKLIGHT_RAW;
	/* Assume backlight is off, get polarity from current state of pin */
	bl_props.power = FB_BLANK_POWERDOWN;

	/* Force polarity to true */
	par->polarity = true;

	bd = backlight_device_register(dev_driver_string(par->info->device),
				       par->info->device, par,
				       &fbtft_bl_ops, &bl_props);
	if (IS_ERR(bd)) {
		dev_err(par->info->device,
			"cannot register backlight device (%ld)\n",
			PTR_ERR(bd));
		return;
	}
	par->info->bl_dev = bd;

	if (!par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight = fbtft_unregister_backlight;
}

static void unregister_backlight(struct fbtft_par *par)
{
	if (par->info->bl_dev) {
		par->info->bl_dev->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(par->info->bl_dev);
		backlight_device_unregister(par->info->bl_dev);
		par->info->bl_dev = NULL;
	}

	/* Just to hook the remove routine */
	if (reg_gpx1) iounmap(reg_gpx1);
	if (reg_gpx2) iounmap(reg_gpx2);
	if (reg_gpa2) iounmap(reg_gpa2);
}

static int verify_gpios(struct fbtft_par *par)
{
	struct fbtft_platform_data *pdata = par->pdata;
	int i;

	fbtft_par_dbg(DEBUG_VERIFY_GPIOS, par, "%s()\n", __func__);

	if (pdata->display.buswidth != 9 &&  par->startbyte == 0 &&
	    !par->gpio.dc) {
		dev_err(par->info->device,
			"Missing info about 'dc' gpio. Aborting.\n");
		return -EINVAL;
	}

	if (!par->pdev)
		return 0;

	if (!par->gpio.wr) {
		dev_err(par->info->device, "Missing 'wr' gpio. Aborting.\n");
		return -EINVAL;
	}
	for (i = 0; i < pdata->display.buswidth; i++) {
		if (!par->gpio.db[i]) {
			dev_err(par->info->device,
				"Missing 'db%02d' gpio. Aborting.\n", i);
			return -EINVAL;
		}
	}

	/* Just to hook the probe routine */
	reg_gpx1 = ioremap(ODROIDXU3_GPX1_REG, 4);
	reg_gpx2 = ioremap(ODROIDXU3_GPX2_REG, 4);
	reg_gpa2 = ioremap(ODROIDXU3_GPA2_REG, 4);
	if ((reg_gpx1 == NULL) || (reg_gpx2 == NULL) || (reg_gpa2 == NULL)) {
		pr_err("%s : ioremap gpio registers error!\n", __func__);
	} else {
		pr_info("%s : ioremap gpio registers success!\n", __func__);
	}

	return 0;
}

static void reset(struct fbtft_par *par)
{
	if (!par->gpio.reset)
		return;
	fbtft_par_dbg(DEBUG_RESET, par, "%s()\n", __func__);
	gpiod_set_value_cansleep(par->gpio.reset, 0);
	usleep_range(20, 40);
	gpiod_set_value_cansleep(par->gpio.reset, 1);
	msleep(120);
}

static int write(struct fbtft_par *par, void *buf, size_t len)
{
	u8 data;
	union reg_bitfield gpx1, gpx2, gpa2;

	if ((reg_gpx1 == NULL) || (reg_gpx2 == NULL) || (reg_gpa2 == NULL)) {
		pr_err("%s : ioremap gpio register fail!\n", __func__);
		return	0;
	}

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
			  "%s(len=%zu): ", __func__, len);

	gpx1.wvalue = ioread32(reg_gpx1);
	gpx2.wvalue = ioread32(reg_gpx2);
	gpa2.wvalue = ioread32(reg_gpa2);

	while (len--) {
		data = *(u8 *) buf;
		gpx1.bits.bit7 = (data & 0x01) ? 1 : 0;
		gpx2.bits.bit0 = (data & 0x02) ? 1 : 0;
		gpx1.bits.bit3 = (data & 0x04) ? 1 : 0;
		gpa2.bits.bit4 = (data & 0x08) ? 1 : 0;
		gpa2.bits.bit6 = (data & 0x10) ? 1 : 0;
		gpa2.bits.bit7 = (data & 0x20) ? 1 : 0;
		gpx1.bits.bit6 = (data & 0x40) ? 1 : 0;
		gpx1.bits.bit5 = (data & 0x80) ? 1 : 0;
		/* Start writing by pulling down /WR */
		gpa2.bits.bit5 = 0;
		iowrite32(gpx1.wvalue, reg_gpx1);
		iowrite32(gpx2.wvalue, reg_gpx2);
		iowrite32(gpa2.wvalue, reg_gpa2);
		gpa2.bits.bit5 = 1;
		iowrite32(gpa2.wvalue, reg_gpa2);

		buf++;
	}

	return 0;
}

static struct fbtft_display display = {
	.regwidth = 8,
	.buswidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
	.init_sequence = default_init_sequence,
	.fbtftops = {
		.set_addr_win = set_addr_win,
		.set_var = set_var,
		.verify_gpios = verify_gpios,
		.register_backlight = register_backlight,
		.unregister_backlight = unregister_backlight,
		.reset = reset,
		.write = write,
	},
};
FBTFT_REGISTER_DRIVER(DRVNAME, "odroid,hktft35", &display);

MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("platform:hktft35");

MODULE_DESCRIPTION("FB driver for the Hardkernel 3.5 inch TFT LCD uses the ILI9488 LCD Controller");
MODULE_AUTHOR("Yang Deokgyu");
MODULE_LICENSE("GPL");
