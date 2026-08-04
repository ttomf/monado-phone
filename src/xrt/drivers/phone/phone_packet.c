// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Wire protocol packet parsing for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xrt/xrt_defines.h"

#include "phone_internals.h"

//! @private
static uint32_t
phone_packet_read_u32_le(const uint8_t *buf)
{
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

//! @private
static float
phone_packet_read_f32_le(const uint8_t *buf)
{
	uint32_t bits = phone_packet_read_u32_le(buf);
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

bool
phone_packet_pose_parse(const uint8_t *buf,
                        size_t size,
                        enum phone_pose_tracking_state *out_state,
                        struct xrt_space_relation *out_rel)
{
	if (size != PHONE_POSE_PACKET_SIZE) {
		return false;
	}

	// The timestamp at [0..8] is on the phone clock and is ignored by the PC.
	*out_state = (enum phone_pose_tracking_state)phone_packet_read_u32_le(&buf[8]);

	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	rel.pose.orientation.x = phone_packet_read_f32_le(&buf[12]);
	rel.pose.orientation.y = phone_packet_read_f32_le(&buf[16]);
	rel.pose.orientation.z = phone_packet_read_f32_le(&buf[20]);
	rel.pose.orientation.w = phone_packet_read_f32_le(&buf[24]);
	rel.pose.position.x = phone_packet_read_f32_le(&buf[28]);
	rel.pose.position.y = phone_packet_read_f32_le(&buf[32]);
	rel.pose.position.z = phone_packet_read_f32_le(&buf[36]);

	*out_rel = rel;

	return true;
}
