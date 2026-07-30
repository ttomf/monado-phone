// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief "auto-prober" for Phone HMD.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "xrt/xrt_prober.h"

#include "util/u_misc.h"

#include "phone_interface.h"


/*!
 * @implements xrt_auto_prober
 */
struct phone_auto_prober
{
	struct xrt_auto_prober base;
};

//! @private @memberof phone_auto_prober
static inline struct phone_auto_prober *
phone_auto_prober(struct xrt_auto_prober *xap)
{
	return (struct phone_auto_prober *)xap;
}

//! @private @memberof phone_auto_prober
static void
phone_auto_prober_destroy(struct xrt_auto_prober *p)
{
	struct phone_auto_prober *ap = phone_auto_prober(p);

	free(ap);
}

//! @public @memberof phone_auto_prober
static int
phone_auto_prober_autoprobe(struct xrt_auto_prober *xap,
                            cJSON *attached_data,
                            bool no_hmds,
                            struct xrt_prober *xp,
                            struct xrt_device **out_xdevs)
{
	struct phone_auto_prober *ap = phone_auto_prober(xap);
	(void)ap;

	// Do not create an HMD device if we are not looking for HMDs.
	if (no_hmds) {
		return 0;
	}

	out_xdevs[0] = phone_hmd_create();
	return 1;
}

struct xrt_auto_prober *
phone_create_auto_prober(void)
{
	struct phone_auto_prober *ap = U_TYPED_CALLOC(struct phone_auto_prober);
	ap->base.name = "Phone HMD Auto-Prober";
	ap->base.destroy = phone_auto_prober_destroy;
	ap->base.lelo_dallas_autoprobe = phone_auto_prober_autoprobe;

	return &ap->base;
}
