// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Compositor target for Phone HMD.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_internals.h"
#include <vulkan/vulkan_core.h>



#define PHONE_TARGET_NUM_IMAGES 4

struct phone_target
{
	struct comp_target base;

	// Images to render into
	VkImage images[PHONE_TARGET_NUM_IMAGES];
	VkDeviceMemory mem[PHONE_TARGET_NUM_IMAGES];
	VkImageView unorm_views[PHONE_TARGET_NUM_IMAGES];
	VkImageView srgb_views[PHONE_TARGET_NUM_IMAGES];

	// Exposed images
	struct comp_target_image image_views[PHONE_TARGET_NUM_IMAGES];

	// For cycling through images
	int32_t current_index;
	int32_t last_index;

	struct u_pacing_compositor *pacing;

	// Vulkan state
	bool vulkan_initialized;
	bool images_allocated;
};


// Free all images owned by the target
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

	pt->images_allocated = false;
}

// Allocate images for the target if not already allocated
static VkResult
allocate_images(struct phone_target *pt, struct vk_bundle *vk, VkExtent2D extent, VkImageUsageFlags usage)
{
	VkResult ret;

	if (pt->images_allocated) {
		return VK_SUCCESS;
	}

	const VkImageSubresourceRange subresource_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = VK_REMAINING_MIP_LEVELS,
	    .baseArrayLayer = 0,
	    .layerCount = VK_REMAINING_ARRAY_LAYERS,
	};

	const VkImageUsageFlags unorm_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	const VkImageUsageFlags srgb_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	for (uint32_t i = 0; i < PHONE_TARGET_NUM_IMAGES; i++) {
		ret = vk_create_image_mutable_rgba(vk, extent, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &pt->mem[i],
		                                   &pt->images[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("phone_target: failed to create image %u", i);
			break;
		}

		ret = vk_create_view_usage(vk, pt->images[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
		                           unorm_usage, subresource_range, &pt->unorm_views[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("phone_target: failed to create unorm view %u", i);
			break;
		}

		ret = vk_create_view_usage(vk, pt->images[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_SRGB,
		                           srgb_usage, subresource_range, &pt->srgb_views[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("phone_target: failed to create srgb view %u", i);
			break;
		}
	}

	if (ret != VK_SUCCESS) {
		free_images(pt, vk);
		return ret;
	}

	pt->images_allocated = true;
	return VK_SUCCESS;
}

// Initialization before vulkan is loaded, no-op
bool
target_init_pre_vulkan(struct comp_target *ct)
{
	return true;
}

// Initialization after vulkan is loaded, sets vulkan_initialized flag
bool
target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	struct phone_target *pt = (struct phone_target *)ct;
	pt->vulkan_initialized = true;
	return true;
}

// This target is always ready for image creation
bool
target_check_ready(struct comp_target *ct)
{
	return true;
}

// This target is not a shared presentable image
bool
target_is_shared_presentable_image(struct comp_target *ct)
{
	return false;
}

// Create image(s) of the target
void
target_create_images(struct comp_target *ct,
                     const struct comp_target_create_images_info *create_info,
                     struct vk_bundle_queue *present_queue)
{
	struct phone_target *pt = (struct phone_target *)ct;
	struct vk_bundle *vk = &pt->base.c->base.vk;

	// Do not start until vulkan is initialized
	if (!pt->vulkan_initialized) {
		U_LOG_E("phone_target: vulkan not initialized");
		return;
	}

	// Search for best format to use, prefer RGB over BGR formats
	VkFormat found = VK_FORMAT_UNDEFINED;
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		VkFormat format = create_info->formats[i];
		if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM) {
			found = format;
		}
		if (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM) {
			found = format;
			break;
		}
	}

	// No valid format found
	if (found == VK_FORMAT_UNDEFINED) {
		U_LOG_E("phone_target: no valid format found");
		return;
	}

	// Try to allocate images for the target
	if (allocate_images(pt, vk, create_info->extent, create_info->image_usage) != VK_SUCCESS) {
		U_LOG_E("phone_target: failed to allocate images");
		return;
	}

	// Expose Vulkan images to the compositor
	for (uint32_t i = 0; i < PHONE_TARGET_NUM_IMAGES; i++) {
		pt->image_views[i].handle = pt->images[i];
		if (found == VK_FORMAT_R8G8B8A8_UNORM || found == VK_FORMAT_B8G8R8A8_UNORM) {
			pt->image_views[i].view = pt->unorm_views[i];
		}
		if (found == VK_FORMAT_R8G8B8A8_SRGB || found == VK_FORMAT_B8G8R8A8_SRGB) {
			pt->image_views[i].view = pt->srgb_views[i];
		}
	}

	// Setup target images
	pt->base.image_count = PHONE_TARGET_NUM_IMAGES;
	pt->base.images = &pt->image_views[0];
	pt->base.width = create_info->extent.width;
	pt->base.height = create_info->extent.height;
	if (found == VK_FORMAT_R8G8B8A8_UNORM || found == VK_FORMAT_B8G8R8A8_UNORM) {
		pt->base.format = VK_FORMAT_R8G8B8A8_UNORM;
	}
	if (found == VK_FORMAT_R8G8B8A8_SRGB || found == VK_FORMAT_B8G8R8A8_SRGB) {
		pt->base.format = VK_FORMAT_R8G8B8A8_SRGB;
	}
	pt->base.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	struct sockaddr_in addr = ((struct phone_hmd *)ct->c->xdev)->phone_addr;
	if (addr.sin_addr.s_addr == 0) {
		U_LOG_E("phone_target: no phone address, cannot start stream");
		return;
	}

	struct xrt_frame_sink *xfs = NULL;
	if (!net_stream_create(&addr, &xfs)) {
		U_LOG_E("phone_target: failed to create network stream");
		return;
	}
	if (!stream_create(vk, create_info->extent, xfs)) {
		U_LOG_E("phone_target: failed to start stream");
		net_stream_destroy();
	}
}

// Has target allocated images
bool
target_has_images(struct comp_target *ct)
{
	struct phone_target *pt = (struct phone_target *)ct;

	return pt->base.images != NULL;
}

// Acquire next image for rendering, cycle in image buffers
VkResult
target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct phone_target *pt = (struct phone_target *)ct;

	if (pt->current_index != -1) {
		return VK_ERROR_UNKNOWN;
	}
	int32_t index = (pt->last_index + 1) % PHONE_TARGET_NUM_IMAGES;
	pt->current_index = index;

	*out_index = index;
	return VK_SUCCESS;
}


