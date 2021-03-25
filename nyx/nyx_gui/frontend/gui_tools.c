/*
 * Copyright (c) 2018 naehrwert
 * Copyright (c) 2018-2021 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#include "gui.h"
#include "gui_tools.h"
#include "gui_tools_partition_manager.h"
#include "gui_emmc_tools.h"
#include "fe_emummc_tools.h"
#include <memory_map.h>
#include "../config.h"
#include <display/di.h>
#include "../hos/pkg1.h"
#include "../hos/pkg2.h"
#include "../hos/hos.h"
#include "../hos/sept.h"
#include <input/touch.h>
#include <libs/fatfs/ff.h>
#include <mem/heap.h>
#include <mem/minerva.h>
#include <sec/se.h>
#include <soc/bpmp.h>
#include <soc/fuse.h>
#include "../storage/nx_emmc.h"
#include <storage/nx_sd.h>
#include <storage/sdmmc.h>
#include <usb/usbd.h>
#include <utils/btn.h>
#include <utils/sprintf.h>
#include <utils/util.h>

extern volatile boot_cfg_t *b_cfg;
extern hekate_config h_cfg;
extern nyx_config n_cfg;

extern char *emmcsn_path_impl(char *path, char *sub_dir, char *filename, sdmmc_storage_t *storage);

static lv_obj_t *_create_container(lv_obj_t *parent)
{
	static lv_style_t h_style;
	lv_style_copy(&h_style, &lv_style_transp);
	h_style.body.padding.inner = 0;
	h_style.body.padding.hor = LV_DPI - (LV_DPI / 4);
	h_style.body.padding.ver = LV_DPI / 6;

	lv_obj_t *h1 = lv_cont_create(parent, NULL);
	lv_cont_set_style(h1, &h_style);
	lv_cont_set_fit(h1, false, true);
	lv_obj_set_width(h1, (LV_HOR_RES / 9) * 4);
	lv_obj_set_click(h1, false);
	lv_cont_set_layout(h1, LV_LAYOUT_OFF);

	return h1;
}

bool get_autorcm_status(bool change)
{
	u8 corr_mod0, mod1;
	bool enabled = false;

	if (h_cfg.t210b01)
		return false;

	sdmmc_storage_init_mmc(&emmc_storage, &emmc_sdmmc, SDMMC_BUS_WIDTH_8, SDHCI_TIMING_MMC_HS400);

	u8 *tempbuf = (u8 *)malloc(0x200);
	sdmmc_storage_set_mmc_partition(&emmc_storage, EMMC_BOOT0);
	sdmmc_storage_read(&emmc_storage, 0x200 / NX_EMMC_BLOCKSIZE, 1, tempbuf);

	// Get the correct RSA modulus byte masks.
	nx_emmc_get_autorcm_masks(&corr_mod0, &mod1);

	// Check if 2nd byte of modulus is correct.
	if (tempbuf[0x11] != mod1)
		goto out;

	if (tempbuf[0x10] != corr_mod0)
		enabled = true;

	// Change autorcm status if requested.
	if (change)
	{
		int i, sect = 0;

		// Iterate BCTs.
		for (i = 0; i < 4; i++)
		{
			sect = (0x200 + (0x4000 * i)) / NX_EMMC_BLOCKSIZE;
			sdmmc_storage_read(&emmc_storage, sect, 1, tempbuf);

			if (!enabled)
				tempbuf[0x10] = 0;
			else
				tempbuf[0x10] = corr_mod0;
			sdmmc_storage_write(&emmc_storage, sect, 1, tempbuf);
		}
		enabled = !(enabled);
	}

out:
	free(tempbuf);
	sdmmc_storage_end(&emmc_storage);

	return enabled;
}

static lv_res_t _create_mbox_autorcm_status(lv_obj_t *btn)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char * mbox_btn_map[] = { "\211", "\222OK", "\211", "" };
	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	bool enabled = get_autorcm_status(true);

	if (enabled)
	{
		lv_mbox_set_text(mbox,
			"AutoRCM e' ora #C7EA46 ATTIVO!#\n\n"
			"Ora puoi entrare automaticamente in RCM premendo #FF8000 POWER#.\n"
			"Usa il tasto AutoRCM nuovamente se lo vorrai rimuovere.");
	}
	else
	{
		lv_mbox_set_text(mbox,
			"AutoRCM e' ora #FF8000 DISATTIVATO!#\n\n"
			"Il processo di avvio e' ora normale e dovrai premere #FF8000 VOL+# + #FF8000 HOME# (jig) per entrare in RCM.\n");
	}

	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	if (enabled)
		lv_btn_set_state(btn, LV_BTN_STATE_TGL_REL);
	else
		lv_btn_set_state(btn, LV_BTN_STATE_REL);
	nyx_generic_onoff_toggle(btn);

	return LV_RES_OK;
}

static lv_res_t _create_mbox_hid(usb_ctxt_t *usbs)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\211", "\262Chiudi", "\211", "" };
	static const char *mbox_btn_map2[] = { "\211", "\222Chiudi", "\211", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	char *txt_buf = malloc(0x1000);

	s_printf(txt_buf, "#FF8000 Emulazione HID#\n\n#C7EA46 Dispositivo:# ");

	if (usbs->type == USB_HID_GAMEPAD)
		strcat(txt_buf, "Gamepad");
	else
		strcat(txt_buf, "Touchpad");

	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_text(lbl_status, " ");
	usbs->label = (void *)lbl_status;

	lv_obj_t *lbl_tip = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_tip, true);
	lv_label_set_static_text(lbl_tip, "Nota: Per terminarlo, premi #C7EA46 L3# + #C7EA46 HOME# o rimuovi il cavo.");
	lv_obj_set_style(lbl_tip, &hint_small_style);

	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	usb_device_gadget_hid(usbs);

	lv_mbox_add_btns(mbox, mbox_btn_map2, mbox_action);

	return LV_RES_OK;
}

static lv_res_t _create_mbox_ums(usb_ctxt_t *usbs)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\211", "\262Chiudi", "\211", "" };
	static const char *mbox_btn_map2[] = { "\211", "\222Chiudi", "\211", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	char *txt_buf = malloc(0x1000);

	s_printf(txt_buf, "#FF8000 Archiviazione di Massa USB#\n\n#C7EA46 Dispositivo:# ");

	if (usbs->type == MMC_SD)
	{
		switch (usbs->partition)
		{
		case 0:
			strcat(txt_buf, "Scheda SD");
			break;
		case EMMC_GPP + 1:
			strcat(txt_buf, "emuMMC GPP");
			break;
		case EMMC_BOOT0 + 1:
			strcat(txt_buf, "emuMMC BOOT0");
			break;
		case EMMC_BOOT1 + 1:
			strcat(txt_buf, "emuMMC BOOT1");
			break;
		}
	}
	else
	{
		switch (usbs->partition)
		{
		case EMMC_GPP + 1:
			strcat(txt_buf, "eMMC GPP");
			break;
		case EMMC_BOOT0 + 1:
			strcat(txt_buf, "eMMC BOOT0");
			break;
		case EMMC_BOOT1 + 1:
			strcat(txt_buf, "eMMC BOOT1");
			break;
		}
	}

	lv_mbox_set_text(mbox, txt_buf);
	free(txt_buf);

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_text(lbl_status, " ");
	usbs->label = (void *)lbl_status;

	lv_obj_t *lbl_tip = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_tip, true);
	if (!usbs->ro)
	{
		if (usbs->type == MMC_SD)
		{
			lv_label_set_static_text(lbl_tip,
				"Nota: Per terminarlo, fai la #C7EA46 rimozione sicura# da dentro il SO.\n"
				"       #FFDD00 NON rimuovere il cavo!#");
		}
		else
		{
			lv_label_set_static_text(lbl_tip,
				"Nota: Per terminarlo, fai la #C7EA46 rimozione sicura# da dentro il SO.\n"
				"       #FFDD00 Se non e' montata, dovresti aver bisogno di rimuovere il cavo!#");
		}
	}
	else
	{
		lv_label_set_static_text(lbl_tip,
			"Nota: Per terminarlo, fai la #C7EA46 rimozione sicura# da dentro il SO.\n"
			"       o rimuovi il cavo!#");
	}
	lv_obj_set_style(lbl_tip, &hint_small_style);

	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	// Dim backlight.
	display_backlight_brightness(20, 1000);

	usb_device_gadget_ums(usbs);

	// Restore backlight.
	display_backlight_brightness(h_cfg.backlight - 20, 1000);

	lv_mbox_add_btns(mbox, mbox_btn_map2, mbox_action);

	ums_mbox = dark_bg;

	return LV_RES_OK;
}

static lv_res_t _create_mbox_ums_error(int error)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\211", "\222OK", "\211", "" };
	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	switch (error)
	{
	case 1:
		lv_mbox_set_text(mbox, "#FF8000 Archiviazione di Massa USB#\n\n#FFFF00 Montaggio scheda SD fallito!#");
		break;
	case 2:
		lv_mbox_set_text(mbox, "#FF8000 Archiviazione di Massa USB#\n\n#FFFF00 Non sono state trovate emuMMC attive!#");
		break;
	case 3:
		lv_mbox_set_text(mbox, "#FF8000 Archiviazione di Massa USB#\n\n#FFFF00 La emuMMC attiva non e' a partizione!#");
		break;
	}

	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 5);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return LV_RES_OK;
}

static void usb_gadget_set_text(void *lbl, const char *text)
{
	lv_label_set_text((lv_obj_t *)lbl, text);
	manual_system_maintenance(true);
}

static lv_res_t _action_hid_jc(lv_obj_t *btn)
{
	// Reduce BPMP, RAM and backlight and power off SDMMC1 to conserve power.
	sd_end();
	minerva_change_freq(FREQ_800);
	bpmp_clk_rate_set(BPMP_CLK_NORMAL);
	display_backlight_brightness(10, 1000);

	usb_ctxt_t usbs;
	usbs.type = USB_HID_GAMEPAD;
	usbs.system_maintenance = &manual_system_maintenance;
	usbs.set_text = &usb_gadget_set_text;

	_create_mbox_hid(&usbs);

	// Restore BPMP, RAM and backlight.
	minerva_change_freq(FREQ_1600);
	bpmp_clk_rate_set(BPMP_CLK_DEFAULT_BOOST);
	display_backlight_brightness(h_cfg.backlight - 20, 1000);

	return LV_RES_OK;
}

/*
static lv_res_t _action_hid_touch(lv_obj_t *btn)
{
	// Reduce BPMP, RAM and backlight and power off SDMMC1 to conserve power.
	sd_end();
	minerva_change_freq(FREQ_800);
	bpmp_clk_rate_set(BPMP_CLK_NORMAL);
	display_backlight_brightness(10, 1000);

	usb_ctxt_t usbs;
	usbs.type = USB_HID_TOUCHPAD;
	usbs.system_maintenance = &manual_system_maintenance;
	usbs.set_text = &usb_gadget_set_text;

	_create_mbox_hid(&usbs);

	// Restore BPMP, RAM and backlight.
	minerva_change_freq(FREQ_1600);
	bpmp_clk_rate_set(BPMP_CLK_DEFAULT_BOOST);
	display_backlight_brightness(h_cfg.backlight - 20, 1000);

	return LV_RES_OK;
}
*/

