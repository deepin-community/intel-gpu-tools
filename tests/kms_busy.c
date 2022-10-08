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

#include <sys/poll.h>
#include <signal.h>
#include <time.h>

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Basic check of KMS ABI with busy framebuffers.");

static igt_output_t *
set_fb_on_crtc(igt_display_t *dpy, int pipe, struct igt_fb *fb)
{
	drmModeModeInfoPtr mode;
	igt_plane_t *primary;
	igt_output_t *output;

	output = igt_get_single_output_for_pipe(dpy, pipe);

	igt_output_set_pipe(output, pipe);
	mode = igt_output_get_mode(output);

	igt_create_pattern_fb(dpy->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED, fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, fb);

	return output;
}

static void do_cleanup_display(igt_display_t *dpy)
{
	enum pipe pipe;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_pipe(dpy, pipe)
		for_each_plane_on_pipe(dpy, pipe, plane)
			igt_plane_set_fb(plane, NULL);

	for_each_connected_output(dpy, output)
		igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void flip_to_fb(igt_display_t *dpy, int pipe,
		       igt_output_t *output,
		       struct igt_fb *fb, int timeout,
		       const char *name, bool modeset)
{
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	struct drm_event_vblank ev;
	IGT_CORK_FENCE(cork);
	igt_spin_t *t;
	int fence;

	fence = igt_cork_plug(&cork, dpy->drm_fd);
	t = igt_spin_new(dpy->drm_fd,
			 .fence = fence,
			 .dependency = fb->gem_handle,
			 .flags = IGT_SPIN_FENCE_IN);
	close(fence);

	igt_fork(child, 1) {
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		if (!modeset)
			do_or_die(drmModePageFlip(dpy->drm_fd,
						  dpy->pipes[pipe].crtc_id, fb->fb_id,
						  DRM_MODE_PAGE_FLIP_EVENT, fb));
		else {
			igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY), fb);
			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit_atomic(dpy,
						  DRM_MODE_ATOMIC_NONBLOCK |
						  DRM_MODE_PAGE_FLIP_EVENT |
						  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		}

		igt_assert_f(poll(&pfd, 1, timeout) == 0,
			     "flip completed whilst %s was busy [%d]\n",
			     name, gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));

		igt_cork_unplug(&cork);
	}

	igt_waitchildren_timeout(5 * timeout,
				 "flip blocked waiting for busy bo\n");
	igt_spin_end(t);
	igt_cork_unplug(&cork);

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	igt_assert(poll(&pfd, 1, 0) == 0);

	if (modeset) {
		gem_quiescent_gpu(dpy->drm_fd);

		/* Clear old mode blob. */
		igt_pipe_refresh(dpy, pipe, true);

		igt_output_set_pipe(output, pipe);
		igt_display_commit2(dpy, COMMIT_ATOMIC);
	}

	igt_spin_free(dpy->drm_fd, t);
}

static void test_flip(igt_display_t *dpy, int pipe, bool modeset)
{
	struct igt_fb fb[2];
	int warmup[] = { 0, 1, 0, -1 };
	struct timespec tv = {};
	igt_output_t *output;
	int timeout;

	if (modeset)
		igt_require(dpy->is_atomic);

	output = set_fb_on_crtc(dpy, pipe, &fb[0]);
	igt_display_commit2(dpy, COMMIT_LEGACY);

	igt_create_pattern_fb(dpy->drm_fd,
			      fb[0].width, fb[0].height,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED,
			      &fb[1]);

	/* Bind both fb to the display (such that they are ready for future
	 * flips without stalling for the bind) leaving fb[0] as bound.
	 */
	igt_nsec_elapsed(&tv);
	for (int i = 0; warmup[i] != -1; i++) {
		struct drm_event_vblank ev;

		do_or_die(drmModePageFlip(dpy->drm_fd,
					  dpy->pipes[pipe].crtc_id,
					  fb[warmup[i]].fb_id,
					  DRM_MODE_PAGE_FLIP_EVENT,
					  &fb[warmup[i]]));
		igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	}
	timeout = igt_nsec_elapsed(&tv) / 1000 / 1000;
	igt_info("Using timeout of %dms\n", timeout);

	/* Make the frontbuffer busy and try to flip to itself */
	flip_to_fb(dpy, pipe, output, &fb[0], timeout, "fb[0]", modeset);

	/* Repeat for flip to second buffer */
	flip_to_fb(dpy, pipe, output, &fb[1], timeout, "fb[1]", modeset);

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);
}

