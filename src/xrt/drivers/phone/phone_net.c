// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Phone HMD network implementation.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "os/os_threading.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "vk/vk_cmd.h"
#include "vk/vk_cmd_pool.h"
#include "vk/vk_image_readback_to_xf_pool.h"

#include "gstreamer/gst_pipeline.h"
#include "gstreamer/gst_sink.h"

#include "phone_internals.h"

#define PHONE_MCAST_ADDR "239.1.1.1"
#define PHONE_PORT 5500
#define PHONE_DISCOVER_MSG_PHONE "MONADO_PHONE_DISCOVER_PHONE"
#define PHONE_DISCOVER_MSG_PC "MONADO_PHONE_DISCOVER_PC"

//! Size of the ring buffer of frames waiting for readback + send.
#define PHONE_STREAM_QUEUE_SIZE 4

//! Resolution the frames are scaled to before being sent to the phone.
#define PHONE_STREAM_WIDTH 1280
#define PHONE_STREAM_HEIGHT 720


/*
 *
 * Phone discovery
 *
 */

static void
sock_opt(int sock, int opt, int val)
{
	if (setsockopt(sock, SOL_SOCKET, opt, &val, sizeof(val)) < 0) {
		U_LOG_E("Failed to set socket option %d", opt);
	}
}

bool
phone_discover(struct sockaddr_in *out_addr)
{
	// Create a UDP socket for multicast discovery.
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		U_LOG_W("phone: socket() failed: %d", errno);
		return false;
	};
	sock_opt(sock, SO_REUSEADDR, 1);
	sock_opt(sock, SO_REUSEPORT, 1);

	// Don't block the prober forever if the phone never answers.
	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		U_LOG_W("phone: SO_RCVTIMEO failed: %d", errno);
	}

	// Bind the socket to the multicast address.
	struct sockaddr_in bind_addr = {0};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(PHONE_PORT);

	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		U_LOG_W("phone: bind() failed: %d", errno);
		close(sock);
		return false;
	}

	// Join the multicast group.
	struct ip_mreq mreq = {0};
	mreq.imr_multiaddr.s_addr = inet_addr(PHONE_MCAST_ADDR);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		U_LOG_W("phone: IP_ADD_MEMBERSHIP failed: %d", errno);
		close(sock);
		return false;
	}

	char recvBuffer[256];
	struct sockaddr_in from = {0};
	socklen_t from_len = sizeof(from);
	while (true) {
		U_LOG_W("phone: recvfrom() waiting...");
		ssize_t n = recvfrom(sock, recvBuffer, sizeof(recvBuffer) - 1, 0, (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				U_LOG_W("phone: recvfrom() timed out, no phone found");
				close(sock);
				return false;
			}
			U_LOG_W("phone: recvfrom() failed: %d", errno);
			continue;
		}
		recvBuffer[n] = '\0';
		U_LOG_I("phone: received: %s", recvBuffer);
		if (strcmp(recvBuffer, PHONE_DISCOVER_MSG_PHONE) == 0) {
			*out_addr = from;
			sendto(sock, PHONE_DISCOVER_MSG_PC, strlen(PHONE_DISCOVER_MSG_PC), 0, (struct sockaddr *)&from,
			       from_len);
			close(sock);
			break;
		}
	}
	return true;
}


/*
 *
 * Video stream.
 *
 */

/*!
 * One frame waiting to be read back from the GPU and sent to the phone.
 */
struct phone_stream_entry
{
	struct vk_image_readback_to_xf *wrap;
	VkFence fence;
	int64_t timestamp_ns;
	uint64_t sequence;
};

/*!
 * State of the phone video stream, one instance per process.
 */
struct phone_stream
{
	struct vk_bundle *vk;

	//! Pool of host-visible images that the GPU copies into.
	struct vk_image_readback_to_xf_pool *pool;

	//! Command pool for the readback copy, only used by the producer.
	struct vk_cmd_pool cmd_pool;

	//! Size of the source images (the phone screen).
	VkExtent2D src_extent;

	//! Intermediate optimal-tiled image the GPU scales into, then 1:1 copied to the host-visible wrap.
	VkImage scale_image;
	VkDeviceMemory scale_memory;
	VkImageLayout scale_layout;
	VkAccessFlags scale_access;

	//! GStreamer encoder + network pipeline.
	struct xrt_frame_context xfctx;
	struct gstreamer_pipeline *gp;
	struct gstreamer_sink *gs;
	struct xrt_frame_sink *xfs;

	//! Producer/consumer queue of frames pending readback + send.
	struct os_mutex mutex;
	struct os_cond cond;
	struct phone_stream_entry queue[PHONE_STREAM_QUEUE_SIZE];
	uint32_t head;
	uint32_t count;
	bool running;