static bool usb_msc_emmc_read_only;
lv_res_t action_ums_sd(lv_obj_t *btn)
{
	usb_ctxt_t usbs;
	usbs.type = MMC_SD;
	usbs.partition = 0;
	usbs.offset = 0;
	usbs.sectors = 0;
	usbs.ro = 0;
	usbs.system_maintenance = &manual_system_maintenance;
	usbs.set_text = &usb_gadget_set_text;

	_create_mbox_ums(&usbs);

	return LV_RES_OK;
}

static lv_res_t _action_ums_emmc_boot0(lv_obj_t *btn)
{
	if (!nyx_emmc_check_battery_enough())
		return LV_RES_OK;

	usb_ctxt_t usbs;
	usbs.type = MMC_EMMC;
	usbs.partition = EMMC_BOOT0 + 1;
	usbs.offset = 0;
	usbs.sectors = 0x2000;
	usbs.ro = usb_msc_emmc_read_only;
	usbs.system_maintenance = &manual_system_maintenance;
	usbs.set_text = &usb_gadget_set_text;

	_create_mbox_ums(&usbs);

	return LV_RES_OK;
}

static lv_res_t _action_ums_emmc_boot1(lv_obj_t *btn)
{
	if (!nyx_emmc_check_battery_enough())
		return LV_RES_OK;

	usb_ctxt_t usbs;
	usbs.type = MMC_EMMC;
	usbs.partition = EMMC_BOOT1 + 1;
	usbs.offset = 0;
	usbs.sectors = 0x2000;
	usbs.ro = usb_msc_emmc_read_only;
	usbs.system_maintenance = &manual_system_maintenance;
	usbs.set_text = &usb_gadget_set_text;

	_create_mbox_ums(&usbs);

	return LV_RES_OK;
}

static lv_res_t _action_ums_emmc_gpp(lv_obj_t *btn)
{
	if (!nyx_emmc_check_battery_enough())
		return LV_RES_OK;

	usb_ctxt_t usbs;
	usbs.type = MMC_EMMC;
	usbs.partition = EMMC_GPP + 1;
	usbs.offset = 0;
	usbs.sectors = 0;
	usbs.ro = usb_msc_emmc_read_only;
	usbs.system_maintenance = &manual_system_maintenance;
	usbs.set_text = &usb_gadget_set_text;

	_create_mbox_ums(&usbs);

	return LV_RES_OK;
}

static lv_res_t _action_ums_emuemmc_boot0(lv_obj_t *btn)
{
	if (!nyx_emmc_check_battery_enough())
		return LV_RES_OK;

	usb_ctxt_t usbs;

	int error = !sd_mount();
	if (!error)
	{
		emummc_cfg_t emu_info;
		load_emummc_cfg(&emu_info);

		error = 2;
		if (emu_info.enabled)
		{
			error = 3;
			if (emu_info.sector)
			{
				error = 0;
				usbs.offset = emu_info.sector;
			}
		}
	}
	sd_unmount();

	if (error)
		_create_mbox_ums_error(error);
	else
	{
		usbs.type = MMC_SD;
		usbs.partition = EMMC_BOOT0 + 1;
		usbs.sectors = 0x2000;
		usbs.ro = usb_msc_emmc_read_only;
		usbs.system_maintenance = &manual_system_maintenance;
		usbs.set_text = &usb_gadget_set_text;
		_create_mbox_ums(&usbs);
	}

	return LV_RES_OK;
}

static lv_res_t _action_ums_emuemmc_boot1(lv_obj_t *btn)
{
	if (!nyx_emmc_check_battery_enough())
		return LV_RES_OK;

	usb_ctxt_t usbs;

	int error = !sd_mount();
	if (!error)
	{
		emummc_cfg_t emu_info;
		load_emummc_cfg(&emu_info);

		error = 2;
		if (emu_info.enabled)
		{
			error = 3;
			if (emu_info.sector)
			{
				error = 0;
				usbs.offset = emu_info.sector + 0x2000;
			}
		}
	}
	sd_unmount();

	if (error)
		_create_mbox_ums_error(error);
	else
	{
		usbs.type = MMC_SD;
		usbs.partition = EMMC_BOOT1 + 1;
		usbs.sectors = 0x2000;
		usbs.ro = usb_msc_emmc_read_only;
		usbs.system_maintenance = &manual_system_maintenance;
		usbs.set_text = &usb_gadget_set_text;
		_create_mbox_ums(&usbs);
	}

	return LV_RES_OK;
}

