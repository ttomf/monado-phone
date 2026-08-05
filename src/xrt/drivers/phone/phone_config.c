// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Configuration for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_config.h"


// TODO: this will load config from file

int
get_port(void)
{
	return 5500;
}

int
get_pose_port(void)
{
	return 5501;
}

char *
get_multicast_addr(void)
{
	return "239.1.1.1";
}

int *
get_panel_size(void)
{
	static int panel_size[2] = {1200, 1080};
	return panel_size;
}
