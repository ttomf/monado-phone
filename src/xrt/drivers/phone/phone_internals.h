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

bool
phone_discover(struct sockaddr_in *out_addr);