static lv_res_t _action_ums_emuemmc_gpp(lv_obj_t *btn)
{
	if (!nyx_emmc_check_battery_enough())
		return LV_RES_OK;

	usb_ctxt_t usbs;

	int error = !sd_mount();
	if (!error)
	{
		emummc_cfg_t emu_info;
		load_emummc_cfg(&emu_info);

		error = 2;
		if (emu_info.enabled)
		{
			error = 3;
			if (emu_info.sector)
			{
				error = 1;
				usbs.offset = emu_info.sector + 0x4000;

				u8 *gpt = malloc(512);
				if (sdmmc_storage_read(&sd_storage, usbs.offset + 1, 1, gpt))
				{
					if (!memcmp(gpt, "EFI PART", 8))
					{
						error = 0;
						usbs.sectors = *(u32 *)(gpt + 0x20) + 1; // Backup LBA + 1.
					}
				}
			}
		}
	}
	sd_unmount();

	if (error)
		_create_mbox_ums_error(error);
	else
	{
		usbs.type = MMC_SD;
		usbs.partition = EMMC_GPP + 1;
		usbs.ro = usb_msc_emmc_read_only;
		usbs.system_maintenance = &manual_system_maintenance;
		usbs.set_text = &usb_gadget_set_text;
		_create_mbox_ums(&usbs);
	}

	return LV_RES_OK;
}

void nyx_run_ums(void *param)
{
	u32 *cfg = (u32 *)param;

	u8 type = (*cfg) >> 24;
	*cfg = *cfg & (~NYX_CFG_EXTRA);

	// Disable read only flag.
	usb_msc_emmc_read_only = false;

	switch (type)
	{
	case NYX_UMS_SD_CARD:
		action_ums_sd(NULL);
		break;
	case NYX_UMS_EMMC_BOOT0:
		_action_ums_emmc_boot0(NULL);
		break;
	case NYX_UMS_EMMC_BOOT1:
		_action_ums_emmc_boot1(NULL);
		break;
	case NYX_UMS_EMMC_GPP:
		_action_ums_emmc_gpp(NULL);
		break;
	case NYX_UMS_EMUMMC_BOOT0:
		_action_ums_emuemmc_boot0(NULL);
		break;
	case NYX_UMS_EMUMMC_BOOT1:
		_action_ums_emuemmc_boot1(NULL);
		break;
	case NYX_UMS_EMUMMC_GPP:
		_action_ums_emuemmc_gpp(NULL);
		break;
	}
}

static lv_res_t _emmc_read_only_toggle(lv_obj_t *btn)
{
	nyx_generic_onoff_toggle(btn);

	usb_msc_emmc_read_only = lv_btn_get_state(btn) & LV_BTN_STATE_TGL_REL ? 1 : 0;

	return LV_RES_OK;
}

static lv_res_t _create_window_usb_tools(lv_obj_t *parent)
{
	lv_obj_t *win = nyx_create_standard_window(SYMBOL_USB" Strumenti USB");

	static lv_style_t h_style;
	lv_style_copy(&h_style, &lv_style_transp);
	h_style.body.padding.inner = 0;
	h_style.body.padding.hor = LV_DPI - (LV_DPI / 4);
	h_style.body.padding.ver = LV_DPI / 9;

	// Create USB Mass Storage container.
	lv_obj_t *h1 = lv_cont_create(win, NULL);
	lv_cont_set_style(h1, &h_style);
	lv_cont_set_fit(h1, false, true);
	lv_obj_set_width(h1, (LV_HOR_RES / 9) * 5);
	lv_obj_set_click(h1, false);
	lv_cont_set_layout(h1, LV_LAYOUT_OFF);

	lv_obj_t *label_sep = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_txt, "Archiviazione di Massa USB");
	lv_obj_set_style(label_txt, lv_theme_get_current()->label.prim);
	lv_obj_align(label_txt, label_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, -LV_DPI * 3 / 10);

	lv_obj_t *line_sep = lv_line_create(h1, NULL);
	static const lv_point_t line_pp[] = { {0, 0}, { LV_HOR_RES - (LV_DPI - (LV_DPI / 4)) * 2, 0} };
	lv_line_set_points(line_sep, line_pp, 2);
	lv_line_set_style(line_sep, lv_theme_get_current()->line.decor);
	lv_obj_align(line_sep, label_txt, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create UMS buttons.
	lv_obj_t *btn1 = lv_btn_create(h1, NULL);
	lv_obj_t *label_btn = lv_label_create(btn1, NULL);
	lv_btn_set_fit(btn1, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_SD"  Scheda SD");

	lv_obj_align(btn1, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn1, LV_BTN_ACTION_CLICK, action_ums_sd);

	lv_obj_t *label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di montare la scheda SD a un PC/Telefono.\n"
		"#C7EA46 Sono supportati tutti i Sistemi Operativi. L'accesso e'# #FF8000 Lettura/Scrittura.#");

	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn1, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create RAW GPP button.
	lv_obj_t *btn_gpp = lv_btn_create(h1, btn1);
	label_btn = lv_label_create(btn_gpp, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_CHIP"  eMMC RAW GPP");
	lv_obj_align(btn_gpp, label_txt2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn_gpp, LV_BTN_ACTION_CLICK, _action_ums_emmc_gpp);

	lv_obj_t *btn_boot0 = lv_btn_create(h1, btn1);
	label_btn = lv_label_create(btn_boot0, NULL);
	lv_label_set_static_text(label_btn, "BOOT0");
	lv_obj_align(btn_boot0, btn_gpp, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 10, 0);
	lv_btn_set_action(btn_boot0, LV_BTN_ACTION_CLICK, _action_ums_emmc_boot0);

	lv_obj_t *btn_boot1 = lv_btn_create(h1, btn1);
	label_btn = lv_label_create(btn_boot1, NULL);
	lv_label_set_static_text(label_btn, "BOOT1");
	lv_obj_align(btn_boot1, btn_boot0, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 10, 0);
	lv_btn_set_action(btn_boot1, LV_BTN_ACTION_CLICK, _action_ums_emmc_boot1);

	lv_obj_t *btn_emu_gpp = lv_btn_create(h1, btn1);
	label_btn = lv_label_create(btn_emu_gpp, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_MODULES_ALT"  emu RAW GPP");
	lv_obj_align(btn_emu_gpp, btn_gpp, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn_emu_gpp, LV_BTN_ACTION_CLICK, _action_ums_emuemmc_gpp);

	lv_obj_t *btn_emu_boot0 = lv_btn_create(h1, btn1);
	label_btn = lv_label_create(btn_emu_boot0, NULL);
	lv_label_set_static_text(label_btn, "BOOT0");
	lv_obj_align(btn_emu_boot0, btn_boot0, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn_emu_boot0, LV_BTN_ACTION_CLICK, _action_ums_emuemmc_boot0);

	lv_obj_t *btn_emu_boot1 = lv_btn_create(h1, btn1);
	label_btn = lv_label_create(btn_emu_boot1, NULL);
	lv_label_set_static_text(label_btn, "BOOT1");
	lv_obj_align(btn_emu_boot1, btn_boot1, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn_emu_boot1, LV_BTN_ACTION_CLICK, _action_ums_emuemmc_boot1);

	label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di montare la eMMC/emuMMC.\n"
		"#C7EA46 L'accesso di default e'# #FF8000 sola lettura.#");
	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn_emu_gpp, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	lv_obj_t *h_write = lv_cont_create(win, NULL);
	lv_cont_set_style(h_write, &h_style);
	lv_cont_set_fit(h_write, false, true);
	lv_obj_set_width(h_write, (LV_HOR_RES / 9) * 2);
	lv_obj_set_click(h_write, false);
	lv_cont_set_layout(h_write, LV_LAYOUT_OFF);
	lv_obj_align(h_write, label_txt2, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 10, 0);

	lv_obj_t *btn_write_access = lv_btn_create(h_write, NULL);
	nyx_create_onoff_button(lv_theme_get_current(), h_write,
		btn_write_access, SYMBOL_EDIT" Sola Lettura", _emmc_read_only_toggle, false);
	if (!n_cfg.ums_emmc_rw)
		lv_btn_set_state(btn_write_access, LV_BTN_STATE_TGL_REL);
	_emmc_read_only_toggle(btn_write_access);

	// Create USB Input Devices container.
	lv_obj_t *h2 = lv_cont_create(win, NULL);
	lv_cont_set_style(h2, &h_style);
	lv_cont_set_fit(h2, false, true);
	lv_obj_set_width(h2, (LV_HOR_RES / 9) * 3);
	lv_obj_set_click(h2, false);
	lv_cont_set_layout(h2, LV_LAYOUT_OFF);
	lv_obj_align(h2, h1, LV_ALIGN_OUT_RIGHT_TOP, LV_DPI * 17 / 29, 0);

	label_sep = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt3 = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_txt3, "Dispositivi di Input USB");
	lv_obj_set_style(label_txt3, lv_theme_get_current()->label.prim);
	lv_obj_align(label_txt3, label_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, -LV_DPI * 4 / 21);

	line_sep = lv_line_create(h2, line_sep);
	lv_obj_align(line_sep, label_txt3, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create Gamepad button.
	lv_obj_t *btn3 = lv_btn_create(h2, NULL);
	label_btn = lv_label_create(btn3, NULL);
	lv_btn_set_fit(btn3, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_CIRCUIT"  Gamepad");
	lv_obj_align(btn3, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn3, LV_BTN_ACTION_CLICK, _action_hid_jc);

	lv_obj_t *label_txt4 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt4, true);
	lv_label_set_static_text(label_txt4,
		"Inserisci i Joy-Con e converti il dispositivo\n"
		"in un gamepad per PC/Telefono.\n"
		"#C7EA46 Richiede entrambi i Joy-Con per funzionare.#");

	lv_obj_set_style(label_txt4, &hint_small_style);
	lv_obj_align(label_txt4, btn3, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);
