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
 */

#include "config.h"

#include <linux/userfaultfd.h>

#include <pthread.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_rand.h"
#include "igt_rapl.h"
#include "igt_sysfs.h"
#include "igt_vgem.h"
#include "sw_sync.h"

#define LO 0
#define HI 1
#define NOISE 2

#define MAX_PRIO I915_CONTEXT_MAX_USER_PRIORITY
#define MIN_PRIO I915_CONTEXT_MIN_USER_PRIORITY

#define MAX_CONTEXTS 1024
#define MAX_ELSP_QLEN 16
#define MAX_ENGINES (I915_EXEC_RING_MASK + 1)

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

IGT_TEST_DESCRIPTION("Check that we can control the order of execution");

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static inline
uint32_t __sync_read_u32(int fd, uint32_t handle, uint64_t offset)
{
	uint32_t value;

	gem_set_domain(fd, handle, /* No write hazard lies! */
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_read(fd, handle, offset, &value, sizeof(value));

	return value;
}

static inline
void __sync_read_u32_count(int fd, uint32_t handle, uint32_t *dst, uint64_t size)
{
	gem_set_domain(fd, handle, /* No write hazard lies! */
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_read(fd, handle, 0, dst, size);
}

static uint32_t __store_dword(int fd, uint32_t ctx, unsigned ring,
			      uint32_t target, uint32_t offset, uint32_t value,
			      uint32_t cork, int fence, unsigned write_domain)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + !cork);
	execbuf.buffer_count = 2 + !!cork;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx;

	if (fence != -1) {
		execbuf.flags |= I915_EXEC_FENCE_IN;
		execbuf.rsvd2 = fence;
	}

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork;
	obj[0].offset = cork << 20;
	obj[1].handle = target;
	obj[1].offset = target << 20;
	obj[2].handle = gem_create(fd, 4096);
	obj[2].offset = 256 << 10;
	obj[2].offset += (random() % 128) << 12;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[1].handle;
	reloc.presumed_offset = obj[1].offset;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = write_domain;
	obj[2].relocs_ptr = to_user_pointer(&reloc);
	obj[2].relocation_count = 1;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = reloc.presumed_offset + reloc.delta;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = reloc.presumed_offset + reloc.delta;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = reloc.presumed_offset + reloc.delta;
	}
	batch[++i] = value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[2].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);

	return obj[2].handle;
}

static void store_dword(int fd, uint32_t ctx, unsigned ring,
			uint32_t target, uint32_t offset, uint32_t value,
			unsigned write_domain)
{
	gem_close(fd, __store_dword(fd, ctx, ring,
				    target, offset, value,
				    0, -1, write_domain));
}

static void store_dword_plug(int fd, uint32_t ctx, unsigned ring,
			     uint32_t target, uint32_t offset, uint32_t value,
			     uint32_t cork, unsigned write_domain)
{
	gem_close(fd, __store_dword(fd, ctx, ring,
				    target, offset, value,
				    cork, -1, write_domain));
}

static void store_dword_fenced(int fd, uint32_t ctx, unsigned ring,
			       uint32_t target, uint32_t offset, uint32_t value,
			       int fence, unsigned write_domain)
{
	gem_close(fd, __store_dword(fd, ctx, ring,
				    target, offset, value,
				    0, fence, write_domain));
}

static uint32_t create_highest_priority(int fd)
{
	uint32_t ctx = gem_context_clone_with_engines(fd, 0);

	/*
	 * If there is no priority support, all contexts will have equal
	 * priority (and therefore the max user priority), so no context
	 * can overtake us, and we effectively can form a plug.
	 */
	__gem_context_set_priority(fd, ctx, MAX_PRIO);

	return ctx;
}

static void unplug_show_queue(int fd, struct igt_cork *c, unsigned int engine)
{
	igt_spin_t *spin[MAX_ELSP_QLEN];
	int max = MAX_ELSP_QLEN;

	/* If no scheduler, all batches are emitted in submission order */
	if (!gem_scheduler_enabled(fd))
		max = 1;

	for (int n = 0; n < max; n++) {
		const struct igt_spin_factory opts = {
			.ctx = create_highest_priority(fd),
			.engine = engine,
		};
		spin[n] = __igt_spin_factory(fd, &opts);
		gem_context_destroy(fd, opts.ctx);
	}

	igt_cork_unplug(c); /* batches will now be queued on the engine */
	igt_debugfs_dump(fd, "i915_engine_info");

	for (int n = 0; n < max; n++)
		igt_spin_free(fd, spin[n]);

}

static void fifo(int fd, unsigned ring)
{
	IGT_CORK_FENCE(cork);
	uint32_t scratch;
	uint32_t result;
	int fence;

	scratch = gem_create(fd, 4096);

	fence = igt_cork_plug(&cork, fd);

	/* Same priority, same timeline, final result will be the second eb */
	store_dword_fenced(fd, 0, ring, scratch, 0, 1, fence, 0);
	store_dword_fenced(fd, 0, ring, scratch, 0, 2, fence, 0);

	unplug_show_queue(fd, &cork, ring);
	close(fence);

	result =  __sync_read_u32(fd, scratch, 0);
	gem_close(fd, scratch);

	igt_assert_eq_u32(result, 2);
}

enum implicit_dir {
	READ_WRITE = 0x1,
	WRITE_READ = 0x2,
};

static void implicit_rw(int i915, unsigned ring, enum implicit_dir dir)
{
	const struct intel_execution_engine2 *e;
	IGT_CORK_FENCE(cork);
	unsigned int count;
	uint32_t scratch;
	uint32_t result;
	int fence;

	count = 0;
	__for_each_physical_engine(i915, e) {
		if (e->flags == ring)
			continue;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		count++;
	}
	igt_require(count);

	scratch = gem_create(i915, 4096);
	fence = igt_cork_plug(&cork, i915);

	if (dir & WRITE_READ)
		store_dword_fenced(i915, 0,
				   ring, scratch, 0, ~ring,
				   fence, I915_GEM_DOMAIN_RENDER);

	__for_each_physical_engine(i915, e) {
		if (e->flags == ring)
			continue;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		store_dword_fenced(i915, 0,
				   e->flags, scratch, 0, e->flags,
				   fence, 0);
	}

	if (dir & READ_WRITE)
		store_dword_fenced(i915, 0,
				   ring, scratch, 0, ring,
				   fence, I915_GEM_DOMAIN_RENDER);

	unplug_show_queue(i915, &cork, ring);
	close(fence);

	result =  __sync_read_u32(i915, scratch, 0);
	gem_close(i915, scratch);

	if (dir & WRITE_READ)
		igt_assert_neq_u32(result, ~ring);
	if (dir & READ_WRITE)
		igt_assert_eq_u32(result, ring);
}

static void independent(int fd, unsigned int engine, unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	IGT_CORK_FENCE(cork);
	igt_spin_t *spin = NULL;
	uint32_t scratch, batch;
	uint32_t *ptr;
	int fence;

	scratch = gem_create(fd, 4096);
	ptr = gem_mmap__device_coherent(fd, scratch, 0, 4096, PROT_READ);
	igt_assert_eq(ptr[0], 0);

	fence = igt_cork_plug(&cork, fd);

	/* Check that we can submit to engine while all others are blocked */
	__for_each_physical_engine(fd, e) {
		if (e->flags == engine)
			continue;

		if (!gem_class_can_store_dword(fd, e->class))
			continue;

		if (spin == NULL) {
			spin = __igt_spin_new(fd,
					      .engine = e->flags,
					      .flags = flags);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.flags = e->flags,
			};
			gem_execbuf(fd, &eb);
		}

		store_dword_fenced(fd, 0, e->flags, scratch, 0, e->flags, fence, 0);
	}
	igt_require(spin);

	/* Same priority, but different timeline (as different engine) */
	batch = __store_dword(fd, 0, engine, scratch, 0, engine, 0, fence, 0);

	unplug_show_queue(fd, &cork, engine);
	close(fence);

	gem_sync(fd, batch);
	igt_assert(!gem_bo_busy(fd, batch));
	igt_assert(gem_bo_busy(fd, spin->handle));
	gem_close(fd, batch);

	/* Only the local engine should be free to complete. */
	igt_assert(gem_bo_busy(fd, scratch));
	igt_assert_eq(ptr[0], engine);

	igt_spin_free(fd, spin);
	gem_quiescent_gpu(fd);

	/* And we expect the others to have overwritten us, order unspecified */
	igt_assert(!gem_bo_busy(fd, scratch));
	igt_assert_neq(ptr[0], engine);

	munmap(ptr, 4096);
	gem_close(fd, scratch);
}

static void smoketest(int fd, unsigned ring, unsigned timeout)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const struct intel_execution_engine2 *e;
	unsigned engines[MAX_ENGINES];
	unsigned nengine;
	unsigned engine;
	uint32_t scratch;
	uint32_t result[2 * ncpus];

	nengine = 0;
	if (ring == ALL_ENGINES) {
		__for_each_physical_engine(fd, e)
			if (gem_class_can_store_dword(fd, e->class))
				engines[nengine++] = e->flags;
	} else {
		engines[nengine++] = ring;
	}
	igt_require(nengine);

	scratch = gem_create(fd, 4096);
	igt_fork(child, ncpus) {
		unsigned long count = 0;
		uint32_t ctx;

		hars_petruska_f54_1_random_perturb(child);

		ctx = gem_context_clone_with_engines(fd, 0);
		igt_until_timeout(timeout) {
			int prio;

			prio = hars_petruska_f54_1_random_unsafe_max(MAX_PRIO - MIN_PRIO) + MIN_PRIO;
			gem_context_set_priority(fd, ctx, prio);

			engine = engines[hars_petruska_f54_1_random_unsafe_max(nengine)];
			store_dword(fd, ctx, engine, scratch,
				    8*child + 0, ~child,
				    0);
			for (unsigned int step = 0; step < 8; step++)
				store_dword(fd, ctx, engine, scratch,
					    8*child + 4, count++,
					    0);
		}
		gem_context_destroy(fd, ctx);
	}
	igt_waitchildren();

	__sync_read_u32_count(fd, scratch, result, sizeof(result));
	gem_close(fd, scratch);

	for (unsigned n = 0; n < ncpus; n++) {
		igt_assert_eq_u32(result[2 * n], ~n);
		/*
		 * Note this count is approximate due to unconstrained
		 * ordering of the dword writes between engines.
		 *
		 * Take the result with a pinch of salt.
		 */
		igt_info("Child[%d] completed %u cycles\n",  n, result[(2 * n) + 1]);
	}
}