	//! Worker thread that waits for the readback to finish and sends the frame.
	struct os_thread_helper thread;

	//! Frame counter, also used as the sequence number.
	uint64_t sequence;
};

static struct phone_stream *g_stream = NULL;

static void *
phone_stream_thread(void *ptr)
{
	struct phone_stream *ps = (struct phone_stream *)ptr;

	os_mutex_lock(&ps->mutex);
	while (true) {
		while (ps->count == 0 && ps->running) {
			os_cond_wait(&ps->cond, &ps->mutex);
		}

		if (!ps->running) {
			break;
		}

		struct phone_stream_entry entry = ps->queue[ps->head];
		ps->head = (ps->head + 1) % PHONE_STREAM_QUEUE_SIZE;
		ps->count--;
		os_mutex_unlock(&ps->mutex);

		struct vk_bundle *vk = ps->vk;

		// Wait for the copy to finish, then the host can read the data.
		VkResult ret = vk->vkWaitForFences(vk->device, 1, &entry.fence, VK_TRUE, UINT64_MAX);
		if (ret != VK_SUCCESS) {
			U_LOG_W("phone: vkWaitForFences: %s", vk_result_string(ret));
		}
		vk->vkDestroyFence(vk->device, entry.fence, NULL);

		struct xrt_frame *frame = &entry.wrap->base_frame;
		frame->source_timestamp = frame->timestamp = entry.timestamp_ns;
		frame->source_sequence = entry.sequence;

		xrt_sink_push_frame(ps->xfs, frame);
		xrt_frame_reference(&frame, NULL);

		os_mutex_lock(&ps->mutex);
	}

	os_mutex_unlock(&ps->mutex);

	return NULL;
}

/*!
 * Called from @ref comp_target::present, on the compositor render thread.
 *
 * Submits the copy of the just-rendered image into a host-visible buffer and
 * hands it to the worker thread for encoding + sending. Never blocks.
 */