/*
	// Create Touchpad button.
	lv_obj_t *btn4 = lv_btn_create(h2, btn1);
	label_btn = lv_label_create(btn4, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_KEYBOARD"  Touchpad");
	lv_obj_align(btn4, label_txt4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn4, LV_BTN_ACTION_CLICK, _action_hid_touch);
	lv_btn_set_state(btn4, LV_BTN_STATE_INA);

	label_txt4 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt4, true);
	lv_label_set_static_text(label_txt4,
		"Control the PC via the device\'s touchscreen.\n"
		"#C7EA46 Two fingers tap acts like a# #FF8000 Right click##C7EA46 .#\n");
	lv_obj_set_style(label_txt4, &hint_small_style);
	lv_obj_align(label_txt4, btn4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);
*/
	return LV_RES_OK;
}

static int _fix_attributes(lv_obj_t *lb_val, char *path, u32 *total)
{
	FRESULT res;
	DIR dir;
	u32 dirLength = 0;
	static FILINFO fno;

	// Open directory.
	res = f_opendir(&dir, path);
	if (res != FR_OK)
		return res;

	dirLength = strlen(path);
	for (;;)
	{
		// Clear file or folder path.
		path[dirLength] = 0;

		// Read a directory item.
		res = f_readdir(&dir, &fno);

		// Break on error or end of dir.
		if (res != FR_OK || fno.fname[0] == 0)
			break;

		// Set new directory or file.
		memcpy(&path[dirLength], "/", 1);
		strcpy(&path[dirLength + 1], fno.fname);

		// Is it a directory?
		if (fno.fattrib & AM_DIR)
		{
			// Check if it's a HOS single file folder.
			strcat(path, "/00");
			bool is_hos_special = !f_stat(path, NULL);
			path[strlen(path) - 3] = 0;

			// Set archive bit to HOS single file folders.
			if (is_hos_special)
			{
				if (!(fno.fattrib & AM_ARC))
				{
					total[0]++;
					f_chmod(path, AM_ARC, AM_ARC);
				}
			}
			else if (fno.fattrib & AM_ARC) // If not, clear the archive bit.
			{
				total[1]++;
				f_chmod(path, 0, AM_ARC);
			}

			lv_label_set_text(lb_val, path);
			manual_system_maintenance(true);

			// Enter the directory.
			res = _fix_attributes(lb_val, path, total);
			if (res != FR_OK)
				break;
		}
	}

	f_closedir(&dir);

	return res;
}

