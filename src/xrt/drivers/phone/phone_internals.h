// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Helper functions and declarations for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <arpa/inet.h>
#include <stdbool.h>

#include "vk/vk_helpers.h"

#include "../../../xrt/compositor/main/comp_compositor.h"

bool
phone_discover(struct sockaddr_in *out_addr);

struct sockaddr_in *
phone_hmd_get_addr(struct xrt_device *xdev);

bool
phone_stream_init(struct vk_bundle *vk, VkExtent2D extent, const struct sockaddr_in *addr);

void
phone_stream_frame(struct comp_target_image *image, struct comp_frame *frame);

void
phone_stream_destroy(void);
