// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief "auto-prober" for Phone HMD.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include <arpa/inet.h>
#include "xrt/xrt_prober.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "phone_interface.h"
#include "phone_internals.h"

/*!
 * @implements xrt_auto_prober
 */
struct phone_auto_prober
{
	struct xrt_auto_prober base;
};

//! @private @memberof phone_auto_prober
static void
phone_auto_prober_destroy(struct xrt_auto_prober *xap)
{
	struct phone_auto_prober *ap = (struct phone_auto_prober *)xap;

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
	struct phone_auto_prober *ap = (struct phone_auto_prober *)xap;
	(void)ap;

	// Do not create an HMD device if we are not looking for HMDs.
	if (no_hmds) {
		return 0;
	}

	struct sockaddr_in phone_addr;
	if (!phone_discover(&phone_addr)) {
		return 0; // nothing found
	}

	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &phone_addr.sin_addr, ip_str, sizeof(ip_str));
	U_LOG_I("phone: found phone at %s", ip_str);

	out_xdevs[0] = phone_hmd_create(&phone_addr);
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