void
phone_stream_frame(struct comp_target_image *image, struct comp_frame *frame)
{
	struct phone_stream *ps = g_stream;
	if (ps == NULL || !ps->running) {
		return;
	}

	struct vk_bundle *vk = ps->vk;

	os_mutex_lock(&ps->mutex);

	if (ps->count == PHONE_STREAM_QUEUE_SIZE) {
		// Queue is full, the worker is behind, drop this frame.
		static uint64_t dropped = 0;
		os_mutex_unlock(&ps->mutex);
		if (dropped++ % 90 == 0) {
			U_LOG_W("phone: stream queue full, dropping frame");
		}
		return;
	}

	struct vk_image_readback_to_xf *wrap = NULL;
	if (!vk_image_readback_to_xf_pool_get_unused_frame(vk, ps->pool, &wrap)) {
		os_mutex_unlock(&ps->mutex);
		return;
	}

	VkFence fence = VK_NULL_HANDLE;
	VkResult ret = vk->vkCreateFence(                                       //
	    vk->device,                                                         //
	    &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, //
	    NULL,                                                               //
	    &fence);                                                            //
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateFence: %s", vk_result_string(ret));
		struct xrt_frame *frame = &wrap->base_frame;
		xrt_frame_reference(&frame, NULL); // Return to the pool.
		os_mutex_unlock(&ps->mutex);
		return;
	}

	VkImageSubresourceRange subresource_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};

	// The compositor renders into the target images and leaves them in
	// VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, our copy takes them from there.
	// The copy is submitted to the same queue the renderer uses, so it
	// runs after the render of this frame has finished.
	vk_cmd_pool_lock(&ps->cmd_pool);

	VkCommandBuffer cmd;
	ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &ps->cmd_pool, 0, &cmd);
	if (ret != VK_SUCCESS) {
		vk_cmd_pool_unlock(&ps->cmd_pool);
		vk->vkDestroyFence(vk->device, fence, NULL);
		struct xrt_frame *frame = &wrap->base_frame;
		xrt_frame_reference(&frame, NULL); // Return to the pool.
		os_mutex_unlock(&ps->mutex);
		return;
	}

	struct xrt_size src_size = {.w = ps->src_extent.width, .h = ps->src_extent.height};
	struct xrt_size scale_size = {.w = PHONE_STREAM_WIDTH, .h = PHONE_STREAM_HEIGHT};

	// Scale the rendered image down into an optimal-tiled intermediate image.
	struct vk_cmd_image_transfer_info blit_info = {
	    .src.params.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .src.params.access_mask =
	        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	    .src.params.stage_mask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
	                             VK_PIPELINE_STAGE_TRANSFER_BIT,
	    .src.params.rect.extent = src_size,
	    .src.fm_image = {.base_array_layer = 0, .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT, .image = image->handle},

	    .dst.params.layout = ps->scale_layout,
	    .dst.params.access_mask = ps->scale_access,
	    .dst.params.stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT,
	    .dst.params.rect.extent = scale_size,
	    .dst.fm_image = {.base_array_layer = 0, .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT, .image = ps->scale_image},
	};

	vk_cmd_blit_image_locked(vk, cmd, &blit_info);

	// Copy the scaled image into the host-visible wrap.
	struct vk_cmd_image_transfer_info copy_info = {
	    .src.params.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .src.params.access_mask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .src.params.stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT,
	    .src.params.rect.extent = scale_size,
	    .src.fm_image = {.base_array_layer = 0, .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT, .image = ps->scale_image},

	    .dst.params.layout = wrap->layout,
	    .dst.params.access_mask = VK_ACCESS_HOST_READ_BIT,
	    .dst.params.stage_mask = VK_PIPELINE_STAGE_HOST_BIT,
	    .dst.params.rect.extent = scale_size,
	    .dst.fm_image = {.base_array_layer = 0, .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT, .image = wrap->image},
	};

	vk_cmd_copy_image_locked(vk, cmd, &copy_info);

	// Remember the layout the intermediate is in for next time.
	ps->scale_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ps->scale_access = VK_ACCESS_TRANSFER_READ_BIT;

	// Put the source image back into the layout the compositor expects.
	vk_cmd_image_barrier_locked(     //
	    vk,                          //
	    cmd,                         //
	    image->handle,               //
	    VK_ACCESS_TRANSFER_READ_BIT, // src_access_mask
	    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, //
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // old_image_layout
	    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,      // new_image_layout
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // src_stage_mask
	    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
	        VK_PIPELINE_STAGE_TRANSFER_BIT, //
	    subresource_range);                 // subresource_range

	// Make the copy readable by the host.
	vk_cmd_image_barrier_locked(              //
	    vk,                                   //
	    cmd,                                  //
	    wrap->image,                          //
	    VK_ACCESS_TRANSFER_WRITE_BIT,         // src_access_mask
	    VK_ACCESS_HOST_READ_BIT,              // dst_access_mask
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // old_image_layout
	    VK_IMAGE_LAYOUT_GENERAL,              // new_image_layout
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // src_stage_mask
	    VK_PIPELINE_STAGE_HOST_BIT,           // dst_stage_mask
	    subresource_range);                   // subresource_range

	// Remember the layout the wrap is in for next time.
	wrap->layout = VK_IMAGE_LAYOUT_GENERAL;

	VkResult submit_ret = vk->vkEndCommandBuffer(cmd);
	if (submit_ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkEndCommandBuffer: %s", vk_result_string(submit_ret));
	}

	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	submit_ret = vk_cmd_submit_locked(vk, ps->cmd_pool.queue, 1, &submit_info, fence);
	if (submit_ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_submit_locked: %s", vk_result_string(submit_ret));
	}

	vk_cmd_pool_unlock(&ps->cmd_pool);

	// Hand the frame to the worker thread.
	uint32_t tail = (ps->head + ps->count) % PHONE_STREAM_QUEUE_SIZE;
	ps->queue[tail].wrap = wrap;
	ps->queue[tail].fence = fence;
	ps->queue[tail].timestamp_ns = frame->predicted_display_time_ns;
	ps->queue[tail].sequence = ps->sequence++;
	ps->count++;

	os_cond_signal(&ps->cond);
	os_mutex_unlock(&ps->mutex);
}