static void test_atomic_commit_hang(igt_display_t *dpy, igt_plane_t *primary,
				    struct igt_fb *busy_fb)
{
	igt_spin_t *t = igt_spin_new(dpy->drm_fd,
				     .dependency = busy_fb->gem_handle,
				     .flags = IGT_SPIN_NO_PREEMPTION);
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	unsigned flags = 0;
	struct drm_event_vblank ev;

	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	flags |= DRM_MODE_ATOMIC_NONBLOCK;
	flags |= DRM_MODE_PAGE_FLIP_EVENT;

	igt_display_commit_atomic(dpy, flags, NULL);

	igt_fork(child, 1) {
		/*
		 * bit of a hack, just set atomic commit to NULL fb to make sure
		 * that we don't wait for the new update to complete.
		 */
		igt_plane_set_fb(primary, NULL);
		igt_display_commit_atomic(dpy, 0, NULL);

		igt_assert_f(poll(&pfd, 1, 1) > 0,
			    "nonblocking update completed whilst fb[%d] was still busy [%d]\n",
			    busy_fb->fb_id, gem_bo_busy(dpy->drm_fd, busy_fb->gem_handle));
	}

	igt_waitchildren();

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_spin_end(t);
}

static void test_hang(igt_display_t *dpy,
		      enum pipe pipe, bool modeset, bool hang_newfb)
{
	struct igt_fb fb[2];
	igt_output_t *output;
	igt_plane_t *primary;

	output = set_fb_on_crtc(dpy, pipe, &fb[0]);
	igt_display_commit2(dpy, COMMIT_ATOMIC);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_pattern_fb(dpy->drm_fd,
			      fb[0].width, fb[0].height,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED,
			      &fb[1]);

	if (modeset) {
		/* Test modeset disable with hang */
		igt_output_set_pipe(output, PIPE_NONE);
		igt_plane_set_fb(primary, &fb[1]);
		test_atomic_commit_hang(dpy, primary, &fb[hang_newfb]);

		/* Test modeset enable with hang */
		igt_plane_set_fb(primary, &fb[0]);
		igt_output_set_pipe(output, pipe);
		test_atomic_commit_hang(dpy, primary, &fb[!hang_newfb]);
	} else {
		/*
		 * Test what happens with a single hanging pageflip.
		 * This always completes early, because we have some
		 * timeouts taking care of it.
		 */
		igt_plane_set_fb(primary, &fb[1]);
		test_atomic_commit_hang(dpy, primary, &fb[hang_newfb]);
	}

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);
}

static void test_pageflip_modeset_hang(igt_display_t *dpy, enum pipe pipe)
{
	struct igt_fb fb;
	struct drm_event_vblank ev;
	igt_output_t *output;
	igt_plane_t *primary;
	igt_spin_t *t;

	output = set_fb_on_crtc(dpy, pipe, &fb);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	t = igt_spin_new(dpy->drm_fd,
			 .dependency = fb.gem_handle,
			 .flags = IGT_SPIN_NO_PREEMPTION);

	do_or_die(drmModePageFlip(dpy->drm_fd, dpy->pipes[pipe].crtc_id, fb.fb_id, DRM_MODE_PAGE_FLIP_EVENT, &fb));

	/* Kill crtc with hung fb */
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_spin_end(t);

	igt_remove_fb(dpy->drm_fd, &fb);
}

