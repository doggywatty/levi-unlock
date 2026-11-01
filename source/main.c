#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <gccore.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <ogc/usbstorage.h>
#include <wiiuse/wpad.h>

#include "usbstorage2.h"
#include "wbfs.h"

#define CFG_SIZE 256
#define CFG_PIN_OFFSET 132
#define CFG_MAGIC_WBFS 0xcacafea1u
#define CFG_MAGIC_FAT 0xcacafea2u
#define CFG_MAX_FILE (CFG_SIZE + 65536)

struct uloader_cfg_layout {
	u32 magic;
	u8 id[16][8];
	char parental[4];
	u8 game_log[8][12];
	u32 hi_score;
	u8 music_mod;
	u8 rumble_off;
	u8 icon;
	u8 pad;
	u32 pad2[4];
};

typedef char cfg_check_parental[(offsetof(struct uloader_cfg_layout, parental) == CFG_PIN_OFFSET) ? 1 : -1];
typedef char cfg_check_size[(sizeof(struct uloader_cfg_layout) == CFG_SIZE) ? 1 : -1];

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static const char *fat_volumes[] = { "sd",  "sd2",  "sd3",  "sd4", "usb", "usb2", "usb3", "usb4", NULL };
static const char *usb_volumes[] = { "usb", "usb2", "usb3", "usb4", NULL };

static void video_init(void)
{
	VIDEO_Init();
	WPAD_Init();
	PAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

static void scan_buttons(u32 *wpad, u32 *pad)
{
	WPAD_ScanPads();
	PAD_ScanPads();

	*wpad = WPAD_ButtonsDown(0);
	*pad = PAD_ButtonsDown(0);
}

static void wait_release(void)
{
	u32 wpad;
	u32 pad;

	do
	{
		scan_buttons(&wpad, &pad);
		VIDEO_WaitVSync();
	}
	while (wpad || pad);
}

static int wait_confirm(void)
{
	u32 wpad;
	u32 pad;

	wait_release();

	for (;;)
	{
		scan_buttons(&wpad, &pad);

		if ((wpad & WPAD_BUTTON_A) || (pad & PAD_BUTTON_A))
			return 1;

		if ((wpad & (WPAD_BUTTON_HOME | WPAD_BUTTON_B)) || (pad & (PAD_BUTTON_START | PAD_BUTTON_B)))
			return 0;

		VIDEO_WaitVSync();
	}
}

static void wait_any_button(void)
{
	u32 wpad;
	u32 pad;
	int i;

	for (i = 0; i < 30; i++)
	{
		scan_buttons(&wpad, &pad);
		VIDEO_WaitVSync();
	}

	for (;;)
	{
		scan_buttons(&wpad, &pad);
		if (wpad || pad)
			return;
		VIDEO_WaitVSync();
	}
}

static int cfg_magic_ok(const u8 *buf)
{
	u32 magic;
	memcpy(&magic, buf, sizeof(magic));
	return magic == CFG_MAGIC_WBFS || magic == CFG_MAGIC_FAT;
}

static int cfg_pin_is_set(const u8 *buf)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		if (buf[CFG_PIN_OFFSET + i])
			return 1;
	}

	return 0;
}

static void cfg_print_pin(const u8 *buf)
{
	int i;
	int digits = 1;

	if (!cfg_pin_is_set(buf))
	{
		printf("     PIN: not set (parental control disabled)\n");
		return;
	}

	for (i = 0; i < 4; i++)
	{
		if (buf[CFG_PIN_OFFSET + i] > 9)
			digits = 0;
	}

	if (digits)
		printf("     Current PIN: %u%u%u%u\n", (unsigned)buf[CFG_PIN_OFFSET + 0], (unsigned)buf[CFG_PIN_OFFSET + 1], (unsigned)buf[CFG_PIN_OFFSET + 2], (unsigned)buf[CFG_PIN_OFFSET + 3]);
	else
		printf("     Current PIN bytes: %02X %02X %02X %02X\n", buf[CFG_PIN_OFFSET + 0], buf[CFG_PIN_OFFSET + 1], buf[CFG_PIN_OFFSET + 2], buf[CFG_PIN_OFFSET + 3]);
}

static void cfg_clear_pin(u8 *buf)
{
	memset(buf + CFG_PIN_OFFSET, 0, 4);
}

/*
 * returns:
 * 0: no config at this path
 * 1: config found, PIN was already disabled
 * 2: config found, PIN removed
 * -1: config found, but an error occurred
 */
static int patch_fat_file(const char *path)
{
	FILE *fp;
	long len;
	u8 *buf;
	char backup[320];
	int ok;

	fp = fopen(path, "rb");
	if (!fp)
		return 0;

	if (fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return 0;
	}

	len = ftell(fp);
	if (len < CFG_SIZE || len > CFG_MAX_FILE)
	{
		fclose(fp);
		printf("  %s\n     Unexpected file size (%ld bytes), skipped.\n", path, len);
		return -1;
	}

	rewind(fp);
	buf = (u8 *)malloc((size_t)len);
	if (!buf)
	{
		fclose(fp);
		printf("  %s\n     Out of memory, skipped.\n", path);
		return -1;
	}

	if (fread(buf, 1, (size_t)len, fp) != (size_t)len)
	{
		fclose(fp);
		free(buf);
		printf("  %s\n     Read error, skipped.\n", path);
		return -1;
	}
	fclose(fp);

	if (!cfg_magic_ok(buf))
	{
		free(buf);
		printf("  %s\n     Not a uLoader config (bad magic), skipped.\n", path);
		return -1;
	}

	printf("  %s\n", path);
	cfg_print_pin(buf);

	if (!cfg_pin_is_set(buf))
	{
		printf("     Already unlocked.\n");
		free(buf);
		return 1;
	}

	snprintf(backup, sizeof(backup), "%s.bak", path);
	fp = fopen(backup, "wb");
	if (!fp)
	{
		printf("     ERROR: cannot create backup; config was not modified.\n");
		free(buf);
		return -1;
	}

	ok = (fwrite(buf, 1, (size_t)len, fp) == (size_t)len);
	if (fclose(fp) != 0)
		ok = 0;

	if (!ok)
	{
		printf("     ERROR: backup write failed; config was not modified.\n");
		free(buf);
		return -1;
	}

	cfg_clear_pin(buf);

	fp = fopen(path, "wb");
	if (!fp)
	{
		printf("     ERROR: cannot rewrite config. Backup: %s\n", backup);
		free(buf);
		return -1;
	}

	ok = (fwrite(buf, 1, (size_t)len, fp) == (size_t)len);
	if (fclose(fp) != 0)
		ok = 0;

	if (!ok)
	{
		printf("     ERROR: write failed. Restore the backup: %s\n", backup);
		free(buf);
		return -1;
	}

	printf("     PIN removed. Backup: %s\n", backup);
	free(buf);
	return 2;
}

