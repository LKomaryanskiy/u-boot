/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <android_bootloader.h>
#include <android_bootloader_message.h>

#include <android_ab.h>
#include <cli.h>
#include <common.h>
#include <image.h>
#include <env.h>
#include <log.h>
#include <malloc.h>
#include <part.h>

#define ANDROID_PARTITION_BOOT "boot"
#define ANDROID_PARTITION_VENDOR_BOOT "vendor_boot"
#define ANDROID_PARTITION_RECOVERY "recovery"
#define ANDROID_PARTITION_SYSTEM "system"

#define ANDROID_ARG_SLOT_SUFFIX "androidboot.slot_suffix="
#define ANDROID_ARG_ROOT "root="

#define ANDROID_NORMAL_BOOT "androidboot.force_normal_boot=1"

static int android_bootloader_message_load(
	struct blk_desc *dev_desc,
	const struct disk_partition *part_info,
	struct bootloader_message *message)
{
	ulong message_blocks = sizeof(struct bootloader_message) /
	    part_info->blksz;
	if (message_blocks > part_info->size) {
		printf("misc partition too small.\n");
		return -1;
	}

	if (blk_dread(dev_desc, part_info->start, message_blocks, message) !=
	    message_blocks) {
		printf("Could not read from misc partition\n");
		return -1;
	}
	debug("ANDROID: Loaded BCB, %lu blocks.\n", message_blocks);
	return 0;
}

static int android_bootloader_message_write(
	struct blk_desc *dev_desc,
	const struct disk_partition *part_info,
	struct bootloader_message *message)
{
	ulong message_blocks = sizeof(struct bootloader_message) /
	    part_info->blksz;
	if (message_blocks > part_info->size) {
		printf("misc partition too small.\n");
		return -1;
	}

	if (blk_dwrite(dev_desc, part_info->start, message_blocks, message) !=
	    message_blocks) {
		printf("Could not write to misc partition\n");
		return -1;
	}
	debug("ANDROID: Wrote new BCB, %lu blocks.\n", message_blocks);
	return 0;
}

static enum android_boot_mode android_bootloader_load_and_clear_mode(
	struct blk_desc *dev_desc,
	const struct disk_partition *misc_part_info)
{
	struct bootloader_message bcb;

#ifdef CONFIG_FASTBOOT
	char *bootloader_str;

	/* Check for message from bootloader stored in RAM from a previous boot.
	 */
	bootloader_str = (char *)CONFIG_FASTBOOT_BUF_ADDR;
	if (!strcmp("reboot-bootloader", bootloader_str)) {
		bootloader_str[0] = '\0';
		return ANDROID_BOOT_MODE_BOOTLOADER;
	}
#endif

	/* Check and update the BCB message if needed. */
	if (android_bootloader_message_load(dev_desc, misc_part_info, &bcb) <
	    0) {
		printf("WARNING: Unable to load the BCB.\n");
		return ANDROID_BOOT_MODE_NORMAL;
	}

	if (!strcmp("bootonce-bootloader", bcb.command)) {
		/* Erase the message in the BCB since this value should be used
		 * only once.
		 */
		memset(bcb.command, 0, sizeof(bcb.command));
		android_bootloader_message_write(dev_desc, misc_part_info,
						 &bcb);
		return ANDROID_BOOT_MODE_BOOTLOADER;
	}

	if (!strcmp("boot-recovery", bcb.command))
		return ANDROID_BOOT_MODE_RECOVERY;

	return ANDROID_BOOT_MODE_NORMAL;
}

/**
 * Return the reboot reason string for the passed boot mode.
 *
 * @param mode	The Android Boot mode.
 * @return a pointer to the reboot reason string for mode.
 */
static const char *android_boot_mode_str(enum android_boot_mode mode)
{
	switch (mode) {
	case ANDROID_BOOT_MODE_NORMAL:
		return "(none)";
	case ANDROID_BOOT_MODE_RECOVERY:
		return "recovery";
	case ANDROID_BOOT_MODE_BOOTLOADER:
		return "bootloader";
	}
	return NULL;
}

static int android_part_get_info_by_name_suffix(struct blk_desc *dev_desc,
						const char *base_name,
						const char *slot_suffix,
						struct disk_partition *part_info)
{
	char *part_name;
	int part_num;
	size_t part_name_len;