igt_main
{
	igt_display_t display = { .drm_fd = -1, .n_pipes = IGT_MAX_PIPES };
	enum pipe n;

	igt_fixture {
		int fd = drm_open_driver_master(DRIVER_INTEL);

		igt_require_gem(fd);
		gem_require_mmap_wc(fd);
		igt_require(gem_has_ring(fd, I915_EXEC_DEFAULT));

		kmstest_set_vt_graphics_mode();
		igt_display_require(&display, fd);
	}

	/* XXX Extend to cover atomic rendering tests to all planes + legacy */

	igt_describe("Test for basic check of KMS ABI with busy framebuffers.");
	igt_subtest_with_dynamic("basic") { /* just run on the first pipe */
		enum pipe pipe;
		igt_output_t *output;

		for_each_pipe_with_valid_output(&display, pipe, output) {
			igt_dynamic("flip")
				test_flip(&display, pipe, false);
			igt_dynamic("modeset")
				test_flip(&display, pipe, true);
			break;
		}
	}

	for_each_pipe_static(n) igt_subtest_group {
		igt_hang_t hang;

		errno = 0;

		igt_fixture {
			igt_display_require_output_on_pipe(&display, n);
		}

		igt_describe("Tests basic flip on pipe.");
		igt_subtest_f("basic-flip-pipe-%s", kmstest_pipe_name(n)) {
			test_flip(&display, n, false);
		}
		igt_describe("Tests basic modeset on pipe.");
		igt_subtest_f("basic-modeset-pipe-%s", kmstest_pipe_name(n)) {

			test_flip(&display, n, true);
		}

		igt_fixture {
			hang = igt_allow_hang(display.drm_fd, 0, 0);
		}

		igt_describe("Hang test on pipe with oldfb and extended pageflip modeset.");
		igt_subtest_f("extended-pageflip-modeset-hang-oldfb-pipe-%s",
			      kmstest_pipe_name(n)) {
			test_pageflip_modeset_hang(&display, n);
		}

		igt_fixture
			igt_require(display.is_atomic);

		igt_describe("Test the results with a single hanging pageflip on pipe with oldfb.");
		igt_subtest_f("extended-pageflip-hang-oldfb-pipe-%s",
			      kmstest_pipe_name(n))
			test_hang(&display, n, false, false);

		igt_describe("Test the results with a single hanging pageflip on pipe with newfb.");
		igt_subtest_f("extended-pageflip-hang-newfb-pipe-%s",
			      kmstest_pipe_name(n))
			test_hang(&display, n, false, true);

		igt_describe("Tests modeset disable/enable with hang on pipe with oldfb.");
		igt_subtest_f("extended-modeset-hang-oldfb-pipe-%s",
			      kmstest_pipe_name(n))
			test_hang(&display, n, true, false);

		igt_describe("Tests modeset disable/enable with hang on pipe with newfb.");
		igt_subtest_f("extended-modeset-hang-newfb-pipe-%s",
			      kmstest_pipe_name(n))
			test_hang(&display, n, true, true);

		igt_describe("Tests modeset disable/enable with hang on reset pipe with oldfb.");
		igt_subtest_f("extended-modeset-hang-oldfb-with-reset-pipe-%s",
			      kmstest_pipe_name(n)) {
			igt_set_module_param_int(display.drm_fd, "force_reset_modeset_test", 1);

			test_hang(&display, n, true, false);

			igt_set_module_param_int(display.drm_fd, "force_reset_modeset_test", 0);
		}

		igt_describe("Tests modeset disable/enable with hang on reset pipe with newfb.");
		igt_subtest_f("extended-modeset-hang-newfb-with-reset-pipe-%s",
			      kmstest_pipe_name(n)) {
			igt_set_module_param_int(display.drm_fd, "force_reset_modeset_test", 1);

			test_hang(&display, n, true, true);

			igt_set_module_param_int(display.drm_fd, "force_reset_modeset_test", 0);
		}

		igt_fixture {
			igt_disallow_hang(display.drm_fd, hang);
		}
	}

	igt_fixture {
		igt_display_fini(&display);
	}
}
