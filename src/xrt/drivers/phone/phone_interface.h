// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Interface to Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <arpa/inet.h>

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_phone Phone HMD driver
 * @ingroup drv
 *
 * @brief Driver for a Phone HMD.
 */

/*!
 * Create a auto prober for a Phone HMD.
 *
 * @ingroup drv_phone
 */
struct xrt_auto_prober *
phone_create_auto_prober(void);

/*!
 * Create a Phone HMD.
 *
 * This is only exposed so that the prober (in one source file)
 * can call the construction function (in another)
 * @ingroup drv_phone
 */
struct xrt_device *
phone_hmd_create(struct sockaddr_in *phone_addr);

struct comp_target_factory;

/*!
 * Get the compositor target factory to use for the phone HMD.
 *
 * Returns NULL if @p xdev is not a phone HMD, in which case the compositor
 * selects a target as usual.
 *
 * @ingroup drv_phone
 */
const struct comp_target_factory *
phone_target_factory_get(struct xrt_device *xdev);

/*!
 * @dir drivers/phone
 *
 * @brief @ref drv_phone files.
 */


#ifdef __cplusplus
}
#endif