	part_name_len = strlen(base_name) + 1;
	if (slot_suffix)
		part_name_len += strlen(slot_suffix);
	part_name = malloc(part_name_len);
	if (!part_name)
		return -1;
	strcpy(part_name, base_name);
	if (slot_suffix)
		strcat(part_name, slot_suffix);

	part_num = part_get_info_by_name(dev_desc, part_name, part_info);
	if (part_num < 0) {
		debug("ANDROID: Could not find partition \"%s\"\n", part_name);
		part_num = -1;
	}

	free(part_name);
	return part_num;
}

static int android_bootloader_boot_bootloader(void)
{
	const char *fastboot_cmd = env_get("fastbootcmd");

	if (fastboot_cmd)
		return run_command(fastboot_cmd, CMD_FLAG_ENV);
	return -1;
}

static char hex_to_char(uint8_t nibble) {
	if (nibble < 10) {
		return '0' + nibble;
	} else {
		return 'a' + nibble - 10;
	}
}
// Helper function to convert 32bit int to a hex string
static void hex_to_str(char* str, ulong input) {
	str[0] = '0'; str[1] = 'x';
	size_t str_idx = 2;
	uint32_t byte_extracted;
	uint8_t nibble;
	// Assume that this is on a little endian system.
	for(int byte_idx = 3; byte_idx >= 0; byte_idx--) {
		byte_extracted = ((0xFF << (byte_idx*8)) & input) >> (byte_idx*8);
		nibble = byte_extracted & 0xF0;
		nibble = nibble >> 4;
		nibble = nibble & 0xF;
		str[str_idx] = hex_to_char(nibble);
		str_idx++;
		nibble = byte_extracted & 0xF;
		str[str_idx] = hex_to_char(nibble);
		str_idx++;
	}
	str[str_idx] = 0;
}
__weak int android_bootloader_boot_kernel(const struct andr_boot_info* boot_info)
{
	ulong kernel_addr, kernel_size, ramdisk_addr, ramdisk_size;
	char *ramdisk_size_str, *fdt_addr = env_get("fdtaddr");
	char kernel_addr_str[12], ramdisk_addr_size_str[22];
	char *boot_args[] = {
		NULL, kernel_addr_str, ramdisk_addr_size_str, fdt_addr, NULL };

	if (android_image_get_kernel(boot_info, images.verify, NULL, &kernel_size))
		return -1;
	if (android_image_get_ramdisk(boot_info, &ramdisk_addr, &ramdisk_size))
		return -1;

	kernel_addr = android_image_get_kload(boot_info);
	hex_to_str(kernel_addr_str, kernel_addr);
	hex_to_str(ramdisk_addr_size_str, ramdisk_addr);
	ramdisk_size_str = &ramdisk_addr_size_str[strlen(ramdisk_addr_size_str)];
	*ramdisk_size_str = ':';
	hex_to_str(ramdisk_size_str + 1, ramdisk_size);

	printf("Booting kernel at %s with fdt at %s ramdisk %s...\n\n\n",
	       kernel_addr_str, fdt_addr, ramdisk_addr_size_str);
#if defined(CONFIG_ARM) && !defined(CONFIG_ARM64)
	do_bootz(NULL, 0, 4, boot_args);
#else
	do_booti(NULL, 0, 4, boot_args);
#endif

	return -1;
}

static char *strjoin(const char **chunks, char separator)
{
	int len, joined_len = 0;
	char *ret, *current;
	const char **p;

	for (p = chunks; *p; p++)
		joined_len += strlen(*p) + 1;

	if (!joined_len) {
		ret = malloc(1);
		if (ret)
			ret[0] = '\0';
		return ret;
	}

	ret = malloc(joined_len);
	current = ret;
	if (!ret)
		return ret;

	for (p = chunks; *p; p++) {
		len = strlen(*p);
		memcpy(current, *p, len);
		current += len;
		*current = separator;
		current++;
	}
	/* Replace the last separator by a \0. */
	current[-1] = '\0';
	return ret;
}

/** android_assemble_cmdline - Assemble the command line to pass to the kernel
 * @return a newly allocated string
 */
