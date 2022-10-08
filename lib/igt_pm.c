/*
 * Copyright © 2013, 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Paulo Zanoni <paulo.r.zanoni@intel.com>
 *    David Weinehall <david.weinehall@intel.com>
 *
 */
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <dirent.h>

#include "drmtest.h"
#include "igt_kms.h"
#include "igt_pm.h"
#include "igt_aux.h"
#include "igt_sysfs.h"

/**
 * SECTION:igt_pm
 * @short_description: Power Management related helpers
 * @title: Power Management
 * @include: igt.h
 *
 * This library provides various helpers to enable power management for,
 * and in some cases subsequently allow restoring the old behaviour of,
 * various external components that by default are set up in a way
 * that interferes with the testing of our power management functionality.
 */

enum {
	POLICY_UNKNOWN = -1,
	POLICY_MAX_PERFORMANCE = 0,
	POLICY_MEDIUM_POWER = 1,
	POLICY_MIN_POWER = 2
};

#define MSR_PKG_CST_CONFIG_CONTROL	0xE2
/*
 * Below PKG CST limit mask and PC8 bits are meant for
 * HSW,BDW SKL,ICL and Goldmont Microarch and future platforms.
 * Refer IA S/W developers manual vol3c part3 chapter:35
 */
#define  PKG_CST_LIMIT_MASK		0xF
#define  PKG_CST_LIMIT_C8		0x6

#define MAX_PERFORMANCE_STR	"max_performance\n"
#define MEDIUM_POWER_STR	"medium_power\n"
#define MIN_POWER_STR		"min_power\n"
/* Remember to fix this if adding longer strings */
#define MAX_POLICY_STRLEN	strlen(MAX_PERFORMANCE_STR)
int8_t *__sata_pm_policies;
int __scsi_host_cnt;

static int __igt_pm_power = -1;

static char __igt_pm_audio_runtime_power_save[64];
static char * __igt_pm_audio_runtime_control_path;
static char __igt_pm_audio_runtime_control[64];

static void __igt_pm_sata_link_pm_exit_handler(int sig);
static void __igt_pm_restore_sata_link_power_management(void);

static int find_runtime_pm(int device)
{
	char path[128];
	struct stat st;

	if (fstat(device, &st) || !S_ISCHR(st.st_mode))
		return -1;

	snprintf(path, sizeof(path), "/sys/dev/char/%d:%d/device/power",
		 major(st.st_rdev), minor(st.st_rdev));

	return open(path, O_RDONLY);
}

static int __igt_pm_audio_restore_runtime_pm(void)
{
	int fd;

	if (!__igt_pm_audio_runtime_power_save[0])
		return 0;

	fd = open("/sys/module/snd_hda_intel/parameters/power_save", O_WRONLY);
	if (fd < 0)
		return errno;

	if (write(fd, __igt_pm_audio_runtime_power_save,
		  strlen(__igt_pm_audio_runtime_power_save)) !=
	    strlen(__igt_pm_audio_runtime_power_save)) {
		close(fd);
		return errno;
	}

	close(fd);

	fd = open(__igt_pm_audio_runtime_control_path, O_WRONLY);
	if (fd < 0)
		return errno;

	if (write(fd, __igt_pm_audio_runtime_control,
		  strlen(__igt_pm_audio_runtime_control)) !=
	    strlen(__igt_pm_audio_runtime_control)) {
		close(fd);
		return errno;
	}

	close(fd);

	memset(__igt_pm_audio_runtime_power_save, 0,
	       sizeof(__igt_pm_audio_runtime_power_save));

	memset(__igt_pm_audio_runtime_control, 0,
	       sizeof(__igt_pm_audio_runtime_control));

	free(__igt_pm_audio_runtime_control_path);
	__igt_pm_audio_runtime_control_path = NULL;

	return 0;
}

static void igt_pm_audio_restore_runtime_pm(void)
{
	int ret;

	if (!__igt_pm_audio_runtime_power_save[0])
		return;

	igt_debug("Restoring audio power management to '%s' and '%s'\n",
		  __igt_pm_audio_runtime_power_save,
		  __igt_pm_audio_runtime_control);

	ret = __igt_pm_audio_restore_runtime_pm();
	if (ret)
		igt_warn("Failed to restore runtime audio PM! (errno=%d)\n",
			 ret);
}

