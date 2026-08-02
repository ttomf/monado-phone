// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Phone HMD compositor target.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"

#include "main/comp_compositor.h"

#include "phone_interface.h"
#include "phone_internals.h"


/*
 *
 * Defines.
 *
 */

//! Number of images the phone target cycles through.
#define PHONE_TARGET_NUM_IMAGES 4

/*
 *
 * Structs.
 *
 */

/*!
 * Phone HMD compositor target.
 *
 * The phone has no physical display connection, so the compositor renders
 * into images that we own.
 *
 * @implements comp_target
 */
struct phone_target
{
	struct comp_target base;

	//! Extent of the images, zero until @ref create_images.
	VkExtent2D extent;

	//! Images the compositor renders into.
	VkImage images[PHONE_TARGET_NUM_IMAGES];
	VkDeviceMemory mem[PHONE_TARGET_NUM_IMAGES];
	VkImageView unorm_views[PHONE_TARGET_NUM_IMAGES];
	VkImageView srgb_views[PHONE_TARGET_NUM_IMAGES];

	//! Exposed via @ref comp_target::images.
	struct comp_target_image image_views[PHONE_TARGET_NUM_IMAGES];

	//! Index of the image currently being rendered, -1 if none.
	int64_t current_index;

	//! Index of the last image that was presented.
	uint32_t last_index;

	//! Compositor frame pacing helper.
	struct u_pacing_compositor *upc;

	//! Number of presented frames, for log throttling.
	uint64_t present_count;

	//! Set once Vulkan resources exist.
	bool has_images;
	bool has_init_vulkan;
};


/*
 *
 * Vulkan helpers.
 *
 */

static void
free_images(struct phone_target *pt, struct vk_bundle *vk)
{
	for (uint32_t i = 0; i < PHONE_TARGET_NUM_IMAGES; i++) {
		if (pt->unorm_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, pt->unorm_views[i], NULL);
			pt->unorm_views[i] = VK_NULL_HANDLE;
		}
		if (pt->srgb_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, pt->srgb_views[i], NULL);
			pt->srgb_views[i] = VK_NULL_HANDLE;
		}
		if (pt->images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, pt->images[i], NULL);
			pt->images[i] = VK_NULL_HANDLE;
		}
		if (pt->mem[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, pt->mem[i], NULL);
			pt->mem[i] = VK_NULL_HANDLE;
		}
	}

	pt->has_images = false;
}

static VkResult
ensure_images(struct phone_target *pt, struct vk_bundle *vk, VkExtent2D extent, VkImageUsageFlags usage)
{
	VkResult ret;

	if (pt->has_images) {
		return VK_SUCCESS;
	}

	VkImageSubresourceRange subresource_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = VK_REMAINING_MIP_LEVELS,
	    .baseArrayLayer = 0,
	    .layerCount = VK_REMAINING_ARRAY_LAYERS,
	};

	// Very few cards support SRGB storage.
	const VkImageUsageFlags unorm_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	const VkImageUsageFlags srgb_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	for (uint32_t i = 0; i < PHONE_TARGET_NUM_IMAGES; i++) {
		ret = vk_create_image_mutable_rgba(vk, extent, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &pt->mem[i],
		                                   &pt->images[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("phone: failed to create image %u", i);
			free_images(pt, vk);
			return ret;
		}

		ret = vk_create_view_usage(   //
		    vk,                       //
		    pt->images[i],            //
		    VK_IMAGE_VIEW_TYPE_2D,    //
		    VK_FORMAT_R8G8B8A8_UNORM, //
		    unorm_usage,              //
		    subresource_range,        //
		    &pt->unorm_views[i]);     //
		if (ret != VK_SUCCESS) {
			U_LOG_E("phone: failed to create unorm view %u", i);
			free_images(pt, vk);
			return ret;
		}

		ret = vk_create_view_usage(  //
		    vk,                      //
		    pt->images[i],           //
		    VK_IMAGE_VIEW_TYPE_2D,   //
		    VK_FORMAT_R8G8B8A8_SRGB, //
		    srgb_usage,              //
		    subresource_range,       //
		    &pt->srgb_views[i]);     //
		if (ret != VK_SUCCESS) {
			U_LOG_E("phone: failed to create srgb view %u", i);
			free_images(pt, vk);
			return ret;
		}
	}

	pt->extent = extent;
	pt->has_images = true;

	return VK_SUCCESS;
}


/*
 *
 * Target members.
 *
 */

static bool
target_init_pre_vulkan(struct comp_target *ct)
{
	return true; // No-op
}