static void patch_all_fat(int *found, int *removed, int *errors)
{
	int i;

	printf("\n[FAT config]\n");

	for (i = 0; fat_volumes[i]; i++)
	{
		char path[128];
		int rc;

		snprintf(path, sizeof(path), "%s:/apps/uloader/uloader.cfg", fat_volumes[i]);
		rc = patch_fat_file(path);
		if (rc > 0)
		{
			(*found)++;
			if (rc == 2)
				(*removed)++;
		}
		else if (rc < 0)
			(*errors)++;

		snprintf(path, sizeof(path), "%s:/uloader.cfg", fat_volumes[i]);
		rc = patch_fat_file(path);
		if (rc > 0)
		{
			(*found)++;
			if (rc == 2)
				(*removed)++;
		}
		else if (rc < 0)
			(*errors)++;
	}
}

static void backup_wbfs_cfg(int part, const u8 *buf)
{
	char path[64];
	FILE *fp;
	int ok;

	snprintf(path, sizeof(path), "sd:/uloader-cfg-backup-wbfs%d.bin", part);
	fp = fopen(path, "wb");
	if (!fp)
	{
		printf("     (No SD card available; no backup written.)\n");
		return;
	}

	ok = (fwrite(buf, 1, CFG_SIZE, fp) == CFG_SIZE);
	if (fclose(fp) != 0)
		ok = 0;

	if (ok)
		printf("     Backup: %s\n", path);
	else
		printf("     WARNING: could not write the backup file.\n");
}

static void patch_wbfs(int *found, int *removed, int *errors)
{
	int i;

	printf("\n[WBFS config]\n");

	if (WBFS_Init() < 0)
	{
		printf("  No USB storage device found.\n");
		return;
	}

	if (WBFS_Open() < 0)
	{
		printf("  No WBFS partition found.\n");
		USBStorage2_Deinit();
		return;
	}

	for (i = 0; i < 4; i++)
	{
		u8 buf[CFG_SIZE];

		if (i > 0 && WBFS_Open2(i) < 0)
			continue;

		if (WBFS_ReadCfgRaw(buf, sizeof(buf)))
		{
			if (!cfg_magic_ok(buf))
			{
				printf("  WBFS partition %d: __CFG_ has an unknown format, skipped.\n", i);
				(*errors)++;
			}
			else
			{
				printf("  WBFS partition %d: __CFG_\n", i);
				cfg_print_pin(buf);
				(*found)++;

				if (!cfg_pin_is_set(buf))
					printf("     Already unlocked.\n");
				else
				{
					backup_wbfs_cfg(i, buf);
					cfg_clear_pin(buf);

					if (WBFS_WriteCfgRaw(buf, sizeof(buf)))
					{
						printf("     PIN removed.\n");
						(*removed)++;
					}
					else
					{
						printf("     ERROR: could not write the WBFS config.\n");
						(*errors)++;
					}
				}
			}
		}

		WBFS_Close();
	}

	USBStorage2_Deinit();
}

static void unmount_usb_fat(void)
{
	int i;
	for (i = 0; usb_volumes[i]; i++)
		fatUnmount(usb_volumes[i]);
}

int main(int argc, char **argv)
{
	int found = 0;
	int removed = 0;
	int errors = 0;

	(void)argc;
	(void)argv;

	video_init();

	printf("\x1b[2J\x1b[H");
	printf("uLoader Parental Control Unlocker\n");
	printf("----\n\n");
	printf("Press A to unlock.\n");
	fflush(stdout);

	if (!wait_confirm())
	{
		printf("\nCancelled. Nothing was changed.\n");
		printf("\nPress HOME to exit.\n");
		fflush(stdout);
		wait_any_button();
		return 0;
	}

	printf("\nMounting storage...\n");
	if (!fatInitDefault())
		printf("  No FAT volume was mounted (WBFS will still be checked).\n");

	patch_all_fat(&found, &removed, &errors);

	unmount_usb_fat();
	patch_wbfs(&found, &removed, &errors);

	printf("\n");
	if (removed)
		printf("Done. %d config(s) unlocked.\n", removed);
	else if (found)
		printf("Nothing to do, parental control was already disabled.\n");
	else
		printf("No uLoader configuration was found.\n");

	if (errors)
		printf("%d file(s) could not be processed. See messages above.\n", errors);

	if (removed)
		printf("\nStart uLoader again; it will no longer ask for the PIN.\n");

	printf("\nPress HOME to exit.\n");
	fflush(stdout);
	wait_any_button();

	return 0;
}