static uint32_t timeslicing_batches(int i915, uint32_t *offset)
{
        uint32_t handle = gem_create(i915, 4096);
        uint32_t cs[256];

	*offset += 4000;
	for (int pair = 0; pair <= 1; pair++) {
		int x = 1;
		int i = 0;

		for (int step = 0; step < 8; step++) {
			if (pair) {
				cs[i++] =
					MI_SEMAPHORE_WAIT |
					MI_SEMAPHORE_POLL |
					MI_SEMAPHORE_SAD_EQ_SDD |
					(4 - 2);
				cs[i++] = x++;
				cs[i++] = *offset;
				cs[i++] = 0;
			}

			cs[i++] = MI_STORE_DWORD_IMM;
			cs[i++] = *offset;
			cs[i++] = 0;
			cs[i++] = x++;

			if (!pair) {
				cs[i++] =
					MI_SEMAPHORE_WAIT |
					MI_SEMAPHORE_POLL |
					MI_SEMAPHORE_SAD_EQ_SDD |
					(4 - 2);
				cs[i++] = x++;
				cs[i++] = *offset;
				cs[i++] = 0;
			}
		}

		cs[i++] = MI_BATCH_BUFFER_END;
		igt_assert(i < ARRAY_SIZE(cs));
		gem_write(i915, handle, pair * sizeof(cs), cs, sizeof(cs));
	}

	*offset = sizeof(cs);
        return handle;
}

static void timeslice(int i915, unsigned int engine)
{
	unsigned int offset = 24 << 20;
	struct drm_i915_gem_exec_object2 obj = {
		.offset = offset,
		.flags = EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_execbuffer2 execbuf  = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	uint32_t *result;
	int out;

	/*
	 * Create a pair of interlocking batches, that ping pong
	 * between each other, and only advance one step at a time.
	 * We require the kernel to preempt at each semaphore and
	 * switch to the other batch in order to advance.
	 */

	igt_require(gem_scheduler_has_semaphores(i915));
	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	obj.handle = timeslicing_batches(i915, &offset);
	result = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_READ);

	execbuf.flags = engine | I915_EXEC_FENCE_OUT;
	execbuf.batch_start_offset = 0;
	gem_execbuf_wr(i915, &execbuf);

	/* No coupling between requests; free to timeslice */

	execbuf.rsvd1 = gem_context_clone_with_engines(i915, 0);
	execbuf.rsvd2 >>= 32;
	execbuf.flags = engine | I915_EXEC_FENCE_OUT;
	execbuf.batch_start_offset = offset;
	gem_execbuf_wr(i915, &execbuf);
	gem_context_destroy(i915, execbuf.rsvd1);

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	/* no hangs! */
	out = execbuf.rsvd2;
	igt_assert_eq(sync_fence_status(out), 1);
	close(out);

	out = execbuf.rsvd2 >> 32;
	igt_assert_eq(sync_fence_status(out), 1);
	close(out);

	igt_assert_eq(result[1000], 16);
	munmap(result, 4096);
}

static uint32_t timesliceN_batches(int i915, uint32_t offset, int count)
{
        uint32_t handle = gem_create(i915, (count + 1) * 1024);
        uint32_t cs[256];

	for (int pair = 0; pair < count; pair++) {
		int x = pair;
		int i = 0;

		for (int step = 0; step < 8; step++) {
			cs[i++] =
				MI_SEMAPHORE_WAIT |
				MI_SEMAPHORE_POLL |
				MI_SEMAPHORE_SAD_EQ_SDD |
				(4 - 2);
			cs[i++] = x;
			cs[i++] = offset;
			cs[i++] = 0;

			cs[i++] = MI_STORE_DWORD_IMM;
			cs[i++] = offset;
			cs[i++] = 0;
			cs[i++] = x + 1;

			x += count;
		}

		cs[i++] = MI_BATCH_BUFFER_END;
		igt_assert(i < ARRAY_SIZE(cs));
		gem_write(i915, handle, (pair + 1) * sizeof(cs),
			  cs, sizeof(cs));
	}

        return handle;
}

static void timesliceN(int i915, unsigned int engine, int count)
{
	const unsigned int sz = ALIGN((count + 1) * 1024, 4096);
	unsigned int offset = 24 << 20;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = timesliceN_batches(i915, offset, count),
		.offset = offset,
		.flags = EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_execbuffer2 execbuf  = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine | I915_EXEC_FENCE_OUT,
	};
	uint32_t *result =
		gem_mmap__device_coherent(i915, obj.handle, 0, sz, PROT_READ);
	int fence[count];

	/*
	 * Create a pair of interlocking batches, that ping pong
	 * between each other, and only advance one step at a time.
	 * We require the kernel to preempt at each semaphore and
	 * switch to the other batch in order to advance.
	 */

	igt_require(gem_scheduler_has_semaphores(i915));
	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	/* No coupling between requests; free to timeslice */

	for (int i = 0; i < count; i++) {
		execbuf.rsvd1 = gem_context_clone_with_engines(i915, 0);
		execbuf.batch_start_offset = (i + 1) * 1024;;
		gem_execbuf_wr(i915, &execbuf);
		gem_context_destroy(i915, execbuf.rsvd1);

		fence[i] = execbuf.rsvd2 >> 32;
	}

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	/* no hangs! */
	for (int i = 0; i < count; i++) {
		igt_assert_eq(sync_fence_status(fence[i]), 1);
		close(fence[i]);
	}

	igt_assert_eq(*result, 8 * count);
	munmap(result, sz);
}

static void lateslice(int i915, unsigned int engine, unsigned long flags)
{
	igt_spin_t *spin[3];
	uint32_t ctx;

	igt_require(gem_scheduler_has_semaphores(i915));
	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	ctx = gem_context_create(i915);
	spin[0] = igt_spin_new(i915, .ctx = ctx, .engine = engine,
			       .flags = (IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT |
					 flags));
	gem_context_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[0]);

	ctx = gem_context_create(i915);
	spin[1] = igt_spin_new(i915, .ctx = ctx, .engine = engine,
			       .fence = spin[0]->out_fence,
			       .flags = (IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_IN |
					 flags));
	gem_context_destroy(i915, ctx);

	usleep(5000); /* give some time for the new spinner to be scheduled */

	/*
	 * Now that we have two spinners in the HW submission queue [ELSP],
	 * and since they are strictly ordered, the timeslicing timer may
	 * be disabled as no reordering is possible. However, upon adding a
	 * third spinner we then expect timeslicing to be real enabled.
	 */

	ctx = gem_context_create(i915);
	spin[2] = igt_spin_new(i915, .ctx = ctx, .engine = engine,
			       .flags = IGT_SPIN_POLL_RUN | flags);
	gem_context_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[2]);

	igt_assert(gem_bo_busy(i915, spin[0]->handle));
	igt_assert(gem_bo_busy(i915, spin[1]->handle));
	igt_assert(gem_bo_busy(i915, spin[2]->handle));

	igt_assert(!igt_spin_has_started(spin[1]));
	igt_spin_free(i915, spin[0]);

	/* Now just spin[1] and spin[2] active */
	igt_spin_busywait_until_started(spin[1]);

	igt_assert(gem_bo_busy(i915, spin[2]->handle));
	igt_spin_free(i915, spin[2]);

	igt_assert(gem_bo_busy(i915, spin[1]->handle));
	igt_spin_free(i915, spin[1]);
}

static void cancel_spinner(int i915,
			   uint32_t ctx, unsigned int engine,
			   igt_spin_t *spin)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine | I915_EXEC_FENCE_SUBMIT,
		.rsvd1 = ctx, /* same vm */
		.rsvd2 = spin->out_fence,
	};
	uint32_t *map, *cs;

	map = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_WRITE);
	cs = map;

	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
		offset_in_page(spin->condition);
	*cs++ = spin->obj[IGT_SPIN_BATCH].offset >> 32;
	*cs++ = MI_BATCH_BUFFER_END;

	*cs++ = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj.handle);
}

static void submit_slice(int i915,
			 const struct intel_execution_engine2 *e,
			 unsigned int flags)
#define EARLY_SUBMIT 0x1
#define LATE_SUBMIT 0x2
#define USERPTR 0x4
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines , 1) = {};
	const struct intel_execution_engine2 *cancel;
	struct drm_i915_gem_context_param param = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines),
		.size = sizeof(engines),
	};

	/*
	 * When using a submit fence, we do not want to block concurrent work,
	 * especially when that work is coperating with the spinner.
	 */

	igt_require(gem_scheduler_has_semaphores(i915));
	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	__for_each_physical_engine(i915, cancel) {
		igt_spin_t *bg, *spin;
		int timeline = -1;
		int fence = -1;

		if (!gem_class_can_store_dword(i915, cancel->class))
			continue;

		igt_debug("Testing cancellation from %s\n", e->name);

		bg = igt_spin_new(i915, .engine = e->flags);

		if (flags & LATE_SUBMIT) {
			timeline = sw_sync_timeline_create();
			fence = sw_sync_timeline_create_fence(timeline, 1);
		}

		engines.engines[0].engine_class = e->class;
		engines.engines[0].engine_instance = e->instance;
		gem_context_set_param(i915, &param);
		spin = igt_spin_new(i915, .ctx = param.ctx_id,
				    .fence = fence,
				    .flags =
				    IGT_SPIN_POLL_RUN |
				    (flags & LATE_SUBMIT ? IGT_SPIN_FENCE_IN : 0) |
				    (flags & USERPTR ? IGT_SPIN_USERPTR : 0) |
				    IGT_SPIN_FENCE_OUT);
		if (fence != -1)
			close(fence);

		if (flags & EARLY_SUBMIT)
			igt_spin_busywait_until_started(spin);

		engines.engines[0].engine_class = cancel->class;
		engines.engines[0].engine_instance = cancel->instance;
		gem_context_set_param(i915, &param);
		cancel_spinner(i915, param.ctx_id, 0, spin);

		if (timeline != -1)
			close(timeline);

		gem_sync(i915, spin->handle);
		igt_spin_free(i915, spin);
		igt_spin_free(i915, bg);
	}

	gem_context_destroy(i915, param.ctx_id);
}

static uint32_t __batch_create(int i915, uint32_t offset)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, ALIGN(offset + 4, 4096));
	gem_write(i915, handle, offset, &bbe, sizeof(bbe));

	return handle;
}

static uint32_t batch_create(int i915)
{
	return __batch_create(i915, 0);
}

static void semaphore_userlock(int i915, unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
	};
	igt_spin_t *spin = NULL;
	uint32_t scratch;

	igt_require(gem_scheduler_has_semaphores(i915));

	/*
	 * Given the use of semaphores to govern parallel submission
	 * of nearly-ready work to HW, we still want to run actually
	 * ready work immediately. Without semaphores, the dependent
	 * work wouldn't be submitted so our ready work will run.
	 */

	scratch = gem_create(i915, 4096);
	__for_each_physical_engine(i915, e) {
		if (!spin) {
			spin = igt_spin_new(i915,
					    .dependency = scratch,
					    .engine = e->flags,
					    .flags = flags);
		} else {
			uint64_t saved = spin->execbuf.flags;

			spin->execbuf.flags &= ~I915_EXEC_RING_MASK;
			spin->execbuf.flags |= e->flags;

			gem_execbuf(i915, &spin->execbuf);

			spin->execbuf.flags = saved;
		}
	}
	igt_require(spin);
	gem_close(i915, scratch);

	/*
	 * On all dependent engines, the request may be executing (busywaiting
	 * on a HW semaphore) but it should not prevent any real work from
	 * taking precedence.
	 */
	scratch = gem_context_clone_with_engines(i915, 0);
	__for_each_physical_engine(i915, e) {
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
			.flags = e->flags,
			.rsvd1 = scratch,
		};

		if (e->flags == (spin->execbuf.flags & I915_EXEC_RING_MASK))
			continue;

		gem_execbuf(i915, &execbuf);
	}
	gem_context_destroy(i915, scratch);
	gem_sync(i915, obj.handle); /* to hang unless we can preempt */
	gem_close(i915, obj.handle);

	igt_spin_free(i915, spin);
}

