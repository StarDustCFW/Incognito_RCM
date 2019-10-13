/*
 * Copyright (c) 2018 naehrwert
 *
 * Copyright (c) 2018-2019 CTCaer
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

#include <string.h>
#include "config/config.h"
#include "gfx/di.h"
#include "gfx/gfx.h"
#include "gfx/tui.h"
#include "libs/fatfs/ff.h"
#include "mem/heap.h"
#include "power/max77620.h"
#include "rtc/max77620-rtc.h"
#include "soc/bpmp.h"
#include "soc/hw_init.h"
#include "soc\t210.h"
#include "storage/emummc.h"
#include "storage/sdmmc.h"
#include "utils/sprintf.h"
#include "utils/util.h"
#include "utils/btn.h"

#include "incognito/incognito.h"

sdmmc_t sd_sdmmc;
sdmmc_storage_t sd_storage;
__attribute__((aligned(16))) FATFS sd_fs;
static bool sd_mounted;

hekate_config h_cfg;
boot_cfg_t __attribute__((section("._boot_cfg"))) b_cfg;

bool sd_mount()
{
    if (sd_mounted)
        return true;

    if (!sdmmc_storage_init_sd(&sd_storage, &sd_sdmmc, SDMMC_1, SDMMC_BUS_WIDTH_4, 11))
    {
        EPRINTF("Failed to init SD card.\nMake sure that it is inserted.\nOr that SD reader is properly seated!");
    }
    else
    {
        int res = 0;
        res = f_mount(&sd_fs, "sd:", 1);
        if (res == FR_OK)
        {
            sd_mounted = 1;
            return true;
        }
        else
        {
            EPRINTFARGS("Failed to mount SD card (FatFS Error %d).\nMake sure that a FAT partition exists..", res);
        }
    }

    return false;
}

void sd_unmount()
{
    if (sd_mounted)
    {
        f_mount(NULL, "sd:", 1);
        sdmmc_storage_end(&sd_storage);
        sd_mounted = false;
    }
}

void *sd_file_read(const char *path, u32 *fsize)
{
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
        return NULL;

    u32 size = f_size(&fp);
    if (fsize)
        *fsize = size;

    void *buf = malloc(size);

    if (f_read(&fp, buf, size, NULL) != FR_OK)
    {
        free(buf);
        f_close(&fp);

        return NULL;
    }

    f_close(&fp);

    return buf;
}

int sd_save_to_file(void *buf, u32 size, const char *filename)
{
    FIL fp;
    u32 res = 0;
    res = f_open(&fp, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (res)
    {
        EPRINTFARGS("Error (%d) creating file\n%s.\n", res, filename);
        return res;
    }

    f_write(&fp, buf, size, NULL);
    f_close(&fp);

    return 0;
}

bool sd_file_exists(const char* filename)
{
    FIL fp;
	u32 res = 0;
	res = f_open(&fp, filename, FA_READ);
	if (res == FR_OK)
	{
        f_close(&fp);
        return true;
	}

    return false;
}

// This is a safe and unused DRAM region for our payloads.
#define RELOC_META_OFF 0x7C
#define PATCHED_RELOC_SZ 0x94
#define PATCHED_RELOC_STACK 0x40007000
#define PATCHED_RELOC_ENTRY 0x40010000
#define EXT_PAYLOAD_ADDR    0xC03C0000
#define RCM_PAYLOAD_ADDR    (EXT_PAYLOAD_ADDR + ALIGN(PATCHED_RELOC_SZ, 0x10))
#define COREBOOT_ADDR (0xD0000000 - 0x100000)
#define CBFS_DRAM_EN_ADDR 0x4003e000
#define CBFS_DRAM_MAGIC 0x4452414D // "DRAM"

void reloc_patcher(u32 payload_dst, u32 payload_src, u32 payload_size)
{
    memcpy((u8 *)payload_src, (u8 *)IPL_LOAD_ADDR, PATCHED_RELOC_SZ);

    volatile reloc_meta_t *relocator = (reloc_meta_t *)(payload_src + RELOC_META_OFF);

    relocator->start = payload_dst - ALIGN(PATCHED_RELOC_SZ, 0x10);
    relocator->stack = PATCHED_RELOC_STACK;
    relocator->end = payload_dst + payload_size;
    relocator->ep = payload_dst;

    if (payload_size == 0x7000)
    {
        memcpy((u8 *)(payload_src + ALIGN(PATCHED_RELOC_SZ, 0x10)), (u8 *)COREBOOT_ADDR, 0x7000); //Bootblock
        *(vu32 *)CBFS_DRAM_EN_ADDR = CBFS_DRAM_MAGIC;
    }
}



int launch_payload(char *path, bool update)
{
	if (!update)
		gfx_clear_grey(0x1B);
	gfx_con_setpos(0, 0);
	if (!path)
		return 1;

	if (sd_mount())
	{
		FIL fp;
		if (f_open(&fp, path, FA_READ))
		{
			EPRINTFARGS("Payload file is missing!\n(%s)", path);
			sd_unmount();

			return 1;
		}

		// Read and copy the payload to our chosen address
		void *buf;
		u32 size = f_size(&fp);

		if (size < 0x30000)
			buf = (void *)RCM_PAYLOAD_ADDR;
		else
			buf = (void *)COREBOOT_ADDR;

		if (f_read(&fp, buf, size, NULL))
		{
			f_close(&fp);
			sd_unmount();

			return 1;
		}

		f_close(&fp);

//		if (update && is_ipl_updated(buf))
//			return 1;

		sd_unmount();

		if (size < 0x30000)
		{
			if (update)
				memcpy((u8 *)(RCM_PAYLOAD_ADDR + PATCHED_RELOC_SZ), &b_cfg, sizeof(boot_cfg_t)); // Transfer boot cfg.
			else
				reloc_patcher(PATCHED_RELOC_ENTRY, EXT_PAYLOAD_ADDR, ALIGN(size, 0x10));

			reconfig_hw_workaround(false, byte_swap_32(*(u32 *)(buf + size - sizeof(u32))));
		}
		else
		{
			reloc_patcher(PATCHED_RELOC_ENTRY, EXT_PAYLOAD_ADDR, 0x7000);
			reconfig_hw_workaround(true, 0);
		}

		// Some cards (Sandisk U1), do not like a fast power cycle. Wait min 100ms.
		sdmmc_storage_init_wait_sd();

		void (*ext_payload_ptr)() = (void *)EXT_PAYLOAD_ADDR;
		void (*update_ptr)() = (void *)RCM_PAYLOAD_ADDR;

		// Launch our payload.
		if (!update)
			(*ext_payload_ptr)();
		else
		{
			EMC(EMC_SCRATCH0) |= EMC_HEKA_UPD;
			(*update_ptr)();
		}
	}

	return 1;
}

void incognito_sysnand()
{

    h_cfg.emummc_force_disable = true;
    b_cfg.extra_cfg &= ~EXTRA_CFG_DUMP_EMUMMC;
    if (!dump_keys())
        goto out;
    if (!incognito())
    {
        gfx_printf("%kError applying Incognito!\nWill restore backup!\n", COLOR_RED);
        backupProdinfo();
    }
    if (!verifyProdinfo(NULL))
    {
        gfx_printf("%kThis should not happen!\nTry restoring or restore via NAND backup from hekate!\n", COLOR_RED);
    }
out:
    cleanUp();
	if (sd_mount()){
	f_unlink("/StarDust/sysnand_serial.txt");
	f_rename("/StarDust/serial.txt", "/StarDust/sysnand_serial.txt");}
    gfx_printf("\n%k---------------\n%kPress any key to return to the main menu.", COLOR_YELLOW, COLOR_ORANGE);
    
}

void incognito_emunand()
{
    if (h_cfg.emummc_force_disable)
        return;
    emu_cfg.enabled = 1;
    b_cfg.extra_cfg |= EXTRA_CFG_DUMP_EMUMMC;
    if (!dump_keys())
        goto out;
    if (!incognito())
    {
        gfx_printf("%kError applying Incognito!\nWill restore backup!\n", COLOR_RED);
        backupProdinfo();
    }
    if (!verifyProdinfo(NULL))
    {
        gfx_printf("%kThis should not happen!\nTry restoring or restore via NAND backup from hekate!\n", COLOR_RED);
    }
out:
    cleanUp();
	if (sd_mount()){
	f_unlink("/StarDust/emunand_serial.txt");
	f_rename("/StarDust/serial.txt", "/StarDust/emunand_serial.txt");}
    gfx_printf("\n%k---------------\n%kPress any key to return to the main menu.", COLOR_YELLOW, COLOR_ORANGE);
    
}

void backup_sysnand()
{
    h_cfg.emummc_force_disable = true;
    b_cfg.extra_cfg &= ~EXTRA_CFG_DUMP_EMUMMC;
    if (!dump_keys())
        goto out;

    backupProdinfo();
out:
    cleanUp();
    gfx_printf("\n%k---------------\n%kPress any key to return to the main menu.", COLOR_YELLOW, COLOR_ORANGE);
    
}

void backup_emunand()
{
    if (h_cfg.emummc_force_disable)
        return;
    emu_cfg.enabled = 1;
    b_cfg.extra_cfg |= EXTRA_CFG_DUMP_EMUMMC;
    if (!dump_keys())
        goto out;

    backupProdinfo();
out:
    cleanUp();
	if (sd_mount()){
	f_unlink("/StarDust/sysnand_serial.txt");
	f_rename("/StarDust/serial.txt", "/StarDust/sysnand_serial.txt");}
    gfx_printf("\n%k---------------\n%kPress any key to return to the main menu.", COLOR_YELLOW, COLOR_ORANGE);
    
}

void restore_sysnand()
{
    h_cfg.emummc_force_disable = true;
    b_cfg.extra_cfg &= ~EXTRA_CFG_DUMP_EMUMMC;
    if (!dump_keys())
        goto out;

    restoreProdinfo();
    if (!verifyProdinfo(NULL))
    {
        gfx_printf("%kThis should not happen!\nTry restoring or restore via NAND backup from hekate!\n", COLOR_RED);
    }
out:
    cleanUp();
	if (sd_mount()){
	f_unlink("/StarDust/sysnand_serial.txt");
	f_rename("/StarDust/serial.txt", "/StarDust/sysnand_serial.txt");}
    gfx_printf("\n%k---------------\n%kPress any key to return to the main menu.", COLOR_YELLOW, COLOR_ORANGE);
    
}

void restore_emunand()
{
    if (h_cfg.emummc_force_disable)
        return;
    emu_cfg.enabled = 1;
    b_cfg.extra_cfg |= EXTRA_CFG_DUMP_EMUMMC;
    if (!dump_keys())
        goto out;

    restoreProdinfo();
    if (!verifyProdinfo(NULL))
    {
        gfx_printf("%kThis should not happen!\nTry restoring or restore via NAND backup from hekate!\n", COLOR_RED);
    }
out:
    cleanUp();
	if (sd_mount()){
	f_unlink("/StarDust/emunand_serial.txt");
	f_rename("/StarDust/serial.txt", "/StarDust/emunand_serial.txt");}
    gfx_printf("\n%k---------------\n%kPress any key to return to the main menu.", COLOR_YELLOW, COLOR_ORANGE);
    
}

void main_menu()
{
	launch_payload("boot_payload.bin", true);
	launch_payload("atmosphere/reboot_payload.bin", true);
}

ment_t ment_top[] = {
    MDEF_HANDLER("Backup (SysNAND)", backup_sysnand, COLOR_ORANGE),
    MDEF_HANDLER("Backup (emuMMC)", backup_emunand, COLOR_ORANGE),
    MDEF_CAPTION("", COLOR_YELLOW),
    MDEF_HANDLER("Incognito (SysNAND)", incognito_sysnand, COLOR_ORANGE),
    MDEF_HANDLER("Incognito (emuMMC)", incognito_emunand, COLOR_ORANGE),

    MDEF_CAPTION("", COLOR_YELLOW),
    MDEF_HANDLER("Restore (SysNAND)", restore_sysnand, COLOR_ORANGE),
    MDEF_HANDLER("Restore (emuMMC)", restore_emunand, COLOR_ORANGE),
    MDEF_CAPTION("", COLOR_YELLOW),
    MDEF_CAPTION("---------------", COLOR_YELLOW),
    MDEF_HANDLER("Reboot (Normal)", reboot_normal, COLOR_GREEN),
    MDEF_HANDLER("Reboot Menu", main_menu, COLOR_BLUE),
    MDEF_HANDLER("Power off", power_off, COLOR_VIOLET),
    MDEF_END()};

menu_t menu_top = {ment_top, NULL, 0, 0};

#define IPL_STACK_TOP 0x4003F000
#define IPL_HEAP_START 0x90020000

extern void pivot_stack(u32 stack_top);

void ipl_main()
{
    config_hw();
    pivot_stack(IPL_STACK_TOP);
    heap_init(IPL_HEAP_START);

    set_default_configuration();

    display_init();
    u32 *fb = display_init_framebuffer();
    gfx_init_ctxt(fb, 720, 1280, 720);
    gfx_con_init();
    display_backlight_pwm_init();

    bpmp_clk_rate_set(BPMP_CLK_SUPER_BOOST);

    h_cfg.emummc_force_disable = emummc_load_cfg();

    if (b_cfg.boot_cfg & BOOT_CFG_SEPT_RUN)
    {
        if (!(b_cfg.extra_cfg & EXTRA_CFG_DUMP_EMUMMC))
            h_cfg.emummc_force_disable = true;
        dump_keys();
    }

    if (h_cfg.emummc_force_disable)
    {
        ment_top[1].type = MENT_CAPTION;
        ment_top[1].color = 0xFF555555;
        ment_top[1].handler = NULL;
    }
//order by argonNX
	if (sd_mount())
	{
		display_backlight_brightness(100, 1000);
		
		if (sd_file_exists ("StarDust/getinfo.inc"))
		{
			f_unlink("StarDust/getinfo.inc");
			display_backlight_brightness(0, 1000);
			h_cfg.emummc_force_disable = true;
			b_cfg.extra_cfg &= ~EXTRA_CFG_DUMP_EMUMMC;
			dump_keys();
			f_unlink("/StarDust/sysnand_serial.txt");
			f_rename("/StarDust/serial.txt", "/StarDust/sysnand_serial.txt");
			cleanUp();
				if (sd_file_exists ("emummc/emummc.ini"))
				{
					if (h_cfg.emummc_force_disable)
					gfx_printf("\nError\n");
					emu_cfg.enabled = 1;
					b_cfg.extra_cfg |= EXTRA_CFG_DUMP_EMUMMC;
					dump_keys();
					f_unlink("/StarDust/emunand_serial.txt");
					f_rename("/StarDust/serial.txt", "/StarDust/emunand_serial.txt");
					cleanUp();
				}
			main_menu();
		}
		
		if (sd_file_exists ("StarDust/backup_sysnand.inc"))
		{
			f_unlink("StarDust/backup_sysnand.inc");
			backup_sysnand();
			main_menu();
		}
		
		if (sd_file_exists ("StarDust/backup_emunand.inc"))
		{
			f_unlink("StarDust/backup_emunand.inc");
			backup_emunand();
			main_menu();
		}
	
		if (sd_file_exists ("StarDust/incognito_sysnand.inc"))
		{
			f_unlink("StarDust/incognito_sysnand.inc");
			incognito_sysnand();
			main_menu();
		}
		
		if (sd_file_exists ("StarDust/incognito_emunand.inc"))
		{
			f_unlink("StarDust/incognito_emunand.inc");
			incognito_emunand();
			main_menu();
		}
		
		if (sd_file_exists ("StarDust/restore_sysnand.inc"))
		{
			f_unlink("StarDust/restore_sysnand.inc");
			restore_sysnand();
			main_menu();
		}
		
		if (sd_file_exists ("StarDust/restore_emunand.inc"))
		{
			f_unlink("StarDust/restore_emunand.inc");
			restore_emunand();
			main_menu();
		}	
	}

    while (true)
        tui_do_menu(&menu_top);

    while (true)
        bpmp_halt();
}
