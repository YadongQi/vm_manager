/*
 * Copyright (c) 2020 Intel Corporation.
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <libgen.h>
#include <uuid/uuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "vm_manager.h"
#include "guest.h"
#include "utils.h"

extern keyfile_group_t g_group[];

static const char *fixed_cmd =
	" -machine type=q35,kernel_irqchip=off"
	" -k en-us"
	" -cpu host -vga none"
	" -enable-kvm"
	" -device qemu-xhci,id=xhci,addr=0x8"
	" -device usb-mouse"
	" -device usb-kbd"
	" -device intel-hda -device hda-duplex"
	" -audiodev id=android_spk,timer-period=5000,driver=pa"
	" -device e1000,netdev=net0"
	" -device intel-iommu,device-iotlb=off,caching-mode=on"
	" -nodefaults ";

static int create_vgpu(GKeyFile *gkf)
{
	int fd = 0;
	ssize_t n = 0;
	g_autofree gchar *gvtg_ver = NULL, *uuid = NULL;
	keyfile_group_t *g = &g_group[GROUP_VGPU];
	char file[MAX_PATH] = { 0 };

	gvtg_ver = g_key_file_get_string(gkf, g->name, g->key[VGPU_GVTG_VER], NULL);
	if (gvtg_ver == NULL) {
		g_warning("Invalid GVTg version\n");
		return -1;
	}
	snprintf(file, MAX_PATH, GVTG_MDEV_TYPE_PATH"%s/create", gvtg_ver);

	uuid = g_key_file_get_string(gkf, g->name, g->key[VGPU_UUID], NULL);
	if (uuid == NULL) {
		g_warning("Invalid VGPU UUID\n");
		return -1;
	}

	fd = open(file, O_WRONLY);
	if (fd == -1) {
		fprintf(stderr, "open %s failed, errno=%d\n", file, errno);
		return -1;
	}

	n = strlen(uuid);
	if (write(fd, uuid, n) != n) {
		fprintf(stderr, "write %s failed, errno=%d\n", file, errno);
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static int passthrough_gpu(void)
{
	int fd = 0;
	ssize_t n = 0;
	int dev_id = 0;
	char buf[64] = { 0 };
	struct stat st;
	int pid;
	int wst;

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "%s: Failed to fork.", __func__);
		return -1;
	} else if (pid == 0) {
		execlp("modprobe", "modprobe", "vfio", NULL);
		return -1;
	} else {
		wait(&wst);
		if (!(WIFEXITED(wst) && !WEXITSTATUS(wst))) {
			fprintf(stderr, "Failed to load module: vfio\n");
			return -1;
		}
	}

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "%s: Failed to fork.", __func__);
		return -1;
	} else if (pid == 0) {
		execlp("modprobe", "modprobe", "vfio-pci", NULL);
		return -1;
	} else {
		wait(&wst);
		if (!(WIFEXITED(wst) && !WEXITSTATUS(wst))) {
			fprintf(stderr, "Failed to load module: vfio-pci\n");
			return -1;
		}
	}

	/* Get device ID */
	fd = open(INTEL_GPU_DEV_PATH"/device", O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "open %s failed, errno=%d\n", INTEL_GPU_DEV_PATH"/device", errno);
		return -1;
	}

	n = read(fd, buf, sizeof(buf));
	if (n == -1) {
		fprintf(stderr, "read %s failed, errno=%d\n", INTEL_GPU_DEV_PATH"/device", errno);
		close(fd);
		return -1;
	}
	dev_id = strtol(buf, NULL, 16);

	close(fd);

	if (stat(INTEL_GPU_DEV_PATH"/driver", &st) == 0) {
		/* Unbind original driver */
		fd = open(INTEL_GPU_DEV_PATH"/driver/unbind", O_WRONLY);
		if (fd == -1) {
			fprintf(stderr, "open %s failed, errno=%d\n", INTEL_GPU_DEV_PATH"/driver/unbind", errno);
			return -1;
		}

		n = strlen(INTEL_GPU_BDF);
		if (write(fd, INTEL_GPU_BDF, n) != n) {
			fprintf(stderr, "write %s failed, errno=%d\n", INTEL_GPU_DEV_PATH"/driver/unbind", errno);
			close(fd);
			return -1;
		}

		close(fd);
	}

	/* Create new vfio id for GPU */
	fd = open(PCI_DRIVER_PATH"vfio-pci/new_id", O_WRONLY);
	if (fd == -1) {
		fprintf(stderr, "open %s failed, errno=%d\n", PCI_DRIVER_PATH"vfio-pci/new_id", errno);
		return -1;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "8086 %x", dev_id);
	n = strlen(buf);
	if (write(fd, buf, n) != n) {
		fprintf(stderr, "write %s failed, errno=%d\n", PCI_DRIVER_PATH"vfio-pci/new_id", errno);
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

int start_guest(char *name)
{
	int ret;
	int cx;
	GKeyFile *gkf;
	g_autofree gchar *val = NULL;
	char cfg_file[MAX_PATH] = { 0 };
	char emu_path[MAX_PATH] = { 0 };
	char cmd_str[MAX_CMDLINE_LEN] = { 0 };
	char *p = &cmd_str[0];
	size_t size = sizeof(cmd_str);
	keyfile_group_t *g = NULL;

	if (!name) {
		fprintf(stderr, "%s: Invalid input param\n", __func__);
		return -1;
	}

	snprintf(cfg_file, sizeof(cfg_file), "%s/%s.ini", civ_config_path, name);

	gkf = g_key_file_new();

	if (!g_key_file_load_from_file(gkf, cfg_file, G_KEY_FILE_NONE, NULL)) {
		g_warning("Error loading ini file :%s", cfg_file);
		return -1;
	}

	g = &g_group[GROUP_EMUL];
	val = g_key_file_get_string(gkf, g->name, g->key[EMUL_PATH], NULL);
	if (val == NULL) {
		g_warning("cannot find key emulator path from group general\n");
		return -1;
	}
	snprintf(emu_path, sizeof(emu_path), "%s", val);
	fprintf(stderr, "emu_path: %s\n", emu_path);

	g = &g_group[GROUP_GLOB];
	val = g_key_file_get_string(gkf, g->name, g->key[GLOB_NAME], NULL);
	if (val == NULL) {
		g_warning("cannot find key name from group global\n");
		return -1;
	}
	cx = snprintf(p, size, " -name %s", val);
	p += cx; size -= cx;

	cx = snprintf(p, size, " -qmp unix:%s/.%s"CIV_GUEST_QMP_SUFFIX",server,nowait", civ_config_path, val);
	p += cx; size -= cx;

	val = g_key_file_get_string(gkf, g->name, g->key[GLOB_ADB_PORT], NULL);
	cx = snprintf(p, size, " -netdev user,id=net0,hostfwd=tcp::%s-:5555", val);
	p += cx; size -= cx;

	val = g_key_file_get_string(gkf, g->name, g->key[GLOB_FASTBOOT_PORT], NULL);
	cx = snprintf(p, size, ",hostfwd=tcp::%s-:5554", val);
	p += cx; size -= cx;

	g = &g_group[GROUP_MEM];
	val = g_key_file_get_string(gkf, g->name, g->key[MEM_SIZE], NULL);
	if (val == NULL) {
		g_warning("cannot find key name from group memory\n");
		return -1;
	}
	cx = snprintf(p, size, " -m %s", val);
	p += cx; size -= cx;

	g = &g_group[GROUP_VCPU];
	val = g_key_file_get_string(gkf, g->name, g->key[VCPU_NUM], NULL);
	if (val == NULL) {
		g_warning("cannot find key name from group vcpu\n");
		return -1;
	}
	cx = snprintf(p, size, " -smp %s", val);
	p += cx; size -= cx;

	g = &g_group[GROUP_FIRM];
	val = g_key_file_get_string(gkf, g->name, g->key[FIRM_TYPE], NULL);
	if (val == NULL) {
		g_warning("cannot find key name from group firmware\n");
		return -1;
	}
	if (strcmp(val, FIRM_OPTS_UNIFIED_STR) == 0) {
		val = g_key_file_get_string(gkf, g->name, g->key[FIRM_PATH], NULL);
		cx = snprintf(p, size, " -drive if=pflash,format=raw,file=%s", val);
		p += cx; size -= cx;
	} else if (strcmp(val, FIRM_OPTS_SPLITED_STR) == 0) {
		val = g_key_file_get_string(gkf, g->name, g->key[FIRM_CODE], NULL);
		cx = snprintf(p, size, " -drive if=pflash,format=raw,readonly,file=%s", val);
		p += cx; size -= cx;
		val = g_key_file_get_string(gkf, g->name, g->key[FIRM_VARS], NULL);
		cx = snprintf(p, size, " -drive if=pflash,format=raw,file=%s", val);
		p += cx; size -= cx;
	} else {
		g_warning("cannot find firmware sub-key\n");
		return -1;
	}

	g = &g_group[GROUP_DISK];
	val = g_key_file_get_string(gkf, g->name, g->key[DISK_PATH], NULL);
	if (val == NULL) {
		g_warning("cannot find key name from group disk\n");
		return -1;
	}
	cx = snprintf(p, size, " -drive file=%s,if=none,id=disk1 -device virtio-blk-pci,drive=disk1,bootindex=1", val);
	p += cx; size -= cx;

	g = &g_group[GROUP_VGPU];
	val = g_key_file_get_string(gkf, g->name, g->key[VGPU_TYPE], NULL);
	if (val == NULL) {
		g_warning("cannot find key name from group graphics\n");
		return -1;
	}
	if (strcmp(val, VGPU_OPTS_GVTG_STR) == 0) {
		char vgpu_path[MAX_PATH] = { 0 };
		struct stat st;
		val = g_key_file_get_string(gkf, g->name, g->key[VGPU_UUID], NULL);
		if (val == NULL) {
			g_warning("Invalid VGPU\n");
			return -1;
		}

		if (check_uuid(val) != 0) {
			fprintf(stderr, "Invalid UUID format!\n");
			return -1;
		}

		snprintf(vgpu_path, sizeof(vgpu_path), INTEL_GPU_DEV_PATH"/%s", val);
		if (stat(vgpu_path, &st) != 0) {
			if (create_vgpu(gkf) == -1) {
				g_warning("failed to create vGPU\n");
				return -1;
			}
		}
		cx = snprintf(p, size, " -display gtk,gl=on -device vfio-pci-nohotplug,ramfb=on,sysfsdev=%s,display=on,x-igd-opregion=on", vgpu_path);
		p += cx; size -= cx;
	} else if (strcmp(val, VGPU_OPTS_GVTD_STR) == 0) {
		if (passthrough_gpu())
			return -1;
		cx = snprintf(p, size, " -vga none -nographic -device vfio-pci,host=00:02.0,x-igd-gms=2,id=hostdev0,bus=pcie.0,addr=0x2,x-igd-opregion=on");
		p += cx; size -= cx;
	} else if (strcmp(val, VGPU_OPTS_VIRTIO_STR) == 0) {
		cx = snprintf(p, size, " -display gtk,gl=on -device virtio-gpu-pci");
		p += cx; size -= cx;
	} else if (strcmp(val, VGPU_OPTS_SW_STR) == 0) {
		cx = snprintf(p, size, " -display gtk,gl=on -device qxl-vga,xres=480,yres=360");
		p += cx; size -= cx;
	} else {
		g_warning("Invalid Graphics config\n");
		return -1;
	}

	g_autofree gchar **extra_keys = NULL;
	gsize len = 0, i;
	extra_keys = g_key_file_get_keys(gkf, "extra", &len, NULL);
	for (i = 0; i < len; i++) {
		val = g_key_file_get_string(gkf, "extra", extra_keys[i], NULL);
		cx = snprintf(p, size, " %s", val);
		p += cx; size -= cx;
	}

	cx = snprintf(p, size, "%s", fixed_cmd);
	p += cx; size -= cx;

	fprintf(stderr, "run: %s %s\n", emu_path, cmd_str);

	ret = execute_cmd(emu_path, cmd_str, strlen(cmd_str));
	if (ret != 0) {
		err(1, "%s:Failed to execute emulator command, err=%d\n", __func__, errno);
		return -1;
	}

	g_key_file_free(gkf);
	return 0;
}