static lv_res_t _create_window_unset_abit_tool(lv_obj_t *btn)
{
	lv_obj_t *win = nyx_create_standard_window(SYMBOL_COPY" Sistema Bit Archiviazione (Tutte le cartelle)");

	// Disable buttons.
	nyx_window_toggle_buttons(win, true);

	lv_obj_t *desc = lv_cont_create(win, NULL);
	lv_obj_set_size(desc, LV_HOR_RES * 10 / 11, LV_VER_RES - (LV_DPI * 11 / 7) * 4);

	lv_obj_t * lb_desc = lv_label_create(desc, NULL);
	lv_label_set_long_mode(lb_desc, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(lb_desc, true);

	if (!sd_mount())
	{
		lv_label_set_text(lb_desc, "#FFDD00 Inizializzazione SD fallita!#");
		lv_obj_set_width(lb_desc, lv_obj_get_width(desc));
	}
	else
	{
		lv_label_set_text(lb_desc, "#00DDFF Scorrendo tutti i file sulla scheda SD!#\nPotrebbe richiedere un po'...");
		lv_obj_set_width(lb_desc, lv_obj_get_width(desc));

		lv_obj_t *val = lv_cont_create(win, NULL);
		lv_obj_set_size(val, LV_HOR_RES * 10 / 11, LV_VER_RES - (LV_DPI * 11 / 7) * 4);

		lv_obj_t * lb_val = lv_label_create(val, lb_desc);

		char *path = malloc(1024);
		path[0] = 0;

		lv_label_set_text(lb_val, "");
		lv_obj_set_width(lb_val, lv_obj_get_width(val));
		lv_obj_align(val, desc, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

		u32 total[2] = { 0 };
		_fix_attributes(lb_val, path, total);

		sd_unmount();

		lv_obj_t *desc2 = lv_cont_create(win, NULL);
		lv_obj_set_size(desc2, LV_HOR_RES * 10 / 11, LV_VER_RES - (LV_DPI * 11 / 7) * 4);
		lv_obj_t * lb_desc2 = lv_label_create(desc2, lb_desc);

		char *txt_buf = (char *)malloc(0x500);

		s_printf(txt_buf, "#96FF00 Totale bit archiviazione sistemati:# #FF8000 %d resettati, %d settati!#", total[1], total[0]);

		lv_label_set_text(lb_desc2, txt_buf);
		lv_obj_set_width(lb_desc2, lv_obj_get_width(desc2));
		lv_obj_align(desc2, val, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 0);

		free(path);
	}

	// Enable buttons.
	nyx_window_toggle_buttons(win, false);

	return LV_RES_OK;
}

static lv_res_t _create_mbox_fix_touchscreen(lv_obj_t *btn)
{
	int res = 0;
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\211", "\222OK", "\211", "" };
	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	char *txt_buf = malloc(0x4000);
	strcpy(txt_buf, "#FF8000 Non toccare lo schermo!#\n\nLa messa a punto iniziera' in ");
	u32 text_idx = strlen(txt_buf);
	lv_mbox_set_text(mbox, txt_buf);

	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	lv_mbox_set_text(mbox,
		"#FFDD00 Avviso: Avvia questa opzione solo se hai problemi!#\n\n"
		"Premi #FF8000 POWER# per Continuare.\nPremi #FF8000 VOL# per annullare.");
	manual_system_maintenance(true);

	if (!(btn_wait() & BTN_POWER))
		goto out;

	manual_system_maintenance(true);
	lv_mbox_set_text(mbox, txt_buf);

	u32 seconds = 5;
	while (seconds)
	{
		s_printf(txt_buf + text_idx, "%d secondi...", seconds);
		lv_mbox_set_text(mbox, txt_buf);
		manual_system_maintenance(true);
		msleep(1000);
		seconds--;
	}

	u8 err[2];
	if (touch_panel_ito_test(err))
	{
		if (!err[0] && !err[1])
		{
			res = touch_execute_autotune();
			if (res)
				goto out;
		}
		else
		{
			touch_sense_enable();

			s_printf(txt_buf, "#FFFF00 Test ITO: ");
			switch (err[0])
			{
			case ITO_FORCE_OPEN:
				strcat(txt_buf, "Force Open");
				break;
			case ITO_SENSE_OPEN:
				strcat(txt_buf, "Sense Open");
				break;
			case ITO_FORCE_SHRT_GND:
				strcat(txt_buf, "Force Short to GND");
				break;
			case ITO_SENSE_SHRT_GND:
				strcat(txt_buf, "Sense Short to GND");
				break;
			case ITO_FORCE_SHRT_VCM:
				strcat(txt_buf, "Force Short to VDD");
				break;
			case ITO_SENSE_SHRT_VCM:
				strcat(txt_buf, "Sense Short to VDD");
				break;
			case ITO_FORCE_SHRT_FORCE:
				strcat(txt_buf, "Force Short to Force");
				break;
			case ITO_SENSE_SHRT_SENSE:
				strcat(txt_buf, "Sense Short to Sense");
				break;
			case ITO_F2E_SENSE:
				strcat(txt_buf, "Force Short to Sense");
				break;
			case ITO_FPC_FORCE_OPEN:
				strcat(txt_buf, "FPC Force Open");
				break;
			case ITO_FPC_SENSE_OPEN:
				strcat(txt_buf, "FPC Sense Open");
				break;
			default:
				strcat(txt_buf, "Unknown");
				break;

			}
			s_printf(txt_buf + strlen(txt_buf), " (%d), Chn: %d#\n\n", err[0], err[1]);
			strcat(txt_buf, "#FFFF00 La calibrazione del touchscreen e' fallita!");
			lv_mbox_set_text(mbox, txt_buf);
			goto out2;
		}
	}

	touch_sense_enable();

out:
	if (res)
		lv_mbox_set_text(mbox, "#C7EA46 Calibrazione touchscreen terminata!");
	else
		lv_mbox_set_text(mbox, "#FFFF00 La calibrazione del touchscreen e' fallita!");

out2:
	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);

	free(txt_buf);

	return LV_RES_OK;
}

static lv_res_t _create_window_dump_pk12_tool(lv_obj_t *btn)
{
	lv_obj_t *win = nyx_create_standard_window(SYMBOL_MODULES" Salva package1/2");

	// Disable buttons.
	nyx_window_toggle_buttons(win, true);

	lv_obj_t *desc = lv_cont_create(win, NULL);
	lv_obj_set_size(desc, LV_HOR_RES * 10 / 11, LV_VER_RES - (LV_DPI * 12 / 7));

	lv_obj_t * lb_desc = lv_label_create(desc, NULL);
	lv_obj_set_style(lb_desc, &monospace_text);
	lv_label_set_long_mode(lb_desc, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(lb_desc, true);
	lv_obj_set_width(lb_desc, lv_obj_get_width(desc));

	if (!sd_mount())
	{
		lv_label_set_text(lb_desc, "#FFDD00 Inizializzazione SD fallita!#");

		goto out_end;
	}

	char path[128];

	u8 kb = 0;
	u8 *pkg1 = (u8 *)calloc(1, 0x40000);
	u8 *warmboot = (u8 *)calloc(1, 0x40000);
	u8 *secmon = (u8 *)calloc(1, 0x40000);
	u8 *loader = (u8 *)calloc(1, 0x40000);
	u8 *pkg2 = NULL;

	char *txt_buf  = (char *)malloc(0x4000);

	if (!sdmmc_storage_init_mmc(&emmc_storage, &emmc_sdmmc, SDMMC_BUS_WIDTH_8, SDHCI_TIMING_MMC_HS400))
	{
		lv_label_set_text(lb_desc, "#FFDD00 Inizializzazione eMMC fallita!#");

		goto out_free;
	}

	sdmmc_storage_set_mmc_partition(&emmc_storage, EMMC_BOOT0);

	// Read package1.
	static const u32 BOOTLOADER_SIZE          = 0x40000;
	static const u32 BOOTLOADER_MAIN_OFFSET   = 0x100000;
	static const u32 HOS_KEYBLOBS_OFFSET      = 0x180000;

	char *build_date = malloc(32);
	u32 pk1_offset =  h_cfg.t210b01 ? sizeof(bl_hdr_t210b01_t) : 0; // Skip T210B01 OEM header.
	sdmmc_storage_read(&emmc_storage, BOOTLOADER_MAIN_OFFSET / NX_EMMC_BLOCKSIZE, BOOTLOADER_SIZE / NX_EMMC_BLOCKSIZE, pkg1);

	const pkg1_id_t *pkg1_id = pkg1_identify(pkg1 + pk1_offset, build_date);

	s_printf(txt_buf, "#00DDFF Trovato pkg1 ('%s')#\n\n", build_date);
	free(build_date);
	lv_label_set_text(lb_desc, txt_buf);
	manual_system_maintenance(true);

	// Dump package1 in its encrypted state if unknown.
	if (!pkg1_id)
	{
		strcat(txt_buf, "#FFDD00 Versione pkg1 sconosciuta!#");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		emmcsn_path_impl(path, "/pkg1", "pkg1_enc.bin", &emmc_storage);
		if (sd_save_to_file(pkg1, BOOTLOADER_SIZE, path))
			goto out_free;

		strcat(txt_buf, "\nPkg1 criptato salvato come pkg1_enc.bin");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		goto out_free;
	}

	kb = pkg1_id->kb;

	if (!h_cfg.se_keygen_done)
	{
		tsec_ctxt_t tsec_ctxt;
		tsec_ctxt.fw = (void *)(pkg1 + pkg1_id->tsec_off);
		tsec_ctxt.pkg1 = (void *)pkg1;
		tsec_ctxt.pkg11_off = pkg1_id->pkg11_off;
		tsec_ctxt.secmon_base = pkg1_id->secmon_base;

		hos_eks_get();

		if (!h_cfg.t210b01 && kb >= KB_FIRMWARE_VERSION_700 && !h_cfg.sept_run)
		{
			u32 key_idx = 0;
			if (kb >= KB_FIRMWARE_VERSION_810)
				key_idx = 1;

			if (h_cfg.eks && h_cfg.eks->enabled[key_idx] >= kb)
				h_cfg.sept_run = true;
			else
			{
				// Check that BCT is proper so sept can run.
				u8 *bct_bldr = (u8 *)calloc(1, 512);
				sdmmc_storage_read(&emmc_storage, 0x2200 / NX_EMMC_BLOCKSIZE, 1, bct_bldr);
				u32 bootloader_entrypoint = *(u32 *)&bct_bldr[0x144];
				free(bct_bldr);
				if (bootloader_entrypoint > SEPT_PRI_ENTRY)
				{
					lv_label_set_text(lb_desc, "#FFDD00 Avvio di sept fallito perche' la BCT principale e' inadeguata!#\n"
						"#FFDD00 Avvia sept con la BCT adeguata almeno una volta per fare il caching delle chiavi.#\n");
					goto out_free;
				}

				// Set boot cfg.
				b_cfg->autoboot = 0;
				b_cfg->autoboot_list = 0;
				b_cfg->extra_cfg = EXTRA_CFG_NYX_SEPT;
				b_cfg->sept = NYX_SEPT_DUMP;

				if (!reboot_to_sept((u8 *)tsec_ctxt.fw, kb))
				{
					lv_label_set_text(lb_desc, "#FFDD00 Avvio di sept fallito#\n");
					goto out_free;
				}
			}
		}

		// Read keyblob.
		u8 *keyblob = (u8 *)calloc(NX_EMMC_BLOCKSIZE, 1);
		sdmmc_storage_read(&emmc_storage, HOS_KEYBLOBS_OFFSET / NX_EMMC_BLOCKSIZE + kb, 1, keyblob);

		// Decrypt.
		hos_keygen(keyblob, kb, &tsec_ctxt);
		if (kb <= KB_FIRMWARE_VERSION_600)
			h_cfg.se_keygen_done = 1;
		free(keyblob);
	}

	if (h_cfg.t210b01 || kb <= KB_FIRMWARE_VERSION_600)
	{
		if (!pkg1_decrypt(pkg1_id, pkg1))
		{
			strcat(txt_buf, "#FFDD00 Decrittazione pkg1 fallita!#\n");
			if (h_cfg.t210b01)
				strcat(txt_buf, "#FFDD00 Forse manca BEK?#\n");
			lv_label_set_text(lb_desc, txt_buf);
			goto out_free;
		}
	}

	if (h_cfg.t210b01 || kb <= KB_FIRMWARE_VERSION_620)
	{
		const u8 *sec_map = pkg1_unpack(warmboot, secmon, loader, pkg1_id, pkg1 + pk1_offset);

		pk11_hdr_t *hdr_pk11 = (pk11_hdr_t *)(pkg1 + pk1_offset + pkg1_id->pkg11_off + 0x20);

		// Use correct sizes.
		u32 sec_size[3] = { hdr_pk11->wb_size, hdr_pk11->ldr_size, hdr_pk11->sm_size };
		for (u32 i = 0; i < 3; i++)
		{
			if (sec_map[i] == PK11_SECTION_WB)
				hdr_pk11->wb_size = sec_size[i];
			else if (sec_map[i] == PK11_SECTION_LD)
				hdr_pk11->ldr_size = sec_size[i];
			else if (sec_map[i] == PK11_SECTION_SM)
				hdr_pk11->sm_size = sec_size[i];
		}

		// Display info.
		s_printf(txt_buf + strlen(txt_buf),
			"#C7EA46 Dimensione Bootloader NX:  #0x%05X\n"
			"#C7EA46 Indirizzo Secure monitor: #0x%05X\n"
			"#C7EA46 Dimensione Secure monitor: #0x%05X\n"
			"#C7EA46 Indirizzo Warmboot:       #0x%05X\n"
			"#C7EA46 Dimensione Warmboot:       #0x%05X\n\n",
			hdr_pk11->ldr_size, pkg1_id->secmon_base, hdr_pk11->sm_size, pkg1_id->warmboot_base, hdr_pk11->wb_size);

		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		// Dump package1.1.
		emmcsn_path_impl(path, "/pkg1", "pkg1_decr.bin", &emmc_storage);
		if (sd_save_to_file(pkg1, 0x40000, path))
			goto out_free;
		strcat(txt_buf, "pkg1 salvato come pkg1_decr.bin\n");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		// Dump nxbootloader.
		emmcsn_path_impl(path, "/pkg1", "nxloader.bin", &emmc_storage);
		if (sd_save_to_file(loader, hdr_pk11->ldr_size, path))
			goto out_free;
		strcat(txt_buf, "NX Bootloader salvato come nxloader.bin\n");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		// Dump secmon.
		emmcsn_path_impl(path, "/pkg1", "secmon.bin", &emmc_storage);
		if (sd_save_to_file(secmon, hdr_pk11->sm_size, path))
			goto out_free;
		strcat(txt_buf, "Secure Monitor salvato come secmon.bin\n");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		// Dump warmboot.
		emmcsn_path_impl(path, "/pkg1", "warmboot.bin", &emmc_storage);
		if (sd_save_to_file(warmboot, hdr_pk11->wb_size, path))
			goto out_free;
		// If T210B01, save a copy of decrypted warmboot binary also.
		if (h_cfg.t210b01)
		{

			se_aes_iv_clear(13);
			se_aes_crypt_cbc(13, 0, warmboot + 0x330, hdr_pk11->wb_size - 0x330, warmboot + 0x330, hdr_pk11->wb_size - 0x330);
			emmcsn_path_impl(path, "/pkg1", "warmboot_dec.bin", &emmc_storage);
			if (sd_save_to_file(warmboot, hdr_pk11->wb_size, path))
				goto out_free;
		}
		strcat(txt_buf, "Warmboot salvato come warmboot.bin\n\n");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);
	}

	// Dump package2.1.
	sdmmc_storage_set_mmc_partition(&emmc_storage, EMMC_GPP);
	// Parse eMMC GPT.
	LIST_INIT(gpt);
	nx_emmc_gpt_parse(&gpt, &emmc_storage);
	// Find package2 partition.
	emmc_part_t *pkg2_part = nx_emmc_part_find(&gpt, "BCPKG2-1-Normal-Main");
	if (!pkg2_part)
		goto out;

	// Read in package2 header and get package2 real size.
	u8 *tmp = (u8 *)malloc(NX_EMMC_BLOCKSIZE);
	nx_emmc_part_read(&emmc_storage, pkg2_part, 0x4000 / NX_EMMC_BLOCKSIZE, 1, tmp);
	u32 *hdr_pkg2_raw = (u32 *)(tmp + 0x100);
	u32 pkg2_size = hdr_pkg2_raw[0] ^ hdr_pkg2_raw[2] ^ hdr_pkg2_raw[3];
	free(tmp);
	// Read in package2.
	u32 pkg2_size_aligned = ALIGN(pkg2_size, NX_EMMC_BLOCKSIZE);
	pkg2 = malloc(pkg2_size_aligned);
	nx_emmc_part_read(&emmc_storage, pkg2_part, 0x4000 / NX_EMMC_BLOCKSIZE,
		pkg2_size_aligned / NX_EMMC_BLOCKSIZE, pkg2);
#if 0
	emmcsn_path_impl(path, "/pkg2", "pkg2_encr.bin", &emmc_storage);
	if (sd_save_to_file(pkg2, pkg2_size_aligned, path))
		goto out;
	gfx_puts("\npkg2 dumped to pkg2_encr.bin\n");
#endif

	// Decrypt package2 and parse KIP1 blobs in INI1 section.
	pkg2_hdr_t *pkg2_hdr = pkg2_decrypt(pkg2, kb);
	if (!pkg2_hdr)
	{
		strcat(txt_buf, "#FFDD00 Decrittazione Pkg2 fallita!#");
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		// Clear EKS slot, in case something went wrong with sept keygen.
		hos_eks_clear(kb);

		goto out;
	}
	else if (kb >= KB_FIRMWARE_VERSION_700)
		hos_eks_save(kb); // Save EKS slot if it doesn't exist.

	// Display info.
	s_printf(txt_buf + strlen(txt_buf),
		"#C7EA46 Dimensione Kernel:   #0x%05X\n"
		"#C7EA46 Dimensione INI1:     #0x%05X\n\n",
		pkg2_hdr->sec_size[PKG2_SEC_KERNEL], pkg2_hdr->sec_size[PKG2_SEC_INI1]);

	lv_label_set_text(lb_desc, txt_buf);
	manual_system_maintenance(true);

	// Dump pkg2.1.
	emmcsn_path_impl(path, "/pkg2", "pkg2_decr.bin", &emmc_storage);
	if (sd_save_to_file(pkg2, pkg2_hdr->sec_size[PKG2_SEC_KERNEL] + pkg2_hdr->sec_size[PKG2_SEC_INI1], path))
		goto out;
	strcat(txt_buf, "pkg2 salvato come pkg2_decr.bin\n");
	lv_label_set_text(lb_desc, txt_buf);
	manual_system_maintenance(true);

	// Dump kernel.
	emmcsn_path_impl(path, "/pkg2", "kernel.bin", &emmc_storage);
	if (sd_save_to_file(pkg2_hdr->data, pkg2_hdr->sec_size[PKG2_SEC_KERNEL], path))
		goto out;
	strcat(txt_buf, "Kernel salvato come kernel.bin\n");
	lv_label_set_text(lb_desc, txt_buf);
	manual_system_maintenance(true);

	// Dump INI1.
	u32 ini1_off = pkg2_hdr->sec_size[PKG2_SEC_KERNEL];
	u32 ini1_size = pkg2_hdr->sec_size[PKG2_SEC_INI1];
	if (!ini1_size)
	{
		pkg2_get_newkern_info(pkg2_hdr->data);
		ini1_off = pkg2_newkern_ini1_start;
		ini1_size = pkg2_newkern_ini1_end - pkg2_newkern_ini1_start;
	}

	if (!ini1_off)
	{
		strcat(txt_buf, "#FFDD00 Salvataggio di INI1 e dei kip fallito!#\n");
		goto out;
	}

	pkg2_ini1_t *ini1 = (pkg2_ini1_t *)(pkg2_hdr->data + ini1_off);
	emmcsn_path_impl(path, "/pkg2", "ini1.bin", &emmc_storage);
	if (sd_save_to_file(ini1, ini1_size, path))
		goto out;

	strcat(txt_buf, "INI1 salvato come ini1.bin\n\n");
	lv_label_set_text(lb_desc, txt_buf);
	manual_system_maintenance(true);

	char filename[32];
	u8 *ptr = (u8 *)ini1;
	ptr += sizeof(pkg2_ini1_t);

	// Dump all kips.
	u8 *kip_buffer = (u8 *)malloc(0x400000);

	for (u32 i = 0; i < ini1->num_procs; i++)
	{
		pkg2_kip1_t *kip1 = (pkg2_kip1_t *)ptr;
		u32 kip1_size = pkg2_calc_kip1_size(kip1);

		s_printf(filename, "%s.kip1", kip1->name);
		if ((u32)kip1 % 8)
		{
			memcpy(kip_buffer, kip1, kip1_size);
			kip1 = (pkg2_kip1_t *)kip_buffer;
		}

		emmcsn_path_impl(path, "/pkg2/ini1", filename, &emmc_storage);
		if (sd_save_to_file(kip1, kip1_size, path))
		{
			free(kip_buffer);
			goto out;
		}

		s_printf(txt_buf + strlen(txt_buf), "%s kip salvato come %s.kip1\n", kip1->name, kip1->name);
		lv_label_set_text(lb_desc, txt_buf);
		manual_system_maintenance(true);

		ptr += kip1_size;
	}
	free(kip_buffer);

out:
	nx_emmc_gpt_free(&gpt);
out_free:
	free(pkg1);
	free(secmon);
	free(warmboot);
	free(loader);
	free(pkg2);
	free(txt_buf);
	sdmmc_storage_end(&emmc_storage);
	sd_unmount();

	if (kb >= KB_FIRMWARE_VERSION_620)
		se_aes_key_clear(8);
out_end:
	// Enable buttons.
	nyx_window_toggle_buttons(win, false);

	return LV_RES_OK;
}

void sept_run_dump(void *param)
{
	_create_window_dump_pk12_tool(NULL);
}

static void _create_tab_tools_emmc_pkg12(lv_theme_t *th, lv_obj_t *parent)
{
	lv_page_set_scrl_layout(parent, LV_LAYOUT_PRETTY);

	// Create Backup & Restore container.
	lv_obj_t *h1 = _create_container(parent);

	lv_obj_t *label_sep = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_txt, "Backup & Ripristino");
	lv_obj_set_style(label_txt, th->label.prim);
	lv_obj_align(label_txt, label_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, -LV_DPI * 3 / 10);

	lv_obj_t *line_sep = lv_line_create(h1, NULL);
	static const lv_point_t line_pp[] = { {0, 0}, { LV_HOR_RES - (LV_DPI - (LV_DPI / 4)) * 2, 0} };
	lv_line_set_points(line_sep, line_pp, 2);
	lv_line_set_style(line_sep, th->line.decor);
	lv_obj_align(line_sep, label_txt, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create Backup eMMC button.
	lv_obj_t *btn = lv_btn_create(h1, NULL);
	if (hekate_bg)
	{
		lv_btn_set_style(btn, LV_BTN_STYLE_REL, &btn_transp_rel);
		lv_btn_set_style(btn, LV_BTN_STYLE_PR, &btn_transp_pr);
	}
	lv_obj_t *label_btn = lv_label_create(btn, NULL);
	lv_btn_set_fit(btn, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_UPLOAD"  Backup eMMC");
	lv_obj_align(btn, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn, LV_BTN_ACTION_CLICK, create_window_backup_restore_tool);

	lv_obj_t *label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di fare il backup delle singole partizioni della eMMC o\n"
		"o un'intera immagine sulla scheda SD.\n"
		"#C7EA46 Supporta schede SD da# #FF8000 4GB# #C7EA46 in su. #"
		"#FF8000 FAT32# #C7EA46 e ##FF8000 exFAT##C7EA46 .#");
	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create Restore eMMC button.
	lv_obj_t *btn2 = lv_btn_create(h1, btn);
	label_btn = lv_label_create(btn2, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_DOWNLOAD"  Ripristina eMMC");
	lv_obj_align(btn2, label_txt2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn2, LV_BTN_ACTION_CLICK, create_window_backup_restore_tool);

	label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di ripristinare le singole partizioni della\n"
		"eMMC/emuMMC o l'intero volume con un'immagine sulla scheda SD.\n"
		"#C7EA46 Supporta schede SD da# #FF8000 4GB# #C7EA46 in su. #"
		"#FF8000 FAT32# #C7EA46 e ##FF8000 exFAT##C7EA46 .#");
	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create Misc container.
	lv_obj_t *h2 = _create_container(parent);
	lv_obj_align(h2, h1, LV_ALIGN_OUT_RIGHT_TOP, 0, 0);

	label_sep = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt3 = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_txt3, "Partizioni SD & USB");
	lv_obj_set_style(label_txt3, th->label.prim);
	lv_obj_align(label_txt3, label_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, -LV_DPI * 3 / 10);

	line_sep = lv_line_create(h2, line_sep);
	lv_obj_align(line_sep, label_txt3, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create Partition SD Card button.
	lv_obj_t *btn3 = lv_btn_create(h2, NULL);
	if (hekate_bg)
	{
		lv_btn_set_style(btn3, LV_BTN_STYLE_REL, &btn_transp_rel);
		lv_btn_set_style(btn3, LV_BTN_STYLE_PR, &btn_transp_pr);
	}
	label_btn = lv_label_create(btn3, NULL);
	lv_btn_set_fit(btn3, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_SD"  Partiziona Scheda SD");
	lv_obj_align(btn3, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn3, LV_BTN_ACTION_CLICK, create_window_partition_manager);

	lv_obj_t *label_txt4 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt4, true);
	lv_label_set_static_text(label_txt4,
		"Ti permette di partizionare la Scheda SD per usarla con la\n"
		"#C7EA46 emuMMC#, #C7EA46 Android# e #C7EA46 Linux#.\n"
		"Puoi anche flashare Linux e Android.\n");
	lv_obj_set_style(label_txt4, &hint_small_style);
	lv_obj_align(label_txt4, btn3, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	label_sep = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_sep, "");
	lv_obj_align(label_sep, label_txt4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI * 11 / 7);

	// Create USB Tools button.
	lv_obj_t *btn4 = lv_btn_create(h2, btn3);
	label_btn = lv_label_create(btn4, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_USB"  Strumenti USB");
	lv_obj_align(btn4, label_txt4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn4, LV_BTN_ACTION_CLICK, _create_window_usb_tools);

	label_txt4 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt4, true);
	lv_label_set_static_text(label_txt4,
		"#C7EA46 Archiviazione di massa USB#, #C7EA46 gamepad# e altri strumenti USB.\n"
		"L'archiviazione di massa puo' montare SD, eMMC e emuMMC.\n"
		"L'opzione gamepad trasforma lo Switch in un\ndispositivo di input.#");
	lv_obj_set_style(label_txt4, &hint_small_style);
	lv_obj_align(label_txt4, btn4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);
}