static void __igt_pm_audio_runtime_exit_handler(int sig)
{
	__igt_pm_audio_restore_runtime_pm();
}

static void strchomp(char *str)
{
	int len = strlen(str);

	if (len && str[len - 1] == '\n')
		str[len - 1] = 0;
}

static int __igt_pm_enable_audio_runtime_pm(void)
{
	char *path = NULL;
	struct dirent *de;
	DIR *dir;
	int err;
	int fd;

	dir = opendir("/sys/class/sound");
	if (!dir)
		return 0;

	/* Find PCI device claimed by snd_hda_intel and tied to i915. */
	while ((de = readdir(dir))) {
		const char *match = "hwC";
		char buf[32] = { }; /* for Valgrind */
		int loops = 500;
		int base;
		int ret;

		if (de->d_type != DT_LNK ||
		    strncmp(de->d_name, match, strlen(match)))
			continue;

		base = openat(dirfd(dir), de->d_name, O_RDONLY);
		igt_assert_fd(base);

		do {
			fd = openat(base, "vendor_name", O_RDONLY);
			if (fd < 0) /* module is still loading? */
				usleep(1000);
			else
				break;
		} while (--loops);
		close(base);
		if (fd < 0)
			continue;

		ret = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		igt_assert(ret > 0);
		buf[ret] = '\0';
		strchomp(buf);

		/* Realtek and similar devices are not what we are after. */
		if (strcmp(buf, "Intel"))
			continue;

		igt_assert(asprintf(&path,
				    "/sys/class/sound/%s/device/device/power/control",
				    de->d_name));

		igt_debug("Audio device path is %s\n", path);
		break;
	}
	closedir(dir);

	fd = open("/sys/module/snd_hda_intel/parameters/power_save", O_RDWR);
	if (fd < 0)
		return 0;

	/* snd_hda_intel loaded but no path found is an error. */
	if (!path) {
		close(fd);
		err = -ESRCH;
		goto err;
	}

	igt_assert(read(fd, __igt_pm_audio_runtime_power_save,
			sizeof(__igt_pm_audio_runtime_power_save) - 1) > 0);
	strchomp(__igt_pm_audio_runtime_power_save);
	igt_install_exit_handler(__igt_pm_audio_runtime_exit_handler);
	igt_assert_eq(write(fd, "1\n", 2), 2);
	close(fd);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		err = -errno;
		goto err;
	}

	igt_assert(read(fd, __igt_pm_audio_runtime_control,
			sizeof(__igt_pm_audio_runtime_control) - 1) > 0);
	strchomp(__igt_pm_audio_runtime_control);
	igt_assert_eq(write(fd, "auto\n", 5), 5);
	close(fd);

	__igt_pm_audio_runtime_control_path = path;

	igt_debug("Saved audio power management as '%s' and '%s'\n",
		  __igt_pm_audio_runtime_power_save,
		  __igt_pm_audio_runtime_control);

	/* Give some time for it to react. */
	sleep(1);
	return 0;

err:
	free(path);
	return err;
}

/**
 * igt_pm_enable_audio_runtime_pm:
 *
 * We know that if we don't enable audio runtime PM, snd_hda_intel will never
 * release its power well refcount, and we'll never reach the LPSP state.
 * There's no guarantee that it will release the power well if we enable
 * runtime PM, but at least we can try.
 *
 * We don't have any assertions on open since the user may not even have
 * snd_hda_intel loaded, which is not a problem.
 */
void igt_pm_enable_audio_runtime_pm(void)
{
	int err;

	/* Check if already enabled. */
	if (__igt_pm_audio_runtime_power_save[0])
		return;

	for (int count = 0; count < 110; count++) {
		if (!__igt_pm_enable_audio_runtime_pm())
			return;

		/* modprobe(sna-hda-intel) acts async so poll for sysfs */
		if (count < 100)
			usleep(10 * 1000); /* poll at 10ms for the first 1s */
		else
			sleep(1);
	}

	err = __igt_pm_enable_audio_runtime_pm();
	if (err)
		igt_debug("Failed to enable audio runtime PM! (%d)\n", -err);
}

