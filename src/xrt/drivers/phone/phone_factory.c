// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Compositor target factory for Phone HMD.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_internals.h"
#include "phone_target.c"


// Destroy target and free resources
static void
target_destroy(struct comp_target *ct)
{
	struct phone_target *pt = (struct phone_target *)ct;
	struct vk_bundle *vk = &pt->base.c->base.vk;

	// Stop GStreamer first (releases all frames back to the pool),
	// then destroy the stream and pool.
	net_stream_destroy();
	stream_destroy();


	// Can only free if we have Vulkan.
	if (pt->vulkan_initialized) {
		free_images(pt, vk);
		pt->vulkan_initialized = false;
		pt->base.image_count = 0;
		pt->base.images = NULL;
		pt->base.width = 0;
		pt->base.height = 0;
		pt->base.format = VK_FORMAT_UNDEFINED;
		pt->base.final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	// Pacing is always created, destroy it
	u_pc_destroy(&pt->pacing);

	// Free memory
	free(pt);
}

// Setup target
static struct comp_target *
target_create(struct comp_compositor *c)
{
	struct phone_target *pt = U_TYPED_CALLOC(struct phone_target);

	pt->base.name = "phone";
	pt->base.init_pre_vulkan = target_init_pre_vulkan;
	pt->base.init_post_vulkan = target_init_post_vulkan;
	pt->base.check_ready = target_check_ready;
	pt->base.is_shared_presentable_image = target_is_shared_presentable_image;
	pt->base.create_images = target_create_images;
	pt->base.has_images = target_has_images;
	pt->base.acquire = target_acquire;
	pt->base.present = target_present;
	pt->base.wait_for_present = target_wait_for_present;
	pt->base.flush = target_flush;
	pt->base.calc_frame_pacing = target_calc_frame_pacing;
	pt->base.mark_timing_point = target_mark_timing_point;
	pt->base.update_timings = target_update_timings;
	pt->base.info_gpu = target_info_gpu;
	pt->base.set_title = target_set_title;
	pt->base.queue_supports_present = target_queue_supports_present;
	pt->base.destroy = target_destroy;
	pt->base.c = c;

	pt->base.wait_for_present_supported = false;

	// Create fake pacer
	u_pc_fake_create(c->settings.nominal_frame_interval_ns, os_monotonic_get_ns(), &pt->pacing);

	// Initialize current index for image cycling
	pt->current_index = -1;
	pt->last_index = 0;

	return &pt->base;
}

static bool
factory_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	// Never auto detected
	return false;
}

// Creates a new Phone HMD target for the compositor
static bool
factory_create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = target_create(c);
	if (ct == NULL) {
		return false;
	}
	U_LOG_I("phone: created compositor target");
	*out_ct = ct;
	return true;
}

// Target factory properties
static const struct comp_target_factory phone_target_factory = {
    .name = "Phone",
    .identifier = "phone",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = NULL,
    .required_instance_extension_count = 0,
    .optional_device_extensions = NULL,
    .optional_device_extension_count = 0,
    .detect = factory_detect,
    .create_target = factory_create_target,
};

// Returns target factory for Phone HMD
const struct comp_target_factory *
phone_target_factory_get(struct xrt_device *xdev)
{
	// If device is not a Phone HMD, return NULL
	if (strcmp(xdev->str, "Phone HMD") != 0) {
		return NULL;
	}

	return &phone_target_factory;
}
