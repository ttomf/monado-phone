// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Internal API and wire protocol documentation for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 *
 * The Android app mirrors the wire protocol definitions in PosePacket.kt,
 * keep them in sync.
 */

#pragma once

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xrt/xrt_frame.h"

#include "vk/vk_helpers.h"

#include "../../../xrt/compositor/main/comp_compositor.h"

struct m_relation_history;

//! Name of the phone HMD device, used to identify it to the compositor target.
#define PHONE_HMD_STR "Phone HMD"


/*
 *
 * Wire protocol
 *
 */

//! Multicast group the phone and the PC use for discovery.
#define PHONE_MCAST_ADDR "239.1.1.1"

//! Port the discovery and the video stream use, on both sides.
#define PHONE_PORT 5500

//! Port the phone sends pose packets to.
#define PHONE_POSE_PORT 5501

//! Beacon sent by the phone, the PC answers with PHONE_DISCOVER_MSG_PC.
#define PHONE_DISCOVER_MSG_PHONE "MONADO_PHONE_DISCOVER_PHONE"

//! Reply sent by the PC to the phone, tells it to start the stream.
#define PHONE_DISCOVER_MSG_PC "MONADO_PHONE_DISCOVER_PC"

//! Resolution the frames are scaled to before being sent to the phone.
#define PHONE_STREAM_WIDTH 1920
#define PHONE_STREAM_HEIGHT 1080

//! Size of a pose packet, see @ref phone_packet_pose_parse.
#define PHONE_POSE_PACKET_SIZE 40

/*!
 * TrackingState values from the Android ARCore TrackingState enum.
 */
enum phone_pose_tracking_state
{
	PHONE_POSE_TRACKING = 0,
	PHONE_POSE_PAUSED = 1,
	PHONE_POSE_STOPPED = 2,
};


/*
 *
 * Discovery
 *
 */

/*!
 * Wait for a phone beacon on the multicast group and answer it.
 *
 * Blocks until a phone is found or the receive timeout expires.
 *
 * @param out_addr Address to send the video stream to, on success.
 *
 * @return true if a phone answered.
 */
bool
phone_discover(struct sockaddr_in *out_addr);


/*
 *
 * Pose stream (phone -> PC)
 *
 */

/*!
 * Parse a 40-byte little-endian pose packet, see @ref phone_packet_pose_parse.
 *
 * Packet layout:
 *  - int64  timestamp_ns    [0..8]   phone clock, ignored by the PC
 *  - int32  tracking_state  [8..12]  @ref phone_pose_tracking_state
 *  - float  qx qy qz qw     [12..28]
 *  - float  tx ty tz        [28..40]
 *
 * @param buf Packet as received from the phone.
 * @param size Size of @p buf.
 * @param out_state Tracking state from the packet.
 * @param out_rel Pose from the packet, flags are not set.
 *
 * @return true on success, false if the packet size is wrong.
 */
bool
phone_packet_pose_parse(const uint8_t *buf,
                        size_t size,
                        enum phone_pose_tracking_state *out_state,
                        struct xrt_space_relation *out_rel);

/*!
 * Start the pose receiver thread.
 *
 * @param rh Relation history the received poses are pushed into, owned by
 * the HMD device.
 *
 * @return true on success.
 */
bool
phone_pose_receive_init(struct m_relation_history *rh);

/*!
 * Stop the pose receiver thread, if it was started.
 */
void
phone_pose_receive_destroy(void);


/*
 *
 * Video stream (PC -> phone)
 *
 */

/*!
 * Create the encode + UDP pipeline that sends frames to the phone.
 *
 * The created sink is handed to @ref phone_stream_init, which pushes
 * completed frames into it.
 *
 * @param addr Phone to send the stream to.
 * @param out_xfs Sink the host-side stream pushes frames into.
 *
 * @return true on success.
 */
bool
phone_net_stream_sink_create(const struct sockaddr_in *addr, struct xrt_frame_sink **out_xfs);

/*!
 * Tear down the encode + UDP pipeline, if it was created.
 */
void
phone_net_stream_destroy(void);

/*!
 * Create the host-side video stream: GPU readback, downscale and frame queue.
 *
 * @param vk Vulkan bundle of the compositor.
 * @param extent Size of the images the compositor renders into.
 * @param xfs Sink completed frames are pushed into, owned by phone_net.c.
 *
 * @return true on success.
 */
bool
phone_stream_init(struct vk_bundle *vk, VkExtent2D extent, struct xrt_frame_sink *xfs);

/*!
 * Called from @ref comp_target::present, on the compositor render thread.
 *
 * Submits the copy of the just-rendered image into a host-visible buffer and
 * hands it to the worker thread for encoding + sending. Never blocks.
 */
void
phone_stream_frame(struct comp_target_image *image, struct comp_frame *frame);

/*!
 * Stop the host-side video stream, if it was started.
 */
void
phone_stream_destroy(void);


/*
 *
 * HMD device
 *
 */

/*!
 * Get the address of the phone this device is paired with.
 *
 * @return NULL if @p xdev is not a phone HMD.
 */
struct sockaddr_in *
phone_hmd_get_addr(struct xrt_device *xdev);