static char *android_assemble_cmdline(const char *slot_suffix,
				      const char *extra_args,
				      const bool normal_boot,
				      const char *android_kernel_cmdline,
				      const bool bootconfig_used)
{
	const char *cmdline_chunks[16];
	const char **current_chunk = cmdline_chunks;
	char *env_cmdline, *cmdline, *rootdev_input;
	char *allocated_suffix = NULL;
	char *allocated_rootdev = NULL;
	unsigned long rootdev_len;

	if (android_kernel_cmdline)
		*(current_chunk++) = android_kernel_cmdline;

	env_cmdline = env_get("bootargs");
	if (env_cmdline)
		*(current_chunk++) = env_cmdline;

	/* The |slot_suffix| needs to be passed to Android init to know what
	 * slot to boot from. This is done through bootconfig when supported.
	 */
	if (slot_suffix && !bootconfig_used) {
		allocated_suffix = malloc(strlen(ANDROID_ARG_SLOT_SUFFIX) +
					  strlen(slot_suffix));
		strcpy(allocated_suffix, ANDROID_ARG_SLOT_SUFFIX);
		strcat(allocated_suffix, slot_suffix);
		*(current_chunk++) = allocated_suffix;
	}

	rootdev_input = env_get("android_rootdev");
	if (rootdev_input) {
		rootdev_len = strlen(ANDROID_ARG_ROOT) + CONFIG_SYS_CBSIZE + 1;
		allocated_rootdev = malloc(rootdev_len);
		strcpy(allocated_rootdev, ANDROID_ARG_ROOT);
		cli_simple_process_macros(rootdev_input,
					  allocated_rootdev +
					  strlen(ANDROID_ARG_ROOT));
		/* Make sure that the string is null-terminated since the
		 * previous could not copy to the end of the input string if it
		 * is too big.
		 */
		allocated_rootdev[rootdev_len - 1] = '\0';
		*(current_chunk++) = allocated_rootdev;
	}

	if (extra_args) {
		*(current_chunk++) = extra_args;
	}

	/* The force_normal_boot param must be passed to android's init sequence
	 * to avoid booting into recovery mode. This is done through bootconfig when
	 * supported.
	 * Refer to link below under "Early Init Boot Sequence"
	 * https://source.android.com/devices/architecture/kernel/mounting-partitions-early
	 */
	if (normal_boot && !bootconfig_used) {
		*(current_chunk++) = ANDROID_NORMAL_BOOT;
	}

	*(current_chunk++) = NULL;
	cmdline = strjoin(cmdline_chunks, ' ');
	free(allocated_suffix);
	free(allocated_rootdev);
	return cmdline;
}