static void __igt_pm_enable_sata_link_power_management(void)
{
	int fd, i;
	ssize_t len;
	char *buf;
	char *file_name;
	int8_t policy;

	file_name = malloc(PATH_MAX);
	buf = malloc(MAX_POLICY_STRLEN + 1);

	for (__scsi_host_cnt = 0; ; __scsi_host_cnt++) {
		snprintf(file_name, PATH_MAX,
			 "/sys/class/scsi_host/host%d/link_power_management_policy",
			 __scsi_host_cnt);

		/*
		 * We don't have any assertions on open since the system
		 * might not have a SATA host.
		 */
		fd = open(file_name, O_RDWR);
		if (fd < 0)
			break;

		len = read(fd, buf, MAX_POLICY_STRLEN);
		buf[len] = '\0';

		if (!strncmp(MAX_PERFORMANCE_STR, buf,
			     strlen(MAX_PERFORMANCE_STR)))
			policy = POLICY_MAX_PERFORMANCE;
		else if (!strncmp(MEDIUM_POWER_STR, buf,
				  strlen(MEDIUM_POWER_STR)))
			policy = POLICY_MEDIUM_POWER;
		else if (!strncmp(MIN_POWER_STR, buf,
				  strlen(MIN_POWER_STR)))
			policy = POLICY_MIN_POWER;
		else
			policy = POLICY_UNKNOWN;

		if (!(__scsi_host_cnt % 256))
			__sata_pm_policies = realloc(__sata_pm_policies,
						     (__scsi_host_cnt / 256 + 1)
						     * 256 + 1);

		__sata_pm_policies[__scsi_host_cnt] = policy;
		close(fd);
	}

	igt_install_exit_handler(__igt_pm_sata_link_pm_exit_handler);

	for (i = 0; i < __scsi_host_cnt; i++) {
		snprintf(file_name, PATH_MAX,
			 "/sys/class/scsi_host/host%d/link_power_management_policy",
			 i);
		fd = open(file_name, O_RDWR);
		if (fd < 0)
			break;

		policy = __sata_pm_policies[i];

		/* If the policy is something we don't know about,
		 * don't touch it, since we might potentially break things.
		 * And we obviously don't need to touch anything if the
		 * setting is already correct...
		 */
		if (policy != POLICY_UNKNOWN &&
		    policy != POLICY_MIN_POWER) {
			lseek(fd, 0, SEEK_SET);
			igt_assert_eq(write(fd, MIN_POWER_STR,
					    strlen(MIN_POWER_STR)),
				      strlen(MIN_POWER_STR));
		}
		close(fd);
	}
	free(buf);
	free(file_name);
}

static void __igt_pm_restore_sata_link_power_management(void)
{
	int fd, i;
	char *file_name;

	if (!__sata_pm_policies)
		return;

	/* Disk runtime PM policies. */
	file_name = malloc(PATH_MAX);
	for (i = 0; i < __scsi_host_cnt; i++) {
		int8_t policy;

		if (__sata_pm_policies[i] == POLICY_UNKNOWN)
			continue;
		else
			policy = __sata_pm_policies[i];

		snprintf(file_name, PATH_MAX,
			 "/sys/class/scsi_host/host%d/link_power_management_policy",
			 i);

		fd = open(file_name, O_WRONLY);
		if (fd < 0)
			break;

		switch (policy) {
		default:
		case POLICY_MAX_PERFORMANCE:
			igt_assert_eq(write(fd, MAX_PERFORMANCE_STR,
					    strlen(MAX_PERFORMANCE_STR)),
				      strlen(MAX_PERFORMANCE_STR));
			break;

		case POLICY_MEDIUM_POWER:
			igt_assert_eq(write(fd, MEDIUM_POWER_STR,
					    strlen(MEDIUM_POWER_STR)),
				      strlen(MEDIUM_POWER_STR));
			break;

		case POLICY_MIN_POWER:
			igt_assert_eq(write(fd, MIN_POWER_STR,
					    strlen(MIN_POWER_STR)),
				      strlen(MIN_POWER_STR));
			break;
		}

		close(fd);
	}
	free(file_name);
	free(__sata_pm_policies);
	__sata_pm_policies = NULL;
}

