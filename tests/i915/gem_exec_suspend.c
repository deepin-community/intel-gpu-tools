/*
 * Copyright © 2016 Intel Corporation
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
 */

/** @file gem_exec_suspend.c
 *
 * Exercise executing batches across suspend before checking the results.
 */

#include <fcntl.h>
#include <unistd.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_sysfs.h"

#define NOSLEEP 0
#define IDLE 1
#define SUSPEND_DEVICES 2
#define SUSPEND 3
#define HIBERNATE_DEVICES 4
#define HIBERNATE 5
#define mode(x) ((x) & 0xff)

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

#define UNCACHED (0<<8)
#define CACHED (1<<8)
#define HANG (2<<8)

static void run_test(int fd, unsigned engine, unsigned flags);

static void check_bo(int fd, uint32_t handle)
{
	uint32_t *map;
	int i;

	igt_debug("Verifying result\n");
	map = gem_mmap__cpu(fd, handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, 0);
	for (i = 0; i < 1024; i++)
		igt_assert_eq(map[i], i);
	munmap(map, 4096);
}

static void test_all(int fd, unsigned flags)
{
	run_test(fd, ALL_ENGINES, flags & ~0xff);
}

static void run_test(int fd, unsigned engine, unsigned flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[I915_EXEC_RING_MASK + 1];
	unsigned nengine;
	igt_spin_t *spin = NULL;

	nengine = 0;
	if (engine == ALL_ENGINES) {
		const struct intel_execution_engine2 *e;

		__for_each_physical_engine(fd, e) {
			if (gem_class_can_store_dword(fd, e->class))
				engines[nengine++] = e->flags;
		}
	} else {
		engines[nengine++] = engine;
	}
	igt_require(nengine);

	/* Before suspending, check normal operation */
	if (mode(flags) != NOSLEEP)
		test_all(fd, flags);

	gem_quiescent_gpu(fd);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = 1 << 11;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	gem_set_caching(fd, obj[0].handle, !!(flags & CACHED));
	obj[0].flags |= EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));
	igt_require(__gem_execbuf(fd, &execbuf) == 0);
	gem_close(fd, obj[1].handle);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[0].handle;
	reloc.presumed_offset = obj[0].offset;
	reloc.offset = sizeof(uint32_t);
	if (gen >= 4 && gen < 8)
		reloc.offset += sizeof(uint32_t);
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = 1;

	for (int i = 0; i < 1024; i++) {
		uint64_t offset;
		uint32_t buf[16];
		int b;

		obj[1].handle = gem_create(fd, 4096);

		reloc.delta = i * sizeof(uint32_t);
		offset = reloc.presumed_offset + reloc.delta;

		b = 0;
		buf[b] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			buf[++b] = offset;
			buf[++b] = offset >> 32;
		} else if (gen >= 4) {
			buf[++b] = 0;
			buf[++b] = offset;
		} else {
			buf[b] -= 1;
			buf[++b] = offset;
		}
		buf[++b] = i;
		buf[++b] = MI_BATCH_BUFFER_END;
		gem_write(fd, obj[1].handle,
			  4096-sizeof(buf), buf, sizeof(buf));
		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engines[rand() % nengine];
		gem_execbuf(fd, &execbuf);
		gem_close(fd, obj[1].handle);
	}

	if (flags & HANG)
		spin = igt_spin_new(fd, .engine = engine);

	switch (mode(flags)) {
	case NOSLEEP:
		break;

	case IDLE:
		igt_system_suspend_autoresume(SUSPEND_STATE_FREEZE,
					      SUSPEND_TEST_NONE);
		break;

	case SUSPEND_DEVICES:
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_DEVICES);
		break;

	case SUSPEND:
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
		break;

	case HIBERNATE_DEVICES:
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_DEVICES);
		break;

	case HIBERNATE:
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_NONE);
		break;
	}

	igt_spin_free(fd, spin);

	check_bo(fd, obj[0].handle);
	gem_close(fd, obj[0].handle);

	gem_quiescent_gpu(fd);

	/* After resume, make sure it still works */
	if (mode(flags) != NOSLEEP)
		test_all(fd, flags);
}