static void semaphore_codependency(int i915, unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	struct {
		igt_spin_t *xcs, *rcs;
	} task[2];
	int i;

	/*
	 * Consider two tasks, task A runs on (xcs0, rcs0) and task B
	 * on (xcs1, rcs0). That is they must both run a dependent
	 * batch on rcs0, after first running in parallel on separate
	 * engines. To maximise throughput, we want the shorter xcs task
	 * to start on rcs first. However, if we insert semaphores we may
	 * pick wrongly and end up running the requests in the least
	 * optimal order.
	 */

	i = 0;
	__for_each_physical_engine(i915, e) {
		uint32_t ctx;

		if (!e->flags) {
			igt_require(gem_class_can_store_dword(i915, e->class));
			continue;
		}

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		ctx = gem_context_clone_with_engines(i915, 0);

		task[i].xcs =
			__igt_spin_new(i915,
				       .ctx = ctx,
				       .engine = e->flags,
				       .flags = IGT_SPIN_POLL_RUN | flags);
		igt_spin_busywait_until_started(task[i].xcs);

		/* Common rcs tasks will be queued in FIFO */
		task[i].rcs =
			__igt_spin_new(i915,
				       .ctx = ctx,
				       .engine = 0,
				       .dependency = task[i].xcs->handle);

		gem_context_destroy(i915, ctx);

		if (++i == ARRAY_SIZE(task))
			break;
	}
	igt_require(i == ARRAY_SIZE(task));

	/* Since task[0] was queued first, it will be first in queue for rcs */
	igt_spin_end(task[1].xcs);
	igt_spin_end(task[1].rcs);
	gem_sync(i915, task[1].rcs->handle); /* to hang if task[0] hogs rcs */

	for (i = 0; i < ARRAY_SIZE(task); i++) {
		igt_spin_end(task[i].xcs);
		igt_spin_end(task[i].rcs);
	}

	for (i = 0; i < ARRAY_SIZE(task); i++) {
		igt_spin_free(i915, task[i].xcs);
		igt_spin_free(i915, task[i].rcs);
	}
}

static void semaphore_resolve(int i915, unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	const uint32_t SEMAPHORE_ADDR = 64 << 10;
	uint32_t semaphore, outer, inner, *sema;

	/*
	 * Userspace may submit batches that wait upon unresolved
	 * semaphores. Ideally, we want to put those blocking batches
	 * to the back of the execution queue if we have something else
	 * that is ready to run right away. This test exploits a failure
	 * to reorder batches around a blocking semaphore by submitting
	 * the release of that semaphore from a later context.
	 */

	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_get_drm_devid(i915) >= 8); /* for MI_SEMAPHORE_WAIT */

	outer = gem_context_clone_with_engines(i915, 0);
	inner = gem_context_clone_with_engines(i915, 0);

	semaphore = gem_create(i915, 4096);
	sema = gem_mmap__wc(i915, semaphore, 0, 4096, PROT_WRITE);

	__for_each_physical_engine(i915, e) {
		struct drm_i915_gem_exec_object2 obj[3];
		struct drm_i915_gem_execbuffer2 eb;
		uint32_t handle, cancel;
		uint32_t *cs, *map;
		igt_spin_t *spin;
		int64_t poke = 1;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		spin = __igt_spin_new(i915, .engine = e->flags, .flags = flags);
		igt_spin_end(spin); /* we just want its address for later */
		gem_sync(i915, spin->handle);
		igt_spin_reset(spin);

		handle = gem_create(i915, 4096);
		cs = map = gem_mmap__cpu(i915, handle, 0, 4096, PROT_WRITE);

		/* Set semaphore initially to 1 for polling and signaling */
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = SEMAPHORE_ADDR;
		*cs++ = 0;
		*cs++ = 1;

		/* Wait until another batch writes to our semaphore */
		*cs++ = MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_EQ_SDD |
			(4 - 2);
		*cs++ = 0;
		*cs++ = SEMAPHORE_ADDR;
		*cs++ = 0;

		/* Then cancel the spinner */
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
			offset_in_page(spin->condition);
		*cs++ = 0;
		*cs++ = MI_BATCH_BUFFER_END;

		*cs++ = MI_BATCH_BUFFER_END;
		munmap(map, 4096);

		memset(&eb, 0, sizeof(eb));

		/* First up is our spinning semaphore */
		memset(obj, 0, sizeof(obj));
		obj[0] = spin->obj[IGT_SPIN_BATCH];
		obj[1].handle = semaphore;
		obj[1].offset = SEMAPHORE_ADDR;
		obj[1].flags = EXEC_OBJECT_PINNED;
		obj[2].handle = handle;
		eb.buffer_count = 3;
		eb.buffers_ptr = to_user_pointer(obj);
		eb.rsvd1 = outer;
		gem_execbuf(i915, &eb);

		/* Then add the GPU hang intermediatory */
		memset(obj, 0, sizeof(obj));
		obj[0].handle = handle;
		obj[0].flags = EXEC_OBJECT_WRITE; /* always after semaphore */
		obj[1] = spin->obj[IGT_SPIN_BATCH];
		eb.buffer_count = 2;
		eb.rsvd1 = 0;
		gem_execbuf(i915, &eb);

		while (READ_ONCE(*sema) == 0)
			;

		/* Now the semaphore is spinning, cancel it */
		cancel = gem_create(i915, 4096);
		cs = map = gem_mmap__cpu(i915, cancel, 0, 4096, PROT_WRITE);
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = SEMAPHORE_ADDR;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = MI_BATCH_BUFFER_END;
		munmap(map, 4096);

		memset(obj, 0, sizeof(obj));
		obj[0].handle = semaphore;
		obj[0].offset = SEMAPHORE_ADDR;
		obj[0].flags = EXEC_OBJECT_PINNED;
		obj[1].handle = cancel;
		eb.buffer_count = 2;
		eb.rsvd1 = inner;
		gem_execbuf(i915, &eb);
		gem_wait(i915, cancel, &poke); /* match sync's WAIT_PRIORITY */
		gem_close(i915, cancel);

		gem_sync(i915, handle); /* To hang unless cancel runs! */
		gem_close(i915, handle);
		igt_spin_free(i915, spin);

		igt_assert_eq(*sema, 0);
	}

	munmap(sema, 4096);
	gem_close(i915, semaphore);

	gem_context_destroy(i915, inner);
	gem_context_destroy(i915, outer);
}

static void semaphore_noskip(int i915, unsigned long flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const struct intel_execution_engine2 *outer, *inner;
	uint32_t ctx;

	igt_require(gen >= 6); /* MI_STORE_DWORD_IMM convenience */

	ctx = gem_context_clone_with_engines(i915, 0);

	__for_each_physical_engine(i915, outer) {
	__for_each_physical_engine(i915, inner) {
		struct drm_i915_gem_exec_object2 obj[3];
		struct drm_i915_gem_execbuffer2 eb;
		uint32_t handle, *cs, *map;
		igt_spin_t *chain, *spin;

		if (inner->flags == outer->flags ||
		    !gem_class_can_store_dword(i915, inner->class))
			continue;

		chain = __igt_spin_new(i915, .engine = outer->flags, .flags = flags);

		spin = __igt_spin_new(i915, .engine = inner->flags, .flags = flags);
		igt_spin_end(spin); /* we just want its address for later */
		gem_sync(i915, spin->handle);
		igt_spin_reset(spin);

		handle = gem_create(i915, 4096);
		cs = map = gem_mmap__cpu(i915, handle, 0, 4096, PROT_WRITE);

		/* Cancel the following spinner */
		*cs++ = MI_STORE_DWORD_IMM;
		if (gen >= 8) {
			*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
				offset_in_page(spin->condition);
			*cs++ = 0;
		} else {
			*cs++ = 0;
			*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
				offset_in_page(spin->condition);
		}
		*cs++ = MI_BATCH_BUFFER_END;

		*cs++ = MI_BATCH_BUFFER_END;
		munmap(map, 4096);

		/* port0: implicit semaphore from engine */
		memset(obj, 0, sizeof(obj));
		obj[0] = chain->obj[IGT_SPIN_BATCH];
		obj[0].flags |= EXEC_OBJECT_WRITE;
		obj[1] = spin->obj[IGT_SPIN_BATCH];
		obj[2].handle = handle;
		memset(&eb, 0, sizeof(eb));
		eb.buffer_count = 3;
		eb.buffers_ptr = to_user_pointer(obj);
		eb.rsvd1 = ctx;
		eb.flags = inner->flags;
		gem_execbuf(i915, &eb);

		/* port1: dependency chain from port0 */
		memset(obj, 0, sizeof(obj));
		obj[0].handle = handle;
		obj[0].flags = EXEC_OBJECT_WRITE;
		obj[1] = spin->obj[IGT_SPIN_BATCH];
		memset(&eb, 0, sizeof(eb));
		eb.buffer_count = 2;
		eb.buffers_ptr = to_user_pointer(obj);
		eb.flags = inner->flags;
		gem_execbuf(i915, &eb);

		igt_spin_set_timeout(chain, NSEC_PER_SEC / 100);
		gem_sync(i915, spin->handle); /* To hang unless cancel runs! */

		gem_close(i915, handle);
		igt_spin_free(i915, spin);
		igt_spin_free(i915, chain);
	}
	}

	gem_context_destroy(i915, ctx);
}

