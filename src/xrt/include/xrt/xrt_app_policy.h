// Copyright 2026, Beyley Cardellio.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for application policy object.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_defines.h"


#ifdef __cplusplus
extern "C" {
#endif


struct xrt_system;
struct xrt_app_system;

/*!
 * @interface xrt_app_instance
 *
 * This interface acts as a way to store per-application policy for @ref xrt_instance.
 *
 * Only a single one of these should be created per application (aka XrInstance, in OpenXR terminology).
 *
 * @sa xrt_instance
 * @sa xrt_instance_create_app_instance
 */
struct xrt_app_instance
{
	/*!
	 * @name Interface Methods
	 *
	 * All implementations of the xrt_app_instance interface must
	 * populate all these function pointers with their implementation
	 * methods. To use this interface, see the helper functions.
	 * @{
	 */

	/*!
	 * Creates an application system from the passed system.
	 *
	 * All @ref xrt_app_system instances created by this function are expected to be destroyed before the
	 * xrt_app_instance is destroyed.
	 *
	 * @note Code consuming this interface should use xrt_app_instance_create_app_system()
	 *
	 * @param      xainst    Pointer to self
	 * @param      xsys      Pointer to system to wrap.
	 * @param[out] out_xasys Return of application system, required.
	 */
	xrt_result_t (*create_app_system)(struct xrt_app_instance *xainst,
	                                  struct xrt_system *xsys,
	                                  struct xrt_app_system **out_xasys);

	/*!
	 * Destroy the application instance and its owned objects.
	 *
	 * Code consuming this interface should use xrt_app_instance_destroy().
	 *
	 * @param xainst Pointer to self
	 */
	void (*destroy)(struct xrt_app_instance *xainst);

	/*!
	 * @}
	 */
};

/*!
 * @copydoc xrt_app_instance::create_app_system
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_app_instance
 */
XRT_NONNULL_ALL static inline xrt_result_t
xrt_app_instance_create_app_system(struct xrt_app_instance *xainst,
                                   struct xrt_system *xsys,
                                   struct xrt_app_system **out_xasys)
{
	return xainst->create_app_system(xainst, xsys, out_xasys);
}

/*!
 * @copydoc xrt_app_instance::destroy
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_app_instance
 */
XRT_NONNULL_ALL static inline void
xrt_app_instance_destroy(struct xrt_app_instance **xainst_ptr)
{
	struct xrt_app_instance *xainst = *xainst_ptr;
	if (xainst == NULL) {
		return;
	}

	xainst->destroy(xainst);
	*xainst_ptr = NULL;
}


/*!
 * @interface xrt_app_system
 *
 * This interface acts as a way to store per-application policy for @ref xrt_system.
 *
 * Only a single one of these should be created per @ref xrt_system per application
 * (aka per XrInstance, in OpenXR terminology).
 *
 * @sa xrt_app_instance_create_app_system
 */
struct xrt_app_system
{
	/*!
	 * @name Interface Methods
	 *
	 * All implementations of the xrt_app_system interface must
	 * populate all these function pointers with their implementation
	 * methods. To use this interface, see the helper functions.
	 * @{
	 */

	/*!
	 * Destroy the application system and its owned objects.
	 *
	 * @note Code consuming this interface should use xrt_app_system_destroy().
	 *
	 * @param xasys Pointer to self
	 */
	void (*destroy)(struct xrt_app_system *xasys);

	/*!
	 * @}
	 */
};

/*!
 * @copydoc xrt_app_system::destroy
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_app_system
 */
XRT_NONNULL_ALL static inline void
xrt_app_system_destroy(struct xrt_app_system **xasys_ptr)
{
	struct xrt_app_system *xasys = *xasys_ptr;
	if (xasys == NULL) {
		return;
	}

	xasys->destroy(xasys);
	*xasys_ptr = NULL;
}


#ifdef __cplusplus
};
#endif
