// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Interface for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#pragma once

#include <arpa/inet.h>
#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create auto prober for the Phone HMD driver
struct xrt_auto_prober *
phone_create_auto_prober(void);

// Create Phone HMD and controller devices.
// Fills out_xdevs[0]=HMD, [1]=left controller, [2]=right controller.
// Returns number of devices created (3 on success, 0 on failure).
uint32_t
phone_hmd_create(struct sockaddr_in *phone_addr, struct xrt_device **out_xdevs);

// Get the target factory for the Phone target
const struct comp_target_factory *
phone_target_factory_get(struct xrt_device *xdev);

#ifdef __cplusplus
}
#endif