/**
 * igt_pm_enable_sata_link_power_management:
 *
 * Enable the min_power policy for SATA link power management.
 * Without this we cannot reach deep runtime power states.
 */
void igt_pm_enable_sata_link_power_management(void)
{
	/* Check if has been already saved. */
	if (__sata_pm_policies)
		return;

	 __igt_pm_enable_sata_link_power_management();
}

/**
 * igt_pm_restore_sata_link_power_management:
 *
 * Restore the link power management policies to the values
 * prior to enabling min_power.
 *
 * Caveat: If the system supports hotplugging and hotplugging takes
 *         place during our testing so that the hosts change numbers
 *         we might restore the settings to the wrong hosts.
 */
void igt_pm_restore_sata_link_power_management(void)
{
	if (!__sata_pm_policies)
		return;

	 __igt_pm_restore_sata_link_power_management();
}

static void __igt_pm_sata_link_pm_exit_handler(int sig)
{
	__igt_pm_restore_sata_link_power_management();
}

static char __igt_pm_runtime_autosuspend[64];
static char __igt_pm_runtime_control[64];

static int __igt_restore_runtime_pm(void)
{
	int fd;

	if (__igt_pm_power < 0)
		return 0;

	fd = openat(__igt_pm_power, "autosuspend_delay_ms", O_WRONLY);
	if (fd < 0)
		return errno;

	if (write(fd, __igt_pm_runtime_autosuspend,
		  strlen(__igt_pm_runtime_autosuspend)) !=
	    strlen(__igt_pm_runtime_autosuspend)) {
		close(fd);
		return errno;
	}

	close(fd);

	fd = openat(__igt_pm_power, "control", O_WRONLY);
	if (fd < 0)
		return errno;

	if (write(fd, __igt_pm_runtime_control,
		  strlen(__igt_pm_runtime_control)) !=
	    strlen(__igt_pm_runtime_control)) {
		close(fd);
		return errno;
	}

	close(fd);

	close(__igt_pm_power);
	__igt_pm_power = -1;

	return 0;
}

/**
 * igt_restore_runtime_pm:
 *
 * Restores the runtime PM configuration as it was before the call to
 * igt_setup_runtime_pm.
 */
void igt_restore_runtime_pm(void)
{
	int ret;

	if (__igt_pm_power < 0)
		return;

	igt_debug("Restoring runtime PM management to '%s' and '%s'\n",
		  __igt_pm_runtime_autosuspend,
		  __igt_pm_runtime_control);

	ret = __igt_restore_runtime_pm();
	if (ret)
		igt_warn("Failed to restore runtime PM! (errno=%d)\n", ret);

	igt_pm_audio_restore_runtime_pm();
}

static void __igt_pm_runtime_exit_handler(int sig)
{
	__igt_restore_runtime_pm();
}

/**
 * igt_setup_runtime_pm:
 *
 * Sets up the runtime PM helper functions and enables runtime PM. To speed up
 * tests the autosuspend delay is set to 0.
 *
 * Returns:
 * True if runtime pm is available, false otherwise.
 */
