// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Internals for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#pragma once

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "xrt/xrt_frame.h"

#include "vk/vk_helpers.h"

#include "phone_config.h"

#include "math/m_relation_history.h"
#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_device.h"
#include "util/u_time.h"
#include "util/u_distortion.h"
#include "util/u_distortion_mesh.h"
#include "util/u_var.h"
#include "util/u_visibility_mask.h"
#include "util/u_pacing.h"
#include "util/u_debug.h"
#include "util/u_hand_tracking.h"

#include "os/os_time.h"
#include "xrt/xrt_prober.h"
#include "main/comp_compositor.h"
#include "vk/vk_image_readback_to_xf_pool.h"
#include "gstreamer/gst_pipeline.h"
#include "gstreamer/gst_sink.h"
#include "os/os_threading.h"


struct hand_packet
{
	int64_t timestamp_ns;
	int8_t flags;
	float left[63];
	float right[63];
};

struct phone_hmd
{
	struct xrt_device base;
	enum u_logging_level log_level;
	struct xrt_hmd_parts parts;
	struct u_cardboard_distortion distortion;
	struct m_relation_history *relation_hist;
	struct hand_packet *hand_packet;
	// Protects hand_packet between the network thread (writer)
	// and phone_hmd_get_hand_tracking (reader)
	struct os_mutex hand_lock;
	struct sockaddr_in phone_addr;
};

bool
phone_discover(struct sockaddr_in *addr);

bool
stream_create(struct vk_bundle *vk, VkExtent2D extent, struct xrt_frame_sink *xfs);

void
stream_frame(struct comp_target_image *image, struct comp_frame *frame);

void
stream_destroy(void);

bool
net_config_create(const struct sockaddr_in *addr);

void
net_config_destroy(void);

bool
net_stream_create(const struct sockaddr_in *addr, struct xrt_frame_sink **out_xfs);

void
net_stream_destroy(void);

bool
net_hand_create(struct hand_packet *out_packet, struct os_mutex *hand_lock);

void
net_hand_destroy(void);

bool
net_pose_create(struct m_relation_history *rh);

void
net_pose_destroy(void);
