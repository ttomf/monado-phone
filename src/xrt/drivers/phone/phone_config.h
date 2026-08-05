// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Configuration for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#pragma once

#define PHONE_DISCOVER_MSG "MONADO_PHONE_DISCOVER_PHONE"
#define PC_DISCOVER_MSG "MONADO_PHONE_DISCOVER_PC"

#define PHONE_STREAM_WIDTH 1920
#define PHONE_STREAM_HEIGHT 1080

int
get_port(void);

int
get_pose_port(void);

char *
get_multicast_addr(void);

int *
get_panel_size(void);