// Present the image at index to the screen
VkResult
target_present(struct comp_target *ct,
               struct vk_bundle_queue *present_queue,
               uint32_t index,
               uint64_t timeline_semaphore_value,
               int64_t desired_present_time_ns,
               int64_t present_slop_ns)
{

	struct phone_target *pt = (struct phone_target *)ct;

	if (pt->current_index != (int32_t)index) {
		return VK_ERROR_UNKNOWN;
	}

	pt->last_index = index;
	pt->current_index = -1;

	stream_frame(&pt->base.images[index], &pt->base.c->frame.rendering);

	return VK_SUCCESS;
}

// Target doesn't support wait_for_present, so always return error
VkResult
target_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Nothing to flush
void
target_flush(struct comp_target *ct)
{}

// Predict when the next frame should be started
void
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

	u_pc_predict(pt->pacing, now_ns, &frame_id, &wake_up_time_ns, &desired_present_time_ns, &present_slop_ns,
	             &predicted_display_time_ns, &predicted_display_period_ns, &min_display_period_ns);

	*out_frame_id = frame_id;
	*out_wake_up_time_ns = wake_up_time_ns;
	*out_desired_present_time_ns = desired_present_time_ns;
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_present_slop_ns = present_slop_ns;
}

// The compositor tells the target a timing information about a single timing point on the frames lifecycle
void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{
	struct phone_target *pt = (struct phone_target *)ct;

	switch (point) {
	case COMP_TARGET_TIMING_POINT_WAKE_UP:
		u_pc_mark_point(pt->pacing, U_TIMING_POINT_WAKE_UP, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_BEGIN:
		u_pc_mark_point(pt->pacing, U_TIMING_POINT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
		u_pc_mark_point(pt->pacing, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_END:
		u_pc_mark_point(pt->pacing, U_TIMING_POINT_SUBMIT_END, frame_id, when_ns);
		break;
	default: U_LOG_E("phone_target: unknown timing point %d", point);
	}
}

// Update timing information for this target, no-op
VkResult
target_update_timings(struct comp_target *ct)
{
	return VK_SUCCESS;
}

// Provide frame timing information about GPU start and stop time
void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	struct phone_target *pt = (struct phone_target *)ct;

	u_pc_info_gpu(pt->pacing, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

// The phone target doesn't use real presentation, so all queues are "supported"
VkResult
target_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *out_supported)
{
	*out_supported = VK_TRUE;
	return VK_SUCCESS;
}

// Show title (like window), no-op
void
target_set_title(struct comp_target *ct, const char *title)
{}