bool igt_setup_runtime_pm(int device)
{
	int fd;
	ssize_t size;
	char buf[6];

	if (__igt_pm_power != -1) /* XXX assume it's the same device! */
		return true;

	__igt_pm_power = find_runtime_pm(device);
	if (__igt_pm_power < 0)
		return false;

	igt_pm_enable_audio_runtime_pm();

	/*
	 * Our implementation uses autosuspend. Try to set it to 0ms so the
	 * test suite goes faster and we have a higher probability of
	 * triggering race conditions.
	 */
	fd = openat(__igt_pm_power, "autosuspend_delay_ms", O_RDWR);
	if (fd < 0) {
		igt_pm_audio_restore_runtime_pm();
		close(__igt_pm_power);
		__igt_pm_power = -1;
		return false;
	}

	/*
	 * Save previous values to be able to  install exit handler to restore
	 * them on test exit.
	 */
	size = read(fd, __igt_pm_runtime_autosuspend,
		    sizeof(__igt_pm_runtime_autosuspend) - 1);

	/*
	 * If we fail to read from the file, it means this system doesn't
	 * support runtime PM.
	 */
	if (size <= 0) {
		close(fd);
		igt_pm_audio_restore_runtime_pm();
		close(__igt_pm_power);
		__igt_pm_power = -1;
		return false;
	}

	__igt_pm_runtime_autosuspend[size] = '\0';

	strchomp(__igt_pm_runtime_autosuspend);
	igt_install_exit_handler(__igt_pm_runtime_exit_handler);

	size = write(fd, "0\n", 2);

	close(fd);

	if (size != 2) {
		close(__igt_pm_power);
		__igt_pm_power = -1;
		return false;
	}

	/* We know we support runtime PM, let's try to enable it now. */
	fd = openat(__igt_pm_power, "control", O_RDWR);
	igt_assert_f(fd >= 0, "Can't open control\n");

	igt_assert(read(fd, __igt_pm_runtime_control,
			sizeof(__igt_pm_runtime_control) - 1) > 0);
	strchomp(__igt_pm_runtime_control);

	igt_debug("Saved runtime power management as '%s' and '%s'\n",
		  __igt_pm_runtime_autosuspend, __igt_pm_runtime_control);

	size = write(fd, "auto\n", 5);
	igt_assert(size == 5);

	lseek(fd, 0, SEEK_SET);
	size = read(fd, buf, ARRAY_SIZE(buf));
	igt_assert(size == 5);
	igt_assert(strncmp(buf, "auto\n", 5) == 0);

	close(fd);

	return true;
}

/**
 * igt_disable_runtime_pm:
 *
 * Disable the runtime pm for i915 device.
 * igt_disable_runtime_pm assumes that igt_setup_runtime_pm has already
 * called to save runtime autosuspend and control attributes.
 */
void igt_disable_runtime_pm(void)
{
	int fd;
	ssize_t size;
	char buf[6];

	igt_assert_fd(__igt_pm_power);

	/* We know we support runtime PM, let's try to disable it now. */
	fd = openat(__igt_pm_power, "control", O_RDWR);
	igt_assert_f(fd >= 0, "Can't open control\n");

	size = write(fd, "on\n", 3);
	igt_assert(size == 3);
	lseek(fd, 0, SEEK_SET);
	size = read(fd, buf, ARRAY_SIZE(buf));
	igt_assert(size == 3);
	igt_assert(strncmp(buf, "on\n", 3) == 0);
	close(fd);
}

/**
 * igt_get_runtime_pm_status:
 *
 * Returns: The current runtime PM status.
 */
static enum igt_runtime_pm_status __igt_get_runtime_pm_status(int fd)
{
	ssize_t n_read;
	char buf[32];

	lseek(fd, 0, SEEK_SET);
	n_read = read(fd, buf, ARRAY_SIZE(buf) - 1);
	igt_assert(n_read >= 0);
	buf[n_read] = '\0';

	if (strncmp(buf, "suspended\n", n_read) == 0)
		return IGT_RUNTIME_PM_STATUS_SUSPENDED;
	else if (strncmp(buf, "active\n", n_read) == 0)
		return IGT_RUNTIME_PM_STATUS_ACTIVE;
	else if (strncmp(buf, "suspending\n", n_read) == 0)
		return IGT_RUNTIME_PM_STATUS_SUSPENDING;
	else if (strncmp(buf, "resuming\n", n_read) == 0)
		return IGT_RUNTIME_PM_STATUS_RESUMING;

	igt_assert_f(false, "Unknown status %s\n", buf);
	return IGT_RUNTIME_PM_STATUS_UNKNOWN;
}

enum igt_runtime_pm_status igt_get_runtime_pm_status(void)
{
	enum igt_runtime_pm_status status;
	int fd;

	if (__igt_pm_power < 0)
		return IGT_RUNTIME_PM_STATUS_UNKNOWN;

