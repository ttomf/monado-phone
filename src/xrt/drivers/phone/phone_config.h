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

void
config_init(void);

int
config_set(const char *key, const char *value);

char *
config_get(const char *key);