int android_bootloader_boot_flow(struct blk_desc *dev_desc,
				 const struct disk_partition *misc_part_info,
				 const char *slot,
				 unsigned long kernel_address)
{
	enum android_boot_mode mode;
	struct disk_partition boot_part_info;
	struct disk_partition vendor_boot_part_info;
	int boot_part_num, vendor_boot_part_num;
	char *command_line;
	char slot_suffix[3];
	const char *mode_cmdline = NULL;
	const char *boot_partition = ANDROID_PARTITION_BOOT;
	const char *vendor_boot_partition = ANDROID_PARTITION_VENDOR_BOOT;
#ifdef CONFIG_ANDROID_SYSTEM_AS_ROOT
	int system_part_num
	struct disk_partition system_part_info;
#endif

	/* Determine the boot mode and clear its value for the next boot if
	 * needed.
	 */
	mode = android_bootloader_load_and_clear_mode(dev_desc, misc_part_info);
	printf("ANDROID: reboot reason: \"%s\"\n", android_boot_mode_str(mode));
	// TODO (rammuthiah) fastboot isn't suported on cuttlefish yet.
	// Once it is, these lines can be removed.
	if (mode == ANDROID_BOOT_MODE_BOOTLOADER) {
		mode = ANDROID_BOOT_MODE_NORMAL;
	}

	bool normal_boot = (mode == ANDROID_BOOT_MODE_NORMAL);
	switch (mode) {
	case ANDROID_BOOT_MODE_NORMAL:
#ifdef CONFIG_ANDROID_SYSTEM_AS_ROOT
		/* In normal mode, we load the kernel from "boot" but append
		 * "skip_initramfs" to the cmdline to make it ignore the
		 * recovery initramfs in the boot partition.
		 */
		mode_cmdline = "skip_initramfs";
#endif
		break;
	case ANDROID_BOOT_MODE_RECOVERY:
#if defined(CONFIG_ANDROID_SYSTEM_AS_ROOT) || defined(CONFIG_ANDROID_USES_RECOVERY_AS_BOOT)
		/* In recovery mode we still boot the kernel from "boot" but
		 * don't skip the initramfs so it boots to recovery.
		 * If on Android device using Recovery As Boot, there is no
		 * recovery  partition.
		 */
#else
		boot_partition = ANDROID_PARTITION_RECOVERY;
#endif
		break;
	case ANDROID_BOOT_MODE_BOOTLOADER:
		/* Bootloader mode enters fastboot. If this operation fails we
		 * simply return since we can't recover from this situation by
		 * switching to another slot.
		 */
		return android_bootloader_boot_bootloader();
	}

	/* Look for an optional slot suffix override. */
	if (!slot || !slot[0])
		slot = env_get("android_slot_suffix");

	slot_suffix[0] = '\0';
	if (slot && slot[0]) {
		slot_suffix[0] = '_';
		slot_suffix[1] = slot[0];
		slot_suffix[2] = '\0';
	} else {
#ifdef CONFIG_ANDROID_AB
		int slot_num = ab_select_slot(dev_desc, misc_part_info, normal_boot);
		if (slot_num < 0) {
			log_err("Could not determine Android boot slot.\n");
			return -1;
		}
		slot_suffix[0] = '_';
		slot_suffix[1] = BOOT_SLOT_NAME(slot_num);
		slot_suffix[2] = '\0';
#endif
	}

	/* Load the kernel from the desired "boot" partition. */
	boot_part_num =
	    android_part_get_info_by_name_suffix(dev_desc, boot_partition,
						 slot_suffix, &boot_part_info);
	/* Load the vendor boot partition if there is one. */
	vendor_boot_part_num =
	    android_part_get_info_by_name_suffix(dev_desc, vendor_boot_partition,
						 slot_suffix,
						 &vendor_boot_part_info);
	if (boot_part_num < 0)
		return -1;
	debug("ANDROID: Loading kernel from \"%s\", partition %d.\n",
	      boot_part_info.name, boot_part_num);

#ifdef CONFIG_ANDROID_SYSTEM_AS_ROOT
	system_part_num =
	    android_part_get_info_by_name_suffix(dev_desc,
						 ANDROID_PARTITION_SYSTEM,
						 slot_suffix,
						 &system_part_info);
	if (system_part_num < 0)
		return -1;
	debug("ANDROID: Using system image from \"%s\", partition %d.\n",
	      system_part_info.name, system_part_num);
#endif

	struct disk_partition *vendor_boot_part_info_ptr = &vendor_boot_part_info;
	if (vendor_boot_part_num < 0) {
		vendor_boot_part_info_ptr = NULL;
	} else {
		printf("ANDROID: Loading vendor ramdisk from \"%s\", partition"
		       " %d.\n", vendor_boot_part_info.name,
		       vendor_boot_part_num);
	}

	struct andr_boot_info* boot_info = android_image_load(dev_desc, &boot_part_info,
				 vendor_boot_part_info_ptr,
				 kernel_address, slot_suffix, normal_boot);

	if (!boot_info)
		return -1;

#ifdef CONFIG_ANDROID_SYSTEM_AS_ROOT
	/* Set Android root variables. */
	env_set_ulong("android_root_devnum", dev_desc->devnum);
	env_set_ulong("android_root_partnum", system_part_num);
#endif
	env_set("android_slotsufix", slot_suffix);

	/* Assemble the command line */
	command_line = android_assemble_cmdline(slot_suffix, mode_cmdline, normal_boot,
							android_image_get_kernel_cmdline(boot_info),
							android_image_is_bootconfig_used(boot_info));
	env_set("bootargs", command_line);

	debug("ANDROID: bootargs: \"%s\"\n", command_line);
	android_bootloader_boot_kernel(boot_info);

	/* TODO: If the kernel doesn't boot mark the selected slot as bad. */
	return -1;
}