static void
noreorder(int i915, unsigned int engine, int prio, unsigned int flags)
#define CORKED 0x1
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine,
		.rsvd1 = gem_context_clone_with_engines(i915, 0),
	};
	IGT_CORK_FENCE(cork);
	uint32_t *map, *cs;
	igt_spin_t *slice;
	igt_spin_t *spin;
	int fence = -1;
	uint64_t addr;
	uint32_t ctx;

	if (flags & CORKED)
		fence = igt_cork_plug(&cork, i915);

	ctx = gem_context_clone(i915, execbuf.rsvd1,
			      I915_CONTEXT_CLONE_ENGINES |
			      I915_CONTEXT_CLONE_VM,
			      0);
	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .fence = fence,
			    .flags = IGT_SPIN_FENCE_OUT | IGT_SPIN_FENCE_IN);
	close(fence);

	/* Loop around the engines, creating a chain of fences */
	spin->execbuf.rsvd2 = (uint64_t)dup(spin->out_fence) << 32;
	spin->execbuf.rsvd2 |= 0xffffffff;
	__for_each_physical_engine(i915, e) {
		if (e->flags == engine)
			continue;

		close(spin->execbuf.rsvd2);
		spin->execbuf.rsvd2 >>= 32;

		spin->execbuf.flags =
			e->flags | I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT;
		gem_execbuf_wr(i915, &spin->execbuf);
	}
	close(spin->execbuf.rsvd2);
	spin->execbuf.rsvd2 >>= 32;
	gem_context_destroy(i915, ctx);

	/*
	 * Wait upon the fence chain, and try to terminate the spinner.
	 *
	 * If the scheduler skips a link in the chain and doesn't reach the
	 * dependency on the same engine, we may preempt that spinner to
	 * execute the terminating batch; and the spinner will untimely
	 * exit.
	 */
	map = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_WRITE);
	cs = map;

	addr = spin->obj[IGT_SPIN_BATCH].offset +
		offset_in_page(spin->condition);
	if (gen >= 8) {
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = addr;
		addr >>= 32;
	} else if (gen >= 4) {
		*cs++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		*cs++ = 0;
	} else {
		*cs++ = (MI_STORE_DWORD_IMM | 1 << 22) - 1;
	}
	*cs++ = addr;
	*cs++ = MI_BATCH_BUFFER_END;
	*cs++ = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	execbuf.rsvd2 = spin->execbuf.rsvd2;
	execbuf.flags |= I915_EXEC_FENCE_IN;

	gem_context_set_priority(i915, execbuf.rsvd1, prio);

	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj.handle);
	gem_context_destroy(i915, execbuf.rsvd1);
	if (cork.fd != -1)
		igt_cork_unplug(&cork);

	/*
	 * Then wait for a timeslice.
	 *
	 * If we start the next spinner it means we have expired the first
	 * spinner's timeslice and the second batch would have already been run,
	 * if it will ever be.
	 *
	 * Without timeslices, fallback to waiting a second.
	 */
	slice = igt_spin_new(i915,
			    .engine = engine,
			    .flags = IGT_SPIN_POLL_RUN);
	igt_until_timeout(1) {
		if (igt_spin_has_started(slice))
			break;
	}
	igt_spin_free(i915, slice);

	/* Check the store did not run before the spinner */
	igt_assert_eq(sync_fence_status(spin->out_fence), 0);
	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
}

static void reorder(int fd, unsigned ring, unsigned flags)
#define EQUAL 1
{
	IGT_CORK_FENCE(cork);
	uint32_t scratch;
	uint32_t result;
	uint32_t ctx[2];
	int fence;

	ctx[LO] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[HI] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[HI], flags & EQUAL ? MIN_PRIO : 0);

	scratch = gem_create(fd, 4096);
	fence = igt_cork_plug(&cork, fd);

	/* We expect the high priority context to be executed first, and
	 * so the final result will be value from the low priority context.
	 */
	store_dword_fenced(fd, ctx[LO], ring, scratch, 0, ctx[LO], fence, 0);
	store_dword_fenced(fd, ctx[HI], ring, scratch, 0, ctx[HI], fence, 0);

	unplug_show_queue(fd, &cork, ring);
	close(fence);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	result =  __sync_read_u32(fd, scratch, 0);
	gem_close(fd, scratch);

	if (flags & EQUAL) /* equal priority, result will be fifo */
		igt_assert_eq_u32(result, ctx[HI]);
	else
		igt_assert_eq_u32(result, ctx[LO]);
}

static void promotion(int fd, unsigned ring)
{
	IGT_CORK_FENCE(cork);
	uint32_t result, dep;
	uint32_t result_read, dep_read;
	uint32_t ctx[3];
	int fence;

	ctx[LO] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[HI] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[HI], 0);

	ctx[NOISE] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[NOISE], MIN_PRIO/2);

	result = gem_create(fd, 4096);
	dep = gem_create(fd, 4096);

	fence = igt_cork_plug(&cork, fd);

	/* Expect that HI promotes LO, so the order will be LO, HI, NOISE.
	 *
	 * fifo would be NOISE, LO, HI.
	 * strict priority would be  HI, NOISE, LO
	 */
	store_dword_fenced(fd, ctx[NOISE], ring, result, 0, ctx[NOISE], fence, 0);
	store_dword_fenced(fd, ctx[LO], ring, result, 0, ctx[LO], fence, 0);

	/* link LO <-> HI via a dependency on another buffer */
	store_dword(fd, ctx[LO], ring, dep, 0, ctx[LO], I915_GEM_DOMAIN_INSTRUCTION);
	store_dword(fd, ctx[HI], ring, dep, 0, ctx[HI], 0);

	store_dword(fd, ctx[HI], ring, result, 0, ctx[HI], 0);

	unplug_show_queue(fd, &cork, ring);
	close(fence);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	dep_read = __sync_read_u32(fd, dep, 0);
	gem_close(fd, dep);

	result_read = __sync_read_u32(fd, result, 0);
	gem_close(fd, result);

	igt_assert_eq_u32(dep_read, ctx[HI]);
	igt_assert_eq_u32(result_read, ctx[NOISE]);
}

static bool set_preempt_timeout(int i915,
				const struct intel_execution_engine2 *e,
				int timeout_ms)
{
	return gem_engine_property_printf(i915, e->name,
					  "preempt_timeout_ms",
					  "%d", timeout_ms) > 0;
}

#define NEW_CTX (0x1 << 0)
#define HANG_LP (0x1 << 1)
static void preempt(int fd, const struct intel_execution_engine2 *e, unsigned flags)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read;
	igt_spin_t *spin[MAX_ELSP_QLEN];
	uint32_t ctx[2];
	igt_hang_t hang;

	/* Set a fast timeout to speed the test up (if available) */
	set_preempt_timeout(fd, e, 150);

	ctx[LO] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[HI] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);

	if (flags & HANG_LP)
		hang = igt_hang_ctx(fd, ctx[LO], e->flags, 0);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		if (flags & NEW_CTX) {
			gem_context_destroy(fd, ctx[LO]);
			ctx[LO] = gem_context_clone_with_engines(fd, 0);
			gem_context_set_priority(fd, ctx[LO], MIN_PRIO);
		}
		spin[n] = __igt_spin_new(fd,
					 .ctx = ctx[LO],
					 .engine = e->flags,
					 .flags = flags & USERPTR ? IGT_SPIN_USERPTR : 0);
		igt_debug("spin[%d].handle=%d\n", n, spin[n]->handle);

		store_dword(fd, ctx[HI], e->flags, result, 0, n + 1, I915_GEM_DOMAIN_RENDER);

		result_read = __sync_read_u32(fd, result, 0);
		igt_assert_eq_u32(result_read, n + 1);
		igt_assert(gem_bo_busy(fd, spin[0]->handle));
	}

	for (int n = 0; n < ARRAY_SIZE(spin); n++)
		igt_spin_free(fd, spin[n]);

	if (flags & HANG_LP)
		igt_post_hang_ring(fd, hang);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	gem_close(fd, result);
}

#define CHAIN 0x1
#define CONTEXTS 0x2

static igt_spin_t *__noise(int fd, uint32_t ctx, int prio, igt_spin_t *spin)
{
	const struct intel_execution_engine2 *e;

	gem_context_set_priority(fd, ctx, prio);

	__for_each_physical_engine(fd, e) {
		if (spin == NULL) {
			spin = __igt_spin_new(fd,
					      .ctx = ctx,
					      .engine = e->flags);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.rsvd1 = ctx,
				.flags = e->flags,
			};
			gem_execbuf(fd, &eb);
		}
	}

	return spin;
}

static void __preempt_other(int fd,
			    uint32_t *ctx,
			    unsigned int target, unsigned int primary,
			    unsigned flags)
{
	const struct intel_execution_engine2 *e;
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read[4096 / sizeof(uint32_t)];
	unsigned int n, i;

	n = 0;
	store_dword(fd, ctx[LO], primary,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);
	n++;

	if (flags & CHAIN) {
		__for_each_physical_engine(fd, e) {
			store_dword(fd, ctx[LO], e->flags,
				    result, (n + 1)*sizeof(uint32_t), n + 1,
				    I915_GEM_DOMAIN_RENDER);
			n++;
		}
	}

	store_dword(fd, ctx[HI], target,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);

	igt_debugfs_dump(fd, "i915_engine_info");
	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	n++;

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(result_read[i], i);

	gem_close(fd, result);
}

static void preempt_other(int fd, unsigned ring, unsigned int flags)
{
	const struct intel_execution_engine2 *e;
	igt_spin_t *spin = NULL;
	uint32_t ctx[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [LOW] write
	 *
	 * Then on our target engine do a [HIGH] write which should then
	 * prompt its dependent LOW writes in front of the spinner on
	 * each engine. The purpose of this test is to check that preemption
	 * can cross engines.
	 */

	ctx[LO] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[NOISE] = gem_context_clone_with_engines(fd, 0);
	spin = __noise(fd, ctx[NOISE], 0, NULL);

	ctx[HI] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);

	__for_each_physical_engine(fd, e) {
		igt_debug("Primary engine: %s\n", e->name);
		__preempt_other(fd, ctx, ring, e->flags, flags);

	}

	igt_assert(gem_bo_busy(fd, spin->handle));
	igt_spin_free(fd, spin);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);
}

static void __preempt_queue(int fd,
			    unsigned target, unsigned primary,
			    unsigned depth, unsigned flags)
{
	const struct intel_execution_engine2 *e;
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read[4096 / sizeof(uint32_t)];
	igt_spin_t *above = NULL, *below = NULL;
	uint32_t ctx[3] = {
		gem_context_clone_with_engines(fd, 0),
		gem_context_clone_with_engines(fd, 0),
		gem_context_clone_with_engines(fd, 0),
	};
	int prio = MAX_PRIO;
	unsigned int n, i;

	for (n = 0; n < depth; n++) {
		if (flags & CONTEXTS) {
			gem_context_destroy(fd, ctx[NOISE]);
			ctx[NOISE] = gem_context_clone_with_engines(fd, 0);
		}
		above = __noise(fd, ctx[NOISE], prio--, above);
	}

	gem_context_set_priority(fd, ctx[HI], prio--);

	for (; n < MAX_ELSP_QLEN; n++) {
		if (flags & CONTEXTS) {
			gem_context_destroy(fd, ctx[NOISE]);
			ctx[NOISE] = gem_context_clone_with_engines(fd, 0);
		}
		below = __noise(fd, ctx[NOISE], prio--, below);
	}

	gem_context_set_priority(fd, ctx[LO], prio--);

	n = 0;
	store_dword(fd, ctx[LO], primary,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);
	n++;

	if (flags & CHAIN) {
		__for_each_physical_engine(fd, e) {
			store_dword(fd, ctx[LO], e->flags,
				    result, (n + 1)*sizeof(uint32_t), n + 1,
				    I915_GEM_DOMAIN_RENDER);
			n++;
		}
	}

	store_dword(fd, ctx[HI], target,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);

	igt_debugfs_dump(fd, "i915_engine_info");

	if (above) {
		igt_assert(gem_bo_busy(fd, above->handle));
		igt_spin_free(fd, above);
	}

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(result_read[i], i);

	if (below) {
		igt_assert(gem_bo_busy(fd, below->handle));
		igt_spin_free(fd, below);
	}

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);

	gem_close(fd, result);
}

