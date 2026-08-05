// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Auto prober for Phone HMD.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_interface.h"
#include "phone_internals.h"


struct phone_auto_prober
{
	struct xrt_auto_prober base;
};

// Destroy autoprober
static void
phone_prober_destroy(struct xrt_auto_prober *xap)
{
	struct phone_auto_prober *ap = (struct phone_auto_prober *)xap;
	free(ap);
}

// Start autoprober
static int
phone_probe(struct xrt_auto_prober *xap,
            cJSON *attached_data,
            bool no_hmds,
            struct xrt_prober *xp,
            struct xrt_device **out_xdevs)
{
	struct phone_auto_prober *ap = (struct phone_auto_prober *)xap;
	(void)ap;

	// If we don't need hmd, don't create it
	if (no_hmds) {
		return 0;
	}

	// Start discovery
	struct sockaddr_in phone_addr;
	if (!phone_discover(&phone_addr)) {
		return 0;
	}

	// Succesfully discovered
	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &phone_addr.sin_addr, ip_str, sizeof(ip_str));
	U_LOG_I("phone: found phone at %s", ip_str);

	out_xdevs[0] = phone_hmd_create(&phone_addr);
	return 1;
}

// Create autoprober
struct xrt_auto_prober *
phone_create_auto_prober(void)
{
	struct phone_auto_prober *ap = U_TYPED_CALLOC(struct phone_auto_prober);
	ap->base.name = "Phone HMD Auto-Prober";
	ap->base.destroy = phone_prober_destroy;
	ap->base.lelo_dallas_autoprobe = phone_probe;

	return &ap->base;
}