static bool
target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	struct phone_target *pt = (struct phone_target *)ct;
	(void)preferred_width;
	(void)preferred_height;

	// We now know Vulkan is running and we can use it.
	pt->has_init_vulkan = true;

	return true;
}

static bool
target_check_ready(struct comp_target *ct)
{
	return true; // Always ready.
}

static bool
target_is_shared_presentable_image(struct comp_target *ct)
{
	// Our images are not shared presentable images.
	return false;
}

static void
target_create_images(struct comp_target *ct,
                     const struct comp_target_create_images_info *create_info,
                     struct vk_bundle_queue *present_queue)
{
	struct phone_target *pt = (struct phone_target *)ct;
	struct vk_bundle *vk = &pt->base.c->base.vk;
	bool use_unorm = false, use_srgb = false, maybe_convert = false;

	// The phone target doesn't use the present_queue parameter, but it must not be NULL.
	assert(present_queue != NULL);
	(void)present_queue;

	// Paranoia.
	assert(pt->has_init_vulkan);

	/*
	 * Find the format we should use, since we are using scratch images to
	 * allocate the images we only support the two formats they use:
	 * VK_FORMAT_R8G8B8A8_SRGB and VK_FORMAT_R8G8B8A8_UNORM. We search for
	 * those, but also look for two other commonly used formats to figure
	 * out if the compositor wanted SRGB or UNORM.
	 */
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		VkFormat format = create_info->formats[i];

		if (!maybe_convert && format == VK_FORMAT_B8G8R8A8_UNORM) {
			use_unorm = true;
			maybe_convert = true;
			continue; // Keep going, we might get better formats.
		}
		if (!maybe_convert && format == VK_FORMAT_B8G8R8A8_SRGB) {
			use_srgb = true;
			maybe_convert = true;
			continue; // Keep going, we might get better formats.
		}

		// These two are what the scratch image allocates.
		if (format == VK_FORMAT_R8G8B8A8_UNORM) {
			use_unorm = true;
			maybe_convert = false;
			break; // Best match, stop searching.
		}
		if (format == VK_FORMAT_R8G8B8A8_SRGB) {
			use_srgb = true;
			maybe_convert = false;
			break; // Best match, stop searching.
		}
	}

	assert(use_unorm || use_srgb);

	if (ensure_images(pt, vk, create_info->extent, create_info->image_usage) != VK_SUCCESS) {
		COMP_ERROR(ct->c, "phone: failed to allocate images");
		return;
	}

	// Share the Vulkan handles of images and image views.
	for (uint32_t i = 0; i < PHONE_TARGET_NUM_IMAGES; i++) {
		pt->image_views[i].handle = pt->images[i];
		if (use_unorm) {
			pt->image_views[i].view = pt->unorm_views[i];
		}
		if (use_srgb) {
			pt->image_views[i].view = pt->srgb_views[i];
		}
	}

	// Fill in exported data.
	pt->base.image_count = PHONE_TARGET_NUM_IMAGES;
	pt->base.images = &pt->image_views[0];
	pt->base.width = create_info->extent.width;
	pt->base.height = create_info->extent.height;
	if (use_unorm) {
		pt->base.format = VK_FORMAT_R8G8B8A8_UNORM;
	}
	if (use_srgb) {
		pt->base.format = VK_FORMAT_R8G8B8A8_SRGB;
	}
	pt->base.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	// Start the video stream to the phone.
	struct sockaddr_in *addr = phone_hmd_get_addr(ct->c->xdev);
	if (addr == NULL) {
		COMP_ERROR(ct->c, "phone: no phone address, cannot start stream");
	} else if (!phone_stream_init(vk, create_info->extent, addr)) {
		COMP_ERROR(ct->c, "phone: failed to start stream");
	}
}

static bool
target_has_images(struct comp_target *ct)
{
	struct phone_target *pt = (struct phone_target *)ct;

	// Simple check.
	return pt->base.images != NULL;
}

static VkResult
target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct phone_target *pt = (struct phone_target *)ct;

	// Error checking.
	assert(pt->current_index == -1);

	uint32_t index = pt->last_index;
	if (++index >= PHONE_TARGET_NUM_IMAGES) {
		index = 0;
	}

	// For error checking.
	pt->current_index = index;

	// Return the variable.
	*out_index = index;

	return VK_SUCCESS;
}