static void preempt_queue(int fd, unsigned ring, unsigned int flags)
{
	const struct intel_execution_engine2 *e;

	for (unsigned depth = 1; depth <= MAX_ELSP_QLEN; depth *= 4)
		__preempt_queue(fd, ring, ring, depth, flags);

	__for_each_physical_engine(fd, e) {
		if (ring == e->flags)
			continue;

		__preempt_queue(fd, ring, e->flags, MAX_ELSP_QLEN, flags);
	}
}

static bool has_context_engines(int i915)
{
	struct drm_i915_gem_context_param param = {
		.ctx_id = 0,
		.param = I915_CONTEXT_PARAM_ENGINES,
	};
	return __gem_context_set_param(i915, &param) == 0;
}

static void preempt_engines(int i915,
			    const struct intel_execution_engine2 *e,
			    unsigned int flags)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines , I915_EXEC_RING_MASK + 1);
	struct drm_i915_gem_context_param param = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines),
		.size = sizeof(engines),
	};
	struct pnode {
		struct igt_list_head spinners;
		struct igt_list_head link;
	} pnode[I915_EXEC_RING_MASK + 1], *p;
	IGT_LIST_HEAD(plist);
	igt_spin_t *spin, *sn;

	/*
	 * A quick test that each engine within a context is an independent
	 * timeline that we can reprioritise and shuffle amongst themselves.
	 */

	igt_require(has_context_engines(i915));

	for (int n = 0; n <= I915_EXEC_RING_MASK; n++) {
		engines.engines[n].engine_class = e->class;
		engines.engines[n].engine_instance = e->instance;
		IGT_INIT_LIST_HEAD(&pnode[n].spinners);
		igt_list_add(&pnode[n].link, &plist);
	}
	gem_context_set_param(i915, &param);

	for (int n = -I915_EXEC_RING_MASK; n <= I915_EXEC_RING_MASK; n++) {
		unsigned int engine = n & I915_EXEC_RING_MASK;

		gem_context_set_priority(i915, param.ctx_id, n);
		spin = igt_spin_new(i915, param.ctx_id, .engine = engine);

		igt_list_move_tail(&spin->link, &pnode[engine].spinners);
		igt_list_move(&pnode[engine].link, &plist);
	}

	igt_list_for_each_entry(p, &plist, link) {
		igt_list_for_each_entry_safe(spin, sn, &p->spinners, link) {
			igt_spin_end(spin);
			gem_sync(i915, spin->handle);
			igt_spin_free(i915, spin);
		}
	}
	gem_context_destroy(i915, param.ctx_id);
}

static void preempt_self(int fd, unsigned ring)
{
	const struct intel_execution_engine2 *e;
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read[4096 / sizeof(uint32_t)];
	igt_spin_t *spin[MAX_ELSP_QLEN];
	unsigned int n, i;
	uint32_t ctx[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [self/LOW] write
	 *
	 * Then on our target engine do a [self/HIGH] write which should then
	 * preempt its own lower priority task on any engine.
	 */

	ctx[NOISE] = gem_context_clone_with_engines(fd, 0);
	ctx[HI] = gem_context_clone_with_engines(fd, 0);

	n = 0;
	gem_context_set_priority(fd, ctx[HI], MIN_PRIO);
	__for_each_physical_engine(fd, e) {
		spin[n] = __igt_spin_new(fd,
					 .ctx = ctx[NOISE],
					 .engine = e->flags);
		store_dword(fd, ctx[HI], e->flags,
			    result, (n + 1)*sizeof(uint32_t), n + 1,
			    I915_GEM_DOMAIN_RENDER);
		n++;
	}
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);
	store_dword(fd, ctx[HI], ring,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < n; i++) {
		igt_assert(gem_bo_busy(fd, spin[i]->handle));
		igt_spin_free(fd, spin[i]);
	}

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(result_read[i], i);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);

	gem_close(fd, result);
}

static void preemptive_hang(int fd, const struct intel_execution_engine2 *e)
{
	igt_spin_t *spin[MAX_ELSP_QLEN];
	igt_hang_t hang;
	uint32_t ctx[2];

	/* Set a fast timeout to speed the test up (if available) */
	set_preempt_timeout(fd, e, 150);

	ctx[HI] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		ctx[LO] = gem_context_clone_with_engines(fd, 0);
		gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

		spin[n] = __igt_spin_new(fd,
					 .ctx = ctx[LO],
					 .engine = e->flags);

		gem_context_destroy(fd, ctx[LO]);
	}

	hang = igt_hang_ctx(fd, ctx[HI], e->flags, 0);
	igt_post_hang_ring(fd, hang);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		/* Current behavior is to execute requests in order of submission.
		 * This is subject to change as the scheduler evolve. The test should
		 * be updated to reflect such changes.
		 */
		igt_assert(gem_bo_busy(fd, spin[n]->handle));
		igt_spin_free(fd, spin[n]);
	}

	gem_context_destroy(fd, ctx[HI]);
}

static void deep(int fd, unsigned ring)
{
#define XS 8
	const unsigned int max_req = MAX_PRIO - MIN_PRIO;
	const unsigned size = ALIGN(4*max_req, 4096);
	struct timespec tv = {};
	IGT_CORK_HANDLE(cork);
	unsigned int nreq;
	uint32_t plug;
	uint32_t result, dep[XS];
	uint32_t read_buf[size / sizeof(uint32_t)];
	uint32_t expected = 0;
	uint32_t *ctx;
	int dep_nreq;
	int n;

	ctx = malloc(sizeof(*ctx) * MAX_CONTEXTS);
	for (n = 0; n < MAX_CONTEXTS; n++) {
		ctx[n] = gem_context_clone_with_engines(fd, 0);
	}

	nreq = gem_submission_measure(fd, ring) / (3 * XS) * MAX_CONTEXTS;
	if (nreq > max_req)
		nreq = max_req;
	igt_info("Using %d requests (prio range %d)\n", nreq, max_req);

	result = gem_create(fd, size);
	for (int m = 0; m < XS; m ++)
		dep[m] = gem_create(fd, size);

	/* Bind all surfaces and contexts before starting the timeout. */
	{
		struct drm_i915_gem_exec_object2 obj[XS + 2];
		struct drm_i915_gem_execbuffer2 execbuf;
		const uint32_t bbe = MI_BATCH_BUFFER_END;

		memset(obj, 0, sizeof(obj));
		for (n = 0; n < XS; n++)
			obj[n].handle = dep[n];
		obj[XS].handle = result;
		obj[XS+1].handle = gem_create(fd, 4096);
		gem_write(fd, obj[XS+1].handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = XS + 2;
		execbuf.flags = ring;
		for (n = 0; n < MAX_CONTEXTS; n++) {
			execbuf.rsvd1 = ctx[n];
			gem_execbuf(fd, &execbuf);
		}
		gem_close(fd, obj[XS+1].handle);
		gem_sync(fd, result);
	}

	plug = igt_cork_plug(&cork, fd);

	/* Create a deep dependency chain, with a few branches */
	for (n = 0; n < nreq && igt_seconds_elapsed(&tv) < 2; n++) {
		const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
		struct drm_i915_gem_exec_object2 obj[3];
		struct drm_i915_gem_relocation_entry reloc;
		struct drm_i915_gem_execbuffer2 eb = {
			.buffers_ptr = to_user_pointer(obj),
			.buffer_count = 3,
			.flags = ring | (gen < 6 ? I915_EXEC_SECURE : 0),
			.rsvd1 = ctx[n % MAX_CONTEXTS],
		};
		uint32_t batch[16];
		int i;

		memset(obj, 0, sizeof(obj));
		obj[0].handle = plug;

		memset(&reloc, 0, sizeof(reloc));
		reloc.presumed_offset = 0;
		reloc.offset = sizeof(uint32_t);
		reloc.delta = sizeof(uint32_t) * n;
		reloc.read_domains = I915_GEM_DOMAIN_RENDER;
		reloc.write_domain = I915_GEM_DOMAIN_RENDER;
		obj[2].handle = gem_create(fd, 4096);
		obj[2].relocs_ptr = to_user_pointer(&reloc);
		obj[2].relocation_count = 1;

		i = 0;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = reloc.delta;
			batch[++i] = 0;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = reloc.delta;
			reloc.offset += sizeof(uint32_t);
		} else {
			batch[i]--;
			batch[++i] = reloc.delta;
		}
		batch[++i] = eb.rsvd1;
		batch[++i] = MI_BATCH_BUFFER_END;
		gem_write(fd, obj[2].handle, 0, batch, sizeof(batch));

		gem_context_set_priority(fd, eb.rsvd1, MAX_PRIO - nreq + n);
		for (int m = 0; m < XS; m++) {
			obj[1].handle = dep[m];
			reloc.target_handle = obj[1].handle;
			gem_execbuf(fd, &eb);
		}
		gem_close(fd, obj[2].handle);
	}
	igt_info("First deptree: %d requests [%.3fs]\n",
		 n * XS, 1e-9*igt_nsec_elapsed(&tv));
	dep_nreq = n;

	for (n = 0; n < nreq && igt_seconds_elapsed(&tv) < 4; n++) {
		uint32_t context = ctx[n % MAX_CONTEXTS];
		gem_context_set_priority(fd, context, MAX_PRIO - nreq + n);

		for (int m = 0; m < XS; m++) {
			store_dword_plug(fd, context, ring, result, 4*n, context, dep[m], 0);
			store_dword(fd, context, ring, result, 4*m, context, I915_GEM_DOMAIN_INSTRUCTION);
		}
		expected = context;
	}
	igt_info("Second deptree: %d requests [%.3fs]\n",
		 n * XS, 1e-9*igt_nsec_elapsed(&tv));

	unplug_show_queue(fd, &cork, ring);
	gem_close(fd, plug);
	igt_require(expected); /* too slow */

	for (n = 0; n < MAX_CONTEXTS; n++)
		gem_context_destroy(fd, ctx[n]);

	for (int m = 0; m < XS; m++) {
		__sync_read_u32_count(fd, dep[m], read_buf, sizeof(read_buf));
		gem_close(fd, dep[m]);

		for (n = 0; n < dep_nreq; n++)
			igt_assert_eq_u32(read_buf[n], ctx[n % MAX_CONTEXTS]);
	}

	__sync_read_u32_count(fd, result, read_buf, sizeof(read_buf));
	gem_close(fd, result);

	/* No reordering due to PI on all contexts because of the common dep */
	for (int m = 0; m < XS; m++)
		igt_assert_eq_u32(read_buf[m], expected);

	free(ctx);
#undef XS
}

static void alarm_handler(int sig)
{
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf))
		err = -errno;
	return err;
}