static void _create_tab_tools_arc_autorcm(lv_theme_t *th, lv_obj_t *parent)
{
	lv_page_set_scrl_layout(parent, LV_LAYOUT_PRETTY);

	// Create Misc container.
	lv_obj_t *h1 = _create_container(parent);

	lv_obj_t *label_sep = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_txt, "Varie");
	lv_obj_set_style(label_txt, th->label.prim);
	lv_obj_align(label_txt, label_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, -LV_DPI * 3 / 10);

	lv_obj_t *line_sep = lv_line_create(h1, NULL);
	static const lv_point_t line_pp[] = { {0, 0}, { LV_HOR_RES - (LV_DPI - (LV_DPI / 4)) * 2, 0} };
	lv_line_set_points(line_sep, line_pp, 2);
	lv_line_set_style(line_sep, th->line.decor);
	lv_obj_align(line_sep, label_txt, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create Unset archive bit button.
	lv_obj_t *btn = lv_btn_create(h1, NULL);
	if (hekate_bg)
	{
		lv_btn_set_style(btn, LV_BTN_STYLE_REL, &btn_transp_rel);
		lv_btn_set_style(btn, LV_BTN_STYLE_PR, &btn_transp_pr);
	}
	lv_obj_t *label_btn = lv_label_create(btn, NULL);
	lv_btn_set_fit(btn, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_DIRECTORY"  Sistema Bit Archiviazione");
	lv_obj_align(btn, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn, LV_BTN_ACTION_CLICK, _create_window_unset_abit_tool);

	lv_obj_t *label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di sistemare il bit di archiviazione per tutte\n"
		"le cartelle, incluse la radice della scheda SD e \n"
		"le cartelle emuMMC \'Nintendo\'.\n"
		"#C7EA46 Imposta il bit di archiviazione a cartelle con ##FF8000 .[ext]#\n"
		"#FF8000 Use questa opzione quando hai avvisi di corruzione.#");
	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create Fix touch calibration button.
	lv_obj_t *btn2 = lv_btn_create(h1, btn);
	label_btn = lv_label_create(btn2, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_KEYBOARD"  Calibra Touchscreen");
	lv_obj_align(btn2, label_txt2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn2, LV_BTN_ACTION_CLICK, _create_mbox_fix_touchscreen);

	label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di calibrare il modulo touchscreen.\n"
		"#FF8000 Cio' sistema vari problemi con il touchscreen in Nyx e HOS.#");
	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create Others container.
	lv_obj_t *h2 = _create_container(parent);
	lv_obj_align(h2, h1, LV_ALIGN_OUT_RIGHT_TOP, 0, 0);

	label_sep = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt3 = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_txt3, "Altri");
	lv_obj_set_style(label_txt3, th->label.prim);
	lv_obj_align(label_txt3, label_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, -LV_DPI * 3 / 10);

	line_sep = lv_line_create(h2, line_sep);
	lv_obj_align(line_sep, label_txt3, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create AutoRCM On/Off button.
	lv_obj_t *btn3 = lv_btn_create(h2, NULL);
	if (hekate_bg)
	{
		lv_btn_set_style(btn3, LV_BTN_STYLE_REL, &btn_transp_rel);
		lv_btn_set_style(btn3, LV_BTN_STYLE_PR, &btn_transp_pr);
		lv_btn_set_style(btn3, LV_BTN_STYLE_TGL_REL, &btn_transp_tgl_rel);
		lv_btn_set_style(btn3, LV_BTN_STYLE_TGL_PR, &btn_transp_tgl_pr);
	}
	label_btn = lv_label_create(btn3, NULL);
	lv_btn_set_fit(btn3, true, true);
	lv_label_set_recolor(label_btn, true);
	lv_label_set_text(label_btn, SYMBOL_REFRESH"  AutoRCM #00FFC9   ATTIVO #");
	lv_obj_align(btn3, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn3, LV_BTN_ACTION_CLICK, _create_mbox_autorcm_status);

	// Set default state for AutoRCM and lock it out if patched unit.
	if (get_autorcm_status(false))
		lv_btn_set_state(btn3, LV_BTN_STATE_TGL_REL);
	else
		lv_btn_set_state(btn3, LV_BTN_STATE_REL);
	nyx_generic_onoff_toggle(btn3);

	if (h_cfg.rcm_patched)
	{
		lv_obj_set_click(btn3, false);
		lv_btn_set_state(btn3, LV_BTN_STATE_INA);
	}
	autorcm_btn = btn3;

	char *txt_buf = (char *)malloc(0x1000);

	s_printf(txt_buf,
		"Ti permette di entrare in RCM senza usare #C7EA46 VOL+# & #C7EA46 HOME# (jig).\n"
		"#FF8000 Puo' ripristinare tutte le varianti di AutoRCM quando richiesto.#\n"
		"#FF3C28 Questo corrompe la BCT e non si puo' avviare senza un bootloader#\n"
		"#FF3C28 personalizzato.#");

	if (h_cfg.rcm_patched)
		strcat(txt_buf, " #FF8000 Questa opzione e' disattivata perche' l'unita' e' patchata!#");

	lv_obj_t *label_txt4 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt4, true);
	lv_label_set_text(label_txt4, txt_buf);
	free(txt_buf);

	lv_obj_set_style(label_txt4, &hint_small_style);
	lv_obj_align(label_txt4, btn3, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	label_sep = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_sep, "");
	lv_obj_align(label_sep, label_txt4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI * 11 / 7);

	// Create Dump Package1/2 button.
	lv_obj_t *btn4 = lv_btn_create(h2, btn);
	label_btn = lv_label_create(btn4, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_MODULES"  Salva Package1/2");
	lv_obj_align(btn4, label_txt4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_btn_set_action(btn4, LV_BTN_ACTION_CLICK, _create_window_dump_pk12_tool);

	label_txt2 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Ti permette di salvare e decriptare pkg1 e pkg2 e suddividerli\n"
		"nelle loro parti individuali. Salva anche il kip1.");
	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn4, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);
}