static VkResult
target_present(struct comp_target *ct,
               struct vk_bundle_queue *present_queue,
               uint32_t index,
               uint64_t timeline_semaphore_value,
               int64_t desired_present_time_ns,
               int64_t present_slop_ns)
{
	struct phone_target *pt = (struct phone_target *)ct;
	struct comp_compositor *c = pt->base.c;

	// The phone target doesn't use the present_queue parameter, but it must not be NULL.
	assert(present_queue != NULL);
	(void)present_queue;
	(void)timeline_semaphore_value;

	assert(pt->current_index == (int64_t)index);

	pt->last_index = index;
	pt->current_index = -1;
	pt->present_count++;

	phone_stream_frame(&pt->base.images[index], &c->frame.rendering);

	return VK_SUCCESS;
}

static VkResult
target_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void
target_flush(struct comp_target *ct)
{
	// No-op
}

static void
target_calc_frame_pacing(struct comp_target *ct,
                         int64_t *out_frame_id,
                         int64_t *out_wake_up_time_ns,
                         int64_t *out_desired_present_time_ns,
                         int64_t *out_present_slop_ns,
                         int64_t *out_predicted_display_time_ns)
{
	struct phone_target *pt = (struct phone_target *)ct;

	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t predicted_display_time_ns = 0;
	int64_t predicted_display_period_ns = 0;
	int64_t min_display_period_ns = 0;
	int64_t now_ns = os_monotonic_get_ns();

	u_pc_predict(pt->upc,                      //
	             now_ns,                       //
	             &frame_id,                    //
	             &wake_up_time_ns,             //
	             &desired_present_time_ns,     //
	             &present_slop_ns,             //
	             &predicted_display_time_ns,   //
	             &predicted_display_period_ns, //
	             &min_display_period_ns);      //

	*out_frame_id = frame_id;
	*out_wake_up_time_ns = wake_up_time_ns;
	*out_desired_present_time_ns = desired_present_time_ns;
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_present_slop_ns = present_slop_ns;
}

static void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{
	struct phone_target *pt = (struct phone_target *)ct;

	switch (point) {
	case COMP_TARGET_TIMING_POINT_WAKE_UP:
		u_pc_mark_point(pt->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_BEGIN: //
		u_pc_mark_point(pt->upc, U_TIMING_POINT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
		u_pc_mark_point(pt->upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_END:
		u_pc_mark_point(pt->upc, U_TIMING_POINT_SUBMIT_END, frame_id, when_ns);
		break;
	default: assert(false);
	}
}

static VkResult
target_update_timings(struct comp_target *ct)
{
	return VK_SUCCESS; // No-op
}

static void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	struct phone_target *pt = (struct phone_target *)ct;

	u_pc_info_gpu(pt->upc, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

static VkResult
target_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *out_supported)
{
	// The phone target doesn't use real presentation, so all queues are "supported".
	(void)queue;
	*out_supported = VK_TRUE;
	return VK_SUCCESS;
}

static void
target_set_title(struct comp_target *ct, const char *title)
{
	// No-op
}

static void
target_destroy(struct comp_target *ct)
{
	struct phone_target *pt = (struct phone_target *)ct;
	struct vk_bundle *vk = &pt->base.c->base.vk;

	// Stop the video stream, it uses the Vulkan device.
	phone_stream_destroy();

	// Can only free if we have Vulkan.
	if (pt->has_init_vulkan) {
		free_images(pt, vk);
		pt->has_init_vulkan = false;
		pt->base.image_count = 0;
		pt->base.images = NULL;
		pt->base.width = 0;
		pt->base.height = 0;
		pt->base.format = VK_FORMAT_UNDEFINED;
		pt->base.final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	// Pacing is always created.
	u_pc_destroy(&pt->upc);

	// Free memory.
	free(pt);
}

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

	// Create the pacer.
	uint64_t now_ns = os_monotonic_get_ns();
	u_pc_fake_create(c->settings.nominal_frame_interval_ns, now_ns, &pt->upc);

	// For error checking.
	pt->current_index = -1;

	return &pt->base;
}


/*
 *
 * Factory
 *
 */

static bool
factory_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	// Never auto-detected, only used when forced by the caller.
	(void)ctf;
	(void)c;
	return false;
}

static bool
factory_create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = target_create(c);
	if (ct == NULL) {
		return false;
	}

	U_LOG_I("phone: created phone compositor target");

	*out_ct = ct;
	return true;
}

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

const struct comp_target_factory *
phone_target_factory_get(struct xrt_device *xdev)
{
	// Only force the phone target if the head mounted display is the phone HMD.
	if (strcmp(xdev->str, "Phone HMD") != 0) {
		return NULL;
	}

	return &phone_target_factory;
}