static void wide(int fd, unsigned ring)
{
	const unsigned int ring_size = gem_submission_measure(fd, ring);
	struct timespec tv = {};
	IGT_CORK_FENCE(cork);
	uint32_t result;
	uint32_t result_read[MAX_CONTEXTS];
	uint32_t *ctx;
	unsigned int count;
	int fence;

	ctx = malloc(sizeof(*ctx)*MAX_CONTEXTS);
	for (int n = 0; n < MAX_CONTEXTS; n++)
		ctx[n] = gem_context_clone_with_engines(fd, 0);

	result = gem_create(fd, 4*MAX_CONTEXTS);

	fence = igt_cork_plug(&cork, fd);

	/* Lots of in-order requests, plugged and submitted simultaneously */
	for (count = 0;
	     igt_seconds_elapsed(&tv) < 5 && count < ring_size;
	     count++) {
		for (int n = 0; n < MAX_CONTEXTS; n++) {
			store_dword_fenced(fd, ctx[n], ring, result, 4*n, ctx[n],
					   fence, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}
	igt_info("Submitted %d requests over %d contexts in %.1fms\n",
		 count, MAX_CONTEXTS, igt_nsec_elapsed(&tv) * 1e-6);

	unplug_show_queue(fd, &cork, ring);
	close(fence);

	for (int n = 0; n < MAX_CONTEXTS; n++)
		gem_context_destroy(fd, ctx[n]);

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));
	for (int n = 0; n < MAX_CONTEXTS; n++)
		igt_assert_eq_u32(result_read[n], ctx[n]);

	gem_close(fd, result);
	free(ctx);
}

static void reorder_wide(int fd, unsigned ring)
{
	const unsigned int ring_size = gem_submission_measure(fd, ring);
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const int priorities[] = { MIN_PRIO, MAX_PRIO };
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t result_read[1024];
	uint32_t result, target;
	IGT_CORK_FENCE(cork);
	uint32_t *expected;
	int fence;

	result = gem_create(fd, 4096);
	target = gem_create(fd, 4096);
	fence = igt_cork_plug(&cork, fd);

	expected = gem_mmap__cpu(fd, target, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, target, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = result;
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = 1;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = result;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = 0; /* lies */

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	execbuf.flags |= I915_EXEC_FENCE_IN;
	execbuf.rsvd2 = fence;

	for (int n = 0, x = 1; n < ARRAY_SIZE(priorities); n++, x++) {
		unsigned int sz = ALIGN(ring_size * 64, 4096);
		uint32_t *batch;

		execbuf.rsvd1 = gem_context_clone_with_engines(fd, 0);
		gem_context_set_priority(fd, execbuf.rsvd1, priorities[n]);

		obj[1].handle = gem_create(fd, sz);
		batch = gem_mmap__device_coherent(fd, obj[1].handle, 0, sz, PROT_WRITE);
		gem_set_domain(fd, obj[1].handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

		for (int m = 0; m < ring_size; m++) {
			uint64_t addr;
			int idx = hars_petruska_f54_1_random_unsafe_max(1024);
			int i;

			execbuf.batch_start_offset = m * 64;
			reloc.offset = execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = idx * sizeof(uint32_t);
			addr = reloc.presumed_offset + reloc.delta;

			i = execbuf.batch_start_offset / sizeof(uint32_t);
			batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				batch[++i] = addr;
				batch[++i] = addr >> 32;
			} else if (gen >= 4) {
				batch[++i] = 0;
				batch[++i] = addr;
				reloc.offset += sizeof(uint32_t);
			} else {
				batch[i]--;
				batch[++i] = addr;
			}
			batch[++i] = x;
			batch[++i] = MI_BATCH_BUFFER_END;

			if (!expected[idx])
				expected[idx] =  x;

			gem_execbuf(fd, &execbuf);
		}

		munmap(batch, sz);
		gem_close(fd, obj[1].handle);
		gem_context_destroy(fd, execbuf.rsvd1);
	}

	unplug_show_queue(fd, &cork, ring);
	close(fence);

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));
	for (int n = 0; n < 1024; n++)
		igt_assert_eq_u32(result_read[n], expected[n]);

	munmap(expected, 4096);

	gem_close(fd, result);
	gem_close(fd, target);
}

static void bind_to_cpu(int cpu)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct sched_param rt = {.sched_priority = 99 };
	cpu_set_t allowed;

	igt_assert(sched_setscheduler(getpid(), SCHED_RR | SCHED_RESET_ON_FORK, &rt) == 0);

	CPU_ZERO(&allowed);
	CPU_SET(cpu % ncpus, &allowed);
	igt_assert(sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed) == 0);
}

static void test_pi_ringfull(int fd, unsigned int engine, unsigned int flags)
#define SHARED BIT(0)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	unsigned int last, count;
	struct itimerval itv;
	IGT_CORK_HANDLE(c);
	uint32_t vip;
	bool *result;

	/*
	 * We start simple. A low priority client should never prevent a high
	 * priority client from submitting their work; even if the low priority
	 * client exhausts their ringbuffer and so is throttled.
	 *
	 * SHARED: A variant on the above rule is that even is the 2 clients
	 * share a read-only resource, the blocked low priority client should
	 * not prevent the high priority client from executing. A buffer,
	 * e.g. the batch buffer, that is shared only for reads (no write
	 * hazard, so the reads can be executed in parallel or in any order),
	 * so not cause priority inversion due to the resource conflict.
	 *
	 * First, we have the low priority context who fills their ring and so
	 * blocks. As soon as that context blocks, we try to submit a high
	 * priority batch, which should be executed immediately before the low
	 * priority context is unblocked.
	 */

	result = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(result != MAP_FAILED);

	memset(&execbuf, 0, sizeof(execbuf));
	memset(&obj, 0, sizeof(obj));

	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.flags = engine;

	/* Warm up both (hi/lo) contexts */
	execbuf.rsvd1 = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, execbuf.rsvd1, MAX_PRIO);
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);
	vip = execbuf.rsvd1;

	execbuf.rsvd1 = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, execbuf.rsvd1, MIN_PRIO);
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);

	/* Fill the low-priority ring */
	obj[0].handle = igt_cork_plug(&c, fd);

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 10000;
	setitimer(ITIMER_REAL, &itv, NULL);

	last = -1;
	count = 0;
	do {
		if (__execbuf(fd, &execbuf) == 0) {
			count++;
			continue;
		}

		if (last == count)
			break;

		last = count;
	} while (1);
	igt_debug("Filled low-priority ring with %d batches\n", count);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;

	/* both parent + child on the same cpu, only parent is RT */
	bind_to_cpu(0);

	igt_fork(child, 1) {
		int err;

		/* Replace our batch to avoid conflicts over shared resources? */
		if (!(flags & SHARED)) {
			obj[1].handle = gem_create(fd, 4096);
			gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));
		}

		result[0] = vip != execbuf.rsvd1;

		igt_debug("Waking parent\n");
		kill(getppid(), SIGALRM);
		sched_yield();
		result[1] = true;

		sigaction(SIGALRM, &sa, NULL);
		itv.it_value.tv_sec = 0;
		itv.it_value.tv_usec = 10000;
		setitimer(ITIMER_REAL, &itv, NULL);

		/*
		 * Since we are the high priority task, we expect to be
		 * able to add ourselves to *our* ring without interruption.
		 */
		igt_debug("HP child executing\n");
		execbuf.rsvd1 = vip;
		err = __execbuf(fd, &execbuf);
		igt_debug("HP execbuf returned %d\n", err);

		memset(&itv, 0, sizeof(itv));
		setitimer(ITIMER_REAL, &itv, NULL);

		result[2] = err == 0;

		if (!(flags & SHARED))
			gem_close(fd, obj[1].handle);
	}

	/* Relinquish CPU just to allow child to create a context */
	sleep(1);
	igt_assert_f(result[0], "HP context (child) not created\n");
	igt_assert_f(!result[1], "Child released too early!\n");

	/* Parent sleeps waiting for ringspace, releasing child */
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 50000;
	setitimer(ITIMER_REAL, &itv, NULL);
	igt_debug("LP parent executing\n");
	igt_assert_eq(__execbuf(fd, &execbuf), -EINTR);
	igt_assert_f(result[1], "Child was not released!\n");
	igt_assert_f(result[2],
		     "High priority child unable to submit within 10ms\n");

	igt_cork_unplug(&c);
	igt_waitchildren();

	gem_context_destroy(fd, execbuf.rsvd1);
	gem_context_destroy(fd, vip);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
	munmap(result, 4096);
}

static int userfaultfd(int flags)
{
	return syscall(SYS_userfaultfd, flags);
}

struct ufd_thread {
	uint32_t batch;
	uint32_t scratch;
	uint32_t *page;
	unsigned int engine;
	unsigned int flags;
	int i915;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int count;
};

static uint32_t create_userptr(int i915, void *page)
{
	uint32_t handle;

	gem_userptr(i915, page, 4096, 0, 0, &handle);
	return handle;
}

static void *ufd_thread(void *arg)
{
	struct ufd_thread *t = arg;
	struct drm_i915_gem_exec_object2 obj[2] = {
		{ .handle = create_userptr(t->i915, t->page) },
		{ .handle = t->batch },
	};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = t->engine,
		.rsvd1 = gem_context_clone_with_engines(t->i915, 0),
	};
	gem_context_set_priority(t->i915, eb.rsvd1, MIN_PRIO);

	igt_debug("submitting fault\n");
	gem_execbuf(t->i915, &eb);
	gem_sync(t->i915, obj[0].handle);
	gem_close(t->i915, obj[0].handle);

	gem_context_destroy(t->i915, eb.rsvd1);

	t->i915 = -1;
	return NULL;
}