void create_tab_tools(lv_theme_t *th, lv_obj_t *parent)
{
	lv_obj_t *tv = lv_tabview_create(parent, NULL);

	lv_obj_set_size(tv, LV_HOR_RES, 572);

	static lv_style_t tabview_style;
	lv_style_copy(&tabview_style, th->tabview.btn.rel);
	tabview_style.body.padding.ver = LV_DPI / 8;

	lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_REL, &tabview_style);
	if (hekate_bg)
	{
		lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_PR, &tabview_btn_pr);
		lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_TGL_PR, &tabview_btn_tgl_pr);
	}

	lv_tabview_set_sliding(tv, false);
	lv_tabview_set_btns_pos(tv, LV_TABVIEW_BTNS_POS_BOTTOM);

	lv_obj_t *tab1= lv_tabview_add_tab(tv, "eMMC "SYMBOL_DOT" Partizioni SD "SYMBOL_DOT" USB");
	lv_obj_t *tab2 = lv_tabview_add_tab(tv, "Bit Arch "SYMBOL_DOT" RCM "SYMBOL_DOT" Touch "SYMBOL_DOT" Pkg1/2");

	lv_obj_t *line_sep = lv_line_create(tv, NULL);
	static const lv_point_t line_pp[] = { {0, 0}, { 0, LV_DPI / 4} };
	lv_line_set_points(line_sep, line_pp, 2);
	lv_line_set_style(line_sep, lv_theme_get_current()->line.decor);
	lv_obj_align(line_sep, tv, LV_ALIGN_IN_BOTTOM_MID, -1, -LV_DPI * 2 / 12);

	_create_tab_tools_emmc_pkg12(th, tab1);
	_create_tab_tools_arc_autorcm(th, tab2);

	lv_tabview_set_tab_act(tv, 0, false);
}