bool
phone_stream_init(struct vk_bundle *vk, VkExtent2D extent, const struct sockaddr_in *addr)
{
	// Already running, a recreate of the images does not restart the stream.
	if (g_stream != NULL) {
		return true;
	}

	struct phone_stream *ps = U_TYPED_CALLOC(struct phone_stream);
	ps->vk = vk;

	ps->src_extent = extent;

	// Host-visible pool that the GPU copies the rendered images into.
	VkExtent2D stream_extent = {.width = PHONE_STREAM_WIDTH, .height = PHONE_STREAM_HEIGHT};
	vk_image_readback_to_xf_pool_create( //
	    vk,                              //
	    stream_extent,                   //
	    &ps->pool,                       //
	    XRT_FORMAT_R8G8B8A8,             //
	    VK_FORMAT_R8G8B8A8_UNORM);       //

	// Intermediate optimal-tiled image for the GPU-side downscale.
	VkResult res = vk_create_image_advanced(                                                    //
	    vk,                                                                                     //
	    (VkExtent3D){.width = stream_extent.width, .height = stream_extent.height, .depth = 1}, //
	    VK_FORMAT_R8G8B8A8_UNORM,                                                               //
	    VK_IMAGE_TILING_OPTIMAL,                                                                //
	    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,                      //
	    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,                                                    //
	    &ps->scale_memory,                                                                      //
	    &ps->scale_image);                                                                      //
	if (res != VK_SUCCESS) {
		U_LOG_E("phone: scale image create failed: %s", vk_result_string(res));
		vk_image_readback_to_xf_pool_destroy(vk, &ps->pool);
		free(ps);
		return false;
	}
	ps->scale_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	ps->scale_access = 0;

	VkResult ret =
	    vk_cmd_pool_init_for_queue(vk, &ps->cmd_pool, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vk->graphics_queue);
	if (ret != VK_SUCCESS) {
		U_LOG_E("phone: vk_cmd_pool_init_for_queue: %s", vk_result_string(ret));
		vk_image_readback_to_xf_pool_destroy(vk, &ps->pool);
		free(ps);
		return false;
	}

	// Build the encode + network pipeline.
	uint16_t port = PHONE_PORT;

	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));

	char pipeline[1024];
	snprintf(pipeline, sizeof(pipeline),
	         "appsrc name=xr_src format=time is-live=true do-timestamp=true ! "
	         "videoconvert ! "
	         "videoscale ! "
	         "video/x-raw,format=I420,width=%u,height=%u ! "
	         "videorate ! "
	         "video/x-raw,framerate=30/1 ! "
	         "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
	         "x265enc tune=zerolatency speed-preset=ultrafast bitrate=12000 "
	         "key-int-max=10 ! "
	         "h265parse config-interval=1 ! "
	         "video/x-h265,stream-format=byte-stream,alignment=nal ! "
	         "rtph265pay config-interval=1 ! "
	         "udpsink host=%s port=%d sync=false async=false",
	         PHONE_STREAM_WIDTH, PHONE_STREAM_HEIGHT, ip_str, port);
	U_LOG_I("phone: stream pipeline: %s", pipeline);

	gstreamer_pipeline_create_from_string(&ps->xfctx, pipeline, &ps->gp);
	gstreamer_sink_create_with_pipeline_fps( //
	    ps->gp,                              //
	    stream_extent.width,                 //
	    stream_extent.height,                //
	    60,                                  //
	    1,                                   //
	    XRT_FORMAT_R8G8B8A8,                 //
	    "xr_src",                            //
	    &ps->gs,                             //
	    &ps->xfs);                           //
	gstreamer_pipeline_play(ps->gp);

	os_mutex_init(&ps->mutex);
	os_cond_init(&ps->cond);
	os_thread_helper_init(&ps->thread);

	ps->running = true;

	ret = os_thread_helper_start(&ps->thread, phone_stream_thread, ps);
	if (ret != 0) {
		U_LOG_E("phone: failed to start stream thread");
		ps->running = false;
		gstreamer_sink_send_eos(ps->gs);
		xrt_frame_context_destroy_nodes(&ps->xfctx);
		vk_cmd_pool_destroy(vk, &ps->cmd_pool);
		vk_image_readback_to_xf_pool_destroy(vk, &ps->pool);
		vk->vkDestroyImage(vk->device, ps->scale_image, NULL);
		vk->vkFreeMemory(vk->device, ps->scale_memory, NULL);
		os_mutex_destroy(&ps->mutex);
		os_cond_destroy(&ps->cond);
		free(ps);
		return false;
	}

	g_stream = ps;

	U_LOG_I("phone: stream started");

	return true;
}

void
phone_stream_destroy(void)
{
	struct phone_stream *ps = g_stream;
	if (ps == NULL) {
		return;
	}
	g_stream = NULL;

	os_mutex_lock(&ps->mutex);
	ps->running = false;
	os_cond_broadcast(&ps->cond);
	os_mutex_unlock(&ps->mutex);
	os_thread_helper_stop_and_wait(&ps->thread);

	gstreamer_sink_send_eos(ps->gs);
	xrt_frame_context_destroy_nodes(&ps->xfctx);

	vk_cmd_pool_destroy(ps->vk, &ps->cmd_pool);
	vk_image_readback_to_xf_pool_destroy(ps->vk, &ps->pool);
	ps->vk->vkDestroyImage(ps->vk->device, ps->scale_image, NULL);
	ps->vk->vkFreeMemory(ps->vk->device, ps->scale_memory, NULL);
	os_mutex_destroy(&ps->mutex);
	os_cond_destroy(&ps->cond);

	free(ps);
}