static void test_pi_userfault(int i915, unsigned int engine)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg;
	struct uffdio_copy copy;
	struct uffd_msg msg;
	struct ufd_thread t;
	pthread_t thread;
	char buf[4096];
	char *poison;
	int ufd;

	/*
	 * Resource contention can easily lead to priority inversion problems,
	 * that we wish to avoid. Here, we simulate one simple form of resource
	 * starvation by using an arbitrary slow userspace fault handler to cause
	 * the low priority context to block waiting for its resource. While it
	 * is blocked, it should not prevent a higher priority context from
	 * executing.
	 *
	 * This is only a very simple scenario, in more general tests we will
	 * need to simulate contention on the shared resource such that both
	 * low and high priority contexts are starving and must fight over
	 * the meagre resources. One step at a time.
	 */

	ufd = userfaultfd(0);
	igt_require_f(ufd != -1, "kernel support for userfaultfd\n");
	igt_require_f(ioctl(ufd, UFFDIO_API, &api) == 0 && api.api == UFFD_API,
		      "userfaultfd API v%lld:%lld\n", UFFD_API, api.api);

	t.i915 = i915;
	t.engine = engine;

	t.page = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	igt_assert(t.page != MAP_FAILED);

	t.batch = gem_create(i915, 4096);
	poison = gem_mmap__device_coherent(i915, t.batch, 0, 4096, PROT_WRITE);
	memset(poison, 0xff, 4096);

	/* Register our fault handler for t.page */
	memset(&reg, 0, sizeof(reg));
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.range.start = to_user_pointer(t.page);
	reg.range.len = 4096;
	do_ioctl(ufd, UFFDIO_REGISTER, &reg);

	/* Kick off the low priority submission */
	igt_assert(pthread_create(&thread, NULL, ufd_thread, &t) == 0);

	/* Wait until the low priority thread is blocked on a fault */
	igt_assert_eq(read(ufd, &msg, sizeof(msg)), sizeof(msg));
	igt_assert_eq(msg.event, UFFD_EVENT_PAGEFAULT);
	igt_assert(from_user_pointer(msg.arg.pagefault.address) == t.page);

	/* While the low priority context is blocked; execute a vip */
	if (1) {
		struct drm_i915_gem_exec_object2 obj = {
			.handle = gem_create(i915, 4096),
		};
		struct pollfd pfd;
		struct drm_i915_gem_execbuffer2 eb = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
			.flags = engine | I915_EXEC_FENCE_OUT,
			.rsvd1 = gem_context_clone_with_engines(i915, 0),
		};
		gem_context_set_priority(i915, eb.rsvd1, MAX_PRIO);
		gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));
		gem_execbuf_wr(i915, &eb);
		gem_close(i915, obj.handle);

		memset(&pfd, 0, sizeof(pfd));
		pfd.fd = eb.rsvd2 >> 32;
		pfd.events = POLLIN;
		poll(&pfd, 1, -1);
		igt_assert_eq(sync_fence_status(pfd.fd), 1);
		close(pfd.fd);

		gem_context_destroy(i915, eb.rsvd1);
	}

	/* Confirm the low priority context is still waiting */
	igt_assert_eq(t.i915, i915);
	memcpy(poison, &bbe, sizeof(bbe));
	munmap(poison, 4096);

	/* Service the fault; releasing the low priority context */
	memset(&copy, 0, sizeof(copy));
	copy.dst = msg.arg.pagefault.address;
	copy.src = to_user_pointer(memset(buf, 0xc5, sizeof(buf)));
	copy.len = 4096;
	do_ioctl(ufd, UFFDIO_COPY, &copy);

	pthread_join(thread, NULL);

	gem_close(i915, t.batch);
	munmap(t.page, 4096);
	close(ufd);
}

static void *iova_thread(struct ufd_thread *t, int prio)
{
	unsigned int clone;
	uint32_t ctx;

	clone = I915_CONTEXT_CLONE_ENGINES;
	if (t->flags & SHARED)
		clone |= I915_CONTEXT_CLONE_VM;

	ctx = gem_context_clone(t->i915, 0, clone, 0);
	gem_context_set_priority(t->i915, ctx, prio);

	store_dword_plug(t->i915, ctx, t->engine,
			 t->scratch, 0, prio,
			 t->batch, 0 /* no write hazard! */);

	pthread_mutex_lock(&t->mutex);
	if (!--t->count)
		pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->mutex);

	gem_context_destroy(t->i915, ctx);
	return NULL;
}

static void *iova_low(void *arg)
{
	return iova_thread(arg, MIN_PRIO);
}

static void *iova_high(void *arg)
{
	return iova_thread(arg, MAX_PRIO);
}

static void test_pi_iova(int i915, unsigned int engine, unsigned int flags)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg;
	struct uffdio_copy copy;
	struct uffd_msg msg;
	struct ufd_thread t;
	igt_spin_t *spin;
	pthread_t hi, lo;
	char poison[4096];
	int ufd;

	/*
	 * In this scenario, we have a pair of contending contexts that
	 * share the same resource. That resource is stuck behind a slow
	 * page fault such that neither context has immediate access to it.
	 * What is expected is that as soon as that resource becomes available,
	 * the two contexts are queued with the high priority context taking
	 * precedence. We need to check that we do not cross-contaminate
	 * the two contents with the page fault on the shared resource
	 * initiated by the low priority context. (Consider that the low
	 * priority context may install an exclusive fence for the page
	 * fault, which is then used for strict ordering by the high priority
	 * context, causing an unwanted implicit dependency between the two
	 * and promoting the low priority context to high.)
	 *
	 * SHARED: the two contexts share a vm, but still have separate
	 * timelines that should not mingle.
	 */

	ufd = userfaultfd(0);
	igt_require_f(ufd != -1, "kernel support for userfaultfd\n");
	igt_require_f(ioctl(ufd, UFFDIO_API, &api) == 0 && api.api == UFFD_API,
		      "userfaultfd API v%lld:%lld\n", UFFD_API, api.api);

	t.i915 = i915;
	t.engine = engine;
	t.flags = flags;

	t.count = 2;
	pthread_cond_init(&t.cond, NULL);
	pthread_mutex_init(&t.mutex, NULL);

	t.page = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	igt_assert(t.page != MAP_FAILED);
	t.batch = create_userptr(i915, t.page);
	t.scratch = gem_create(i915, 4096);

	/* Register our fault handler for t.page */
	memset(&reg, 0, sizeof(reg));
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.range.start = to_user_pointer(t.page);
	reg.range.len = 4096;
	do_ioctl(ufd, UFFDIO_REGISTER, &reg);

	/*
	 * Fill the engine with spinners; the store_dword() is too quick!
	 *
	 * It is not that it is too quick, it that the order in which the
	 * requests are signaled from the pagefault completion is loosely
	 * defined (currently, it's in order of attachment so low context
	 * wins), then submission into the execlists is immediate with the
	 * low context filling the last slot in the ELSP. Preemption will
	 * not take place until after the low priority context has had a
	 * chance to run, and since the task is very short there is no
	 * arbitration point inside the batch buffer so we only preempt
	 * after the low priority context has completed.
	 *
	 * One way to prevent such opportunistic execution of the low priority
	 * context would be to remove direct submission and wait until all
	 * signals are delivered (as the signal delivery is under the irq lock,
	 * the local tasklet will not run until after all signals have been
	 * delivered... but another tasklet might).
	 */
	spin = igt_spin_new(i915, .engine = engine);
	for (int i = 0; i < MAX_ELSP_QLEN; i++) {
		spin->execbuf.rsvd1 = create_highest_priority(i915);
		gem_execbuf(i915, &spin->execbuf);
		gem_context_destroy(i915, spin->execbuf.rsvd1);
	}

	/* Kick off the submission threads */
	igt_assert(pthread_create(&lo, NULL, iova_low, &t) == 0);

	/* Wait until the low priority thread is blocked on the fault */
	igt_assert_eq(read(ufd, &msg, sizeof(msg)), sizeof(msg));
	igt_assert_eq(msg.event, UFFD_EVENT_PAGEFAULT);
	igt_assert(from_user_pointer(msg.arg.pagefault.address) == t.page);

	/* Then release a very similar thread, but at high priority! */
	igt_assert(pthread_create(&hi, NULL, iova_high, &t) == 0);

	/* Service the fault; releasing both contexts */
	memset(&copy, 0, sizeof(copy));
	copy.dst = msg.arg.pagefault.address;
	copy.src = to_user_pointer(memset(poison, 0xc5, sizeof(poison)));
	copy.len = 4096;
	do_ioctl(ufd, UFFDIO_COPY, &copy);

	/* Wait until both threads have had a chance to submit */
	pthread_mutex_lock(&t.mutex);
	while (t.count)
		pthread_cond_wait(&t.cond, &t.mutex);
	pthread_mutex_unlock(&t.mutex);
	igt_debugfs_dump(i915, "i915_engine_info");
	igt_spin_free(i915, spin);

	pthread_join(hi, NULL);
	pthread_join(lo, NULL);
	gem_close(i915, t.batch);

	igt_assert_eq(__sync_read_u32(i915, t.scratch, 0), MIN_PRIO);
	gem_close(i915, t.scratch);

	munmap(t.page, 4096);
	close(ufd);
}

static void measure_semaphore_power(int i915)
{
	const struct intel_execution_engine2 *signaler, *e;
	struct rapl gpu, pkg;

	igt_require(gpu_power_open(&gpu) == 0);
	pkg_power_open(&pkg);

	__for_each_physical_engine(i915, signaler) {
		struct {
			struct power_sample pkg, gpu;
		} s_spin[2], s_sema[2];
		double baseline, total;
		int64_t jiffie = 1;
		igt_spin_t *spin;

		if (!gem_class_can_store_dword(i915, signaler->class))
			continue;

		spin = __igt_spin_new(i915,
				      .engine = signaler->flags,
				      .flags = IGT_SPIN_POLL_RUN);
		gem_wait(i915, spin->handle, &jiffie); /* waitboost */
		igt_spin_busywait_until_started(spin);

		rapl_read(&pkg, &s_spin[0].pkg);
		rapl_read(&gpu, &s_spin[0].gpu);
		usleep(100*1000);
		rapl_read(&gpu, &s_spin[1].gpu);
		rapl_read(&pkg, &s_spin[1].pkg);

		/* Add a waiter to each engine */
		__for_each_physical_engine(i915, e) {
			igt_spin_t *sema;

			if (e->flags == signaler->flags)
				continue;

			sema = __igt_spin_new(i915,
					      .engine = e->flags,
					      .dependency = spin->handle);

			igt_spin_free(i915, sema);
		}
		usleep(10); /* just give the tasklets a chance to run */

		rapl_read(&pkg, &s_sema[0].pkg);
		rapl_read(&gpu, &s_sema[0].gpu);
		usleep(100*1000);
		rapl_read(&gpu, &s_sema[1].gpu);
		rapl_read(&pkg, &s_sema[1].pkg);

		igt_spin_free(i915, spin);

		baseline = power_W(&gpu, &s_spin[0].gpu, &s_spin[1].gpu);
		total = power_W(&gpu, &s_sema[0].gpu, &s_sema[1].gpu);
		igt_info("%s: %.1fmW + %.1fmW (total %1.fmW)\n",
			 signaler->name,
			 1e3 * baseline,
			 1e3 * (total - baseline),
			 1e3 * total);

		if (rapl_valid(&pkg)) {
			baseline = power_W(&pkg, &s_spin[0].pkg, &s_spin[1].pkg);
			total = power_W(&pkg, &s_sema[0].pkg, &s_sema[1].pkg);
			igt_info("pkg: %.1fmW + %.1fmW (total %1.fmW)\n",
				 1e3 * baseline,
				 1e3 * (total - baseline),
				 1e3 * total);
		}
	}

	rapl_close(&gpu);
	rapl_close(&pkg);
}

static int read_timestamp_frequency(int i915)
{
	int value = 0;
	drm_i915_getparam_t gp = {
		.value = &value,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};
	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return value;
}

static uint64_t div64_u64_round_up(uint64_t x, uint64_t y)
{
	return (x + y - 1) / y;
}

static uint64_t ticks_to_ns(int i915, uint64_t ticks)
{
	return div64_u64_round_up(ticks * NSEC_PER_SEC,
				  read_timestamp_frequency(i915));
}