struct battery_sample {
	struct timespec tv;
	uint64_t charge;
};

static bool get_power(int dir, struct battery_sample *s)
{
	return (clock_gettime(CLOCK_REALTIME, &s->tv) == 0 &&
		igt_sysfs_scanf(dir, "charge_now", "%"PRIu64, &s->charge) == 1);
}

static double d_charge(const struct battery_sample *after,
		       const struct battery_sample *before)
{
	return (before->charge - after->charge) * 1e-3; /* mWh */
}

static double d_time(const struct battery_sample *after,
		     const struct battery_sample *before)
{
	return ((after->tv.tv_sec - before->tv.tv_sec) +
		(after->tv.tv_nsec - before->tv.tv_nsec) * 1e-9); /* s */
}

static void power_test(int i915, unsigned engine, unsigned flags)
{
	struct battery_sample before, after;
	char *status;
	int dir;

	dir = open("/sys/class/power_supply/BAT0", O_RDONLY);
	igt_require_f(dir != -1, "/sys/class/power_supply/BAT0 not available\n");

	igt_require_f(get_power(dir, &before),
		      "power test needs reported energy level\n");

	status = igt_sysfs_get(dir, "status");
	igt_require_f(status && strcmp(status, "Discharging") == 0,
		      "power test needs to be on battery, not mains, power\n");
	free(status);

	igt_set_autoresume_delay(5 * 60); /* 5 minutes; longer == more stable */

	igt_assert(get_power(dir, &before));
	run_test(i915, engine, flags);
	igt_assert(get_power(dir, &after));

	igt_set_autoresume_delay(0);

	igt_info("Power consumed while suspended: %.3fmWh\n",
		 d_charge(&after, &before));
	igt_info("Discharge rate while suspended: %.3fmW\n",
		 d_charge(&after, &before) * 3600 / d_time(&after, &before));
}

igt_main
{
	const struct {
		const char *suffix;
		unsigned mode;
	} modes[] = {
		{ "", NOSLEEP },
		{ "-S3", SUSPEND },
		{ "-S4", HIBERNATE },
		{ NULL, 0 }
	}, *m;
	const struct intel_execution_engine2 *e;
	igt_hang_t hang;
	int fd;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		igt_require(gem_can_store_dword(fd, 0));

		igt_fork_hang_detector(fd);
	}

	igt_subtest("basic")
		run_test(fd, ALL_ENGINES, NOSLEEP);
	igt_subtest("basic-S0")
		run_test(fd, ALL_ENGINES, IDLE);
	igt_subtest("basic-S3-devices")
		run_test(fd, ALL_ENGINES, SUSPEND_DEVICES);
	igt_subtest("basic-S3")
		run_test(fd, ALL_ENGINES, SUSPEND);
	igt_subtest("basic-S4-devices")
		run_test(fd, ALL_ENGINES, HIBERNATE_DEVICES);
	igt_subtest("basic-S4")
		run_test(fd, ALL_ENGINES, HIBERNATE);

	for (m = modes; m->suffix; m++) {
		igt_subtest_with_dynamic_f("uncached%s", m->suffix) {
			__for_each_physical_engine(fd, e) {
				if (!gem_class_can_store_dword(fd, e->class))
					continue;
				igt_dynamic_f("%s", e->name)
					run_test(fd, e->flags, m->mode | UNCACHED);
			}
		}

		igt_subtest_with_dynamic_f("cached%s", m->suffix) {
			__for_each_physical_engine(fd, e) {
				if (!gem_class_can_store_dword(fd, e->class))
					continue;
				igt_dynamic_f("%s", e->name)
					run_test(fd, e->flags, m->mode | CACHED);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		hang = igt_allow_hang(fd, 0, 0);
	}

	igt_subtest("hang-S3")
		run_test(fd, 0, SUSPEND | HANG);
	igt_subtest("hang-S4")
		run_test(fd, 0, HIBERNATE | HANG);

	igt_subtest("power-S0")
		power_test(fd, 0, IDLE);
	igt_subtest("power-S3")
		power_test(fd, 0, SUSPEND);

	igt_fixture {
		igt_disallow_hang(fd, hang);
		close(fd);
	}
}
