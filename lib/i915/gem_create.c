// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <errno.h>

#include "gem.h"
#include "i915_drm.h"
#include "igt_core.h"
#include "ioctl_wrappers.h"

int __gem_create(int fd, uint64_t *size, uint32_t *handle)
{
	struct drm_i915_gem_create create = {
		.size = *size,
	};
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) == 0) {
		*handle = create.handle;
		*size = create.size;
	} else {
		err = -errno;
		igt_assume(err != 0);
	}

	errno = 0;
	return err;
}

/**
 * gem_create:
 * @fd: open i915 drm file descriptor
 * @size: desired size of the buffer
 *
 * This wraps the GEM_CREATE ioctl, which allocates a new gem buffer object of
 * @size.
 *
 * Returns: The file-private handle of the created buffer object
 */
uint32_t gem_create(int fd, uint64_t size)
{
	uint32_t handle;

	igt_assert_eq(__gem_create(fd, &size, &handle), 0);

	return handle;
}