static int cmp_u32(const void *A, const void *B)
{
	const uint32_t *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static uint32_t read_ctx_timestamp(int i915,
				   uint32_t ctx,
				   const struct intel_execution_engine2 *e)
{
	const int use_64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	const uint32_t base = gem_engine_mmio_base(i915, e->name);
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
		.offset = 32 << 20,
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags,
		.rsvd1 = ctx,
	};
#define RUNTIME (base + 0x3a8)
	uint32_t *map, *cs;
	uint32_t ts;

	igt_require(base);

	cs = map = gem_mmap__device_coherent(i915, obj.handle,
					     0, 4096, PROT_WRITE);

	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = RUNTIME;
	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj.handle;
	reloc.presumed_offset = obj.offset;
	reloc.offset = offset_in_page(cs);
	reloc.delta = 4000;
	*cs++ = obj.offset + 4000;
	*cs++ = obj.offset >> 32;

	*cs++ = MI_BATCH_BUFFER_END;

	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	ts = map[1000];
	munmap(map, 4096);

	return ts;
}

static void fairslice(int i915,
		      const struct intel_execution_engine2 *e,
		      unsigned long flags,
		      int duration)
{
	const double timeslice_duration_ns = 1e6;
	igt_spin_t *spin = NULL;
	double threshold;
	uint32_t ctx[3];
	uint32_t ts[3];

	for (int i = 0; i < ARRAY_SIZE(ctx); i++) {
		ctx[i] = gem_context_clone_with_engines(i915, 0);
		if (spin == NULL) {
			spin = __igt_spin_new(i915,
					      .ctx = ctx[i],
					      .engine = e->flags,
					      .flags = flags);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.flags = e->flags,
				.rsvd1 = ctx[i],
			};
			gem_execbuf(i915, &eb);
		}
	}

	sleep(duration); /* over the course of many timeslices */

	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_spin_end(spin);
	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		ts[i] = read_ctx_timestamp(i915, ctx[i], e);

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		gem_context_destroy(i915, ctx[i]);
	igt_spin_free(i915, spin);

	/*
	 * If we imagine that the timeslices are randomly distributed to
	 * the clients, we would expect the variance to be modelled
	 * by a drunken walk; ergo sqrt(num_timeslices).
	 */
	threshold = sqrt(1e9 * duration / timeslice_duration_ns);
	threshold *= timeslice_duration_ns;
	threshold *= 2; /* CI safety factor before crying wolf */

	qsort(ts, 3, sizeof(*ts), cmp_u32);
	igt_info("%s: [%.1f, %.1f, %.1f] ms, expect %1.f +- %.1fms\n", e->name,
		 1e-6 * ticks_to_ns(i915, ts[0]),
		 1e-6 * ticks_to_ns(i915, ts[1]),
		 1e-6 * ticks_to_ns(i915, ts[2]),
		 1e3 * duration / 3,
		 1e-6 * threshold);

	igt_assert_f(ts[2], "CTX_TIMESTAMP not reported!\n");
	igt_assert_f(ticks_to_ns(i915, ts[2] - ts[0]) < 2 * threshold,
		     "Range of timeslices greater than tolerable: %.2fms > %.2fms; unfair!\n",
		     1e-6 * ticks_to_ns(i915, ts[2] - ts[0]),
		     1e-6 * threshold * 2);
}

#define test_each_engine(T, i915, e) \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		igt_dynamic_f("%s", e->name)

#define test_each_engine_store(T, i915, e) \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		for_each_if(gem_class_can_store_dword(fd, e->class)) \
		igt_dynamic_f("%s", e->name)

igt_main
{
	int fd = -1;

	igt_fixture {
		igt_require_sw_sync();

		fd = drm_open_driver_master(DRIVER_INTEL);
		gem_submission_print_method(fd);
		gem_scheduler_print_capability(fd);

		igt_require_gem(fd);
		gem_require_mmap_wc(fd);
		gem_require_contexts(fd);

		igt_fork_hang_detector(fd);
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		test_each_engine_store("fifo", fd, e)
			fifo(fd, e->flags);

		test_each_engine_store("implicit-read-write", fd, e)
			implicit_rw(fd, e->flags, READ_WRITE);

		test_each_engine_store("implicit-write-read", fd, e)
			implicit_rw(fd, e->flags, WRITE_READ);

		test_each_engine_store("implicit-boths", fd, e)
			implicit_rw(fd, e->flags, READ_WRITE | WRITE_READ);

		test_each_engine_store("independent", fd, e)
			independent(fd, e->flags, 0);
		test_each_engine_store("u-independent", fd, e)
			independent(fd, e->flags, IGT_SPIN_USERPTR);
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_ctx_priority(fd));
		}

		test_each_engine("timeslicing", fd, e)
			timeslice(fd, e->flags);

		test_each_engine("thriceslice", fd, e)
			timesliceN(fd, e->flags, 3);

		test_each_engine("manyslice", fd, e)
			timesliceN(fd, e->flags, 67);

		test_each_engine("lateslice", fd, e)
			lateslice(fd, e->flags, 0);
		test_each_engine("u-lateslice", fd, e)
			lateslice(fd, e->flags, IGT_SPIN_USERPTR);

		igt_subtest_group {
			igt_fixture {
				igt_require(gem_scheduler_has_semaphores(fd));
				igt_require(gem_scheduler_has_preemption(fd));
				igt_require(intel_gen(intel_get_drm_devid(fd)) >= 8);
			}

			test_each_engine("fairslice", fd, e)
				fairslice(fd, e, 0, 2);

			test_each_engine("u-fairslice", fd, e)
				fairslice(fd, e, IGT_SPIN_USERPTR, 2);

			igt_subtest("fairslice-all")  {
				__for_each_physical_engine(fd, e) {
					igt_fork(child, 1)
						fairslice(fd, e, 0, 2);
				}
				igt_waitchildren();
			}
			igt_subtest("u-fairslice-all")  {
				__for_each_physical_engine(fd, e) {
					igt_fork(child, 1)
						fairslice(fd, e,
							  IGT_SPIN_USERPTR,
							  2);
				}
				igt_waitchildren();
			}
		}

		test_each_engine("submit-early-slice", fd, e)
			submit_slice(fd, e, EARLY_SUBMIT);
		test_each_engine("u-submit-early-slice", fd, e)
			submit_slice(fd, e, EARLY_SUBMIT | USERPTR);
		test_each_engine("submit-golden-slice", fd, e)
			submit_slice(fd, e, 0);
		test_each_engine("u-submit-golden-slice", fd, e)
			submit_slice(fd, e, USERPTR);
		test_each_engine("submit-late-slice", fd, e)
			submit_slice(fd, e, LATE_SUBMIT);
		test_each_engine("u-submit-late-slice", fd, e)
			submit_slice(fd, e, LATE_SUBMIT | USERPTR);

		igt_subtest("semaphore-user")
			semaphore_userlock(fd, 0);
		igt_subtest("semaphore-codependency")
			semaphore_codependency(fd, 0);
		igt_subtest("semaphore-resolve")
			semaphore_resolve(fd, 0);
		igt_subtest("semaphore-noskip")
			semaphore_noskip(fd, 0);

		igt_subtest("u-semaphore-user")
			semaphore_userlock(fd, IGT_SPIN_USERPTR);
		igt_subtest("u-semaphore-codependency")
			semaphore_codependency(fd, IGT_SPIN_USERPTR);
		igt_subtest("u-semaphore-resolve")
			semaphore_resolve(fd, IGT_SPIN_USERPTR);
		igt_subtest("u-semaphore-noskip")
			semaphore_noskip(fd, IGT_SPIN_USERPTR);

		igt_subtest("smoketest-all")
			smoketest(fd, ALL_ENGINES, 30);

		test_each_engine_store("in-order", fd, e)
			reorder(fd, e->flags, EQUAL);

		test_each_engine_store("out-order", fd, e)
			reorder(fd, e->flags, 0);

		test_each_engine_store("promotion", fd, e)
			promotion(fd, e->flags);

		igt_subtest_group {
			igt_fixture {
				igt_require(gem_scheduler_has_preemption(fd));
			}

			test_each_engine_store("preempt", fd, e)
				preempt(fd, e, 0);

			test_each_engine_store("preempt-contexts", fd, e)
				preempt(fd, e, NEW_CTX);

			test_each_engine_store("preempt-user", fd, e)
				preempt(fd, e, USERPTR);

			test_each_engine_store("preempt-self", fd, e)
				preempt_self(fd, e->flags);

			test_each_engine_store("preempt-other", fd, e)
				preempt_other(fd, e->flags, 0);

			test_each_engine_store("preempt-other-chain", fd, e)
				preempt_other(fd, e->flags, CHAIN);

			test_each_engine_store("preempt-queue", fd, e)
				preempt_queue(fd, e->flags, 0);

			test_each_engine_store("preempt-queue-chain", fd, e)
				preempt_queue(fd, e->flags, CHAIN);
			test_each_engine_store("preempt-queue-contexts", fd, e)
				preempt_queue(fd, e->flags, CONTEXTS);

			test_each_engine_store("preempt-queue-contexts-chain", fd, e)
				preempt_queue(fd, e->flags, CONTEXTS | CHAIN);

			test_each_engine_store("preempt-engines", fd, e)
				preempt_engines(fd, e, 0);

			igt_subtest_group {
				igt_hang_t hang;

				igt_fixture {
					igt_stop_hang_detector();
					hang = igt_allow_hang(fd, 0, 0);
				}

				test_each_engine_store("preempt-hang", fd, e)
					preempt(fd, e, NEW_CTX | HANG_LP);

				test_each_engine_store("preemptive-hang", fd, e)
					preemptive_hang(fd, e);

				igt_fixture {
					igt_disallow_hang(fd, hang);
					igt_fork_hang_detector(fd);
				}
			}
		}

		test_each_engine_store("noreorder", fd, e)
			noreorder(fd, e->flags, 0, 0);

		test_each_engine_store("noreorder-priority", fd, e) {
			igt_require(gem_scheduler_enabled(fd));
			noreorder(fd, e->flags, MAX_PRIO, 0);
		}

		test_each_engine_store("noreorder-corked", fd, e) {
			igt_require(gem_scheduler_enabled(fd));
			noreorder(fd, e->flags, MAX_PRIO, CORKED);
		}

		test_each_engine_store("deep", fd, e)
			deep(fd, e->flags);

		test_each_engine_store("wide", fd, e)
			wide(fd, e->flags);

		test_each_engine_store("reorder-wide", fd, e)
			reorder_wide(fd, e->flags);

		test_each_engine_store("smoketest", fd, e)
			smoketest(fd, e->flags, 5);
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_ctx_priority(fd));
			igt_require(gem_scheduler_has_preemption(fd));
		}

		test_each_engine("pi-ringfull", fd, e)
			test_pi_ringfull(fd, e->flags, 0);

		test_each_engine("pi-common", fd, e)
			test_pi_ringfull(fd, e->flags, SHARED);

		test_each_engine("pi-userfault", fd, e)
			test_pi_userfault(fd, e->flags);

		test_each_engine("pi-distinct-iova", fd, e)
			test_pi_iova(fd, e->flags, 0);

		test_each_engine("pi-shared-iova", fd, e)
			test_pi_iova(fd, e->flags, SHARED);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_semaphores(fd));
		}

		igt_subtest("semaphore-power")
			measure_semaphore_power(fd);
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