	fd = openat(__igt_pm_power, "runtime_status", O_RDONLY);
	igt_assert_f(fd >= 0, "Can't open runtime_status\n");

	status = __igt_get_runtime_pm_status(fd);
	close(fd);

	return status;
}

/**
 * _pm_status_name
 * @status: runtime PM status to stringify
 *
 * Returns: The current runtime PM status as a string
 */
static const char *_pm_status_name(enum igt_runtime_pm_status status)
{
	switch (status) {
	case IGT_RUNTIME_PM_STATUS_ACTIVE:
		return "active";
	case IGT_RUNTIME_PM_STATUS_RESUMING:
		return "resuming";
	case IGT_RUNTIME_PM_STATUS_SUSPENDED:
		return "suspended";
	case IGT_RUNTIME_PM_STATUS_SUSPENDING:
		return "suspending";
	default:
		return "unknown";
	}
}

/**
 * igt_wait_for_pm_status:
 * @status: desired runtime PM status
 *
 * Waits until for the driver to switch to into the desired runtime PM status,
 * with a 10 second timeout.
 *
 * Returns:
 * True if the desired runtime PM status was attained, false if the operation
 * timed out.
 */
bool igt_wait_for_pm_status(enum igt_runtime_pm_status status)
{
	enum igt_runtime_pm_status expected = status;
	bool ret;
	int fd;

	if (__igt_pm_power < 0)
		return false;

	fd = openat(__igt_pm_power, "runtime_status", O_RDONLY);
	igt_assert_f(fd >= 0, "Can't open runtime_status\n");

	ret = igt_wait((status = __igt_get_runtime_pm_status(fd)) == expected,
		       10000, 100);
	close(fd);

	if (!ret)
		igt_warn("timeout: pm_status expected:%s, got:%s\n",
			_pm_status_name(expected),
			_pm_status_name(status));

	return ret;
}

/**
 * dmc_loaded:
 * @debugfs: fd to the debugfs dir.
 *
 * Check whether DMC FW is loaded or not. DMC FW is require for few Display C
 * states like DC5 and DC6. FW does the Context Save and Restore during Display
 * C States entry and exit.
 *
 * Returns:
 * True if DMC FW is loaded otherwise false.
 */
bool igt_pm_dmc_loaded(int debugfs)
{
	char buf[15];
	int len;

	len = igt_sysfs_read(debugfs, "i915_dmc_info", buf, sizeof(buf) - 1);
	if (len < 0)
		return true; /* no CSR support, no DMC requirement */

	buf[len] = '\0';

	igt_info("DMC: %s\n", buf);
	return strstr(buf, "fw loaded: yes");
}

/**
 * igt_pm_pc8_plus_residencies_enabled:
 * @msr_fd: fd to /dev/cpu/0/msr
 * Check whether BIOS has disabled the PC8 package deeper state.
 *
 * Returns:
 * True if PC8+ package deeper state enabled on machine otherwise false.
 */
bool igt_pm_pc8_plus_residencies_enabled(int msr_fd)
{
	int rc;
	uint64_t val;

	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PKG_CST_CONFIG_CONTROL);
	if (rc != sizeof(val))
		return false;
	if ((val & PKG_CST_LIMIT_MASK) < PKG_CST_LIMIT_C8) {
		igt_info("PKG C-states limited below PC8 by the BIOS\n");
		return false;
	}

	return true;
}

/**
 * i915_output_is_lpsp_capable:
 * @drm_fd: fd to drm device
 * @output: igt output for which lpsp capability need to be evaluated
 * Check lpsp capability for a given output.
 *
 * Returns:
 * True if given output is lpsp capable otherwise false.
 */
bool i915_output_is_lpsp_capable(int drm_fd, igt_output_t *output)
{
	char buf[256];
	int fd, len;

	fd = igt_debugfs_connector_dir(drm_fd, output->name, O_RDONLY);
	igt_require(fd >= 0);
	len = igt_debugfs_simple_read(fd, "i915_lpsp_capability",
				      buf, sizeof(buf));

	/* if i915_lpsp_capability not present return the capability as false */
	if (len < 0)
		return false;

	close(fd);

	return strstr(buf, "LPSP: capable");
}
