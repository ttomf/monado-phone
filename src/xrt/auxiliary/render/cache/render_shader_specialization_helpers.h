// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Build render shader specialization keys from render configuration.
 * @ingroup aux_render
 */

#pragma once

#include "cache/render_shader_specialization.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline struct render_distortion_spec
render_make_distortion_spec(uint32_t distortion_texel_count, VkBool32 do_timewarp, uint32_t view_count)
{
	struct render_distortion_spec spec = {
	    .distortion_texel_count = (int32_t)distortion_texel_count,
	    .do_timewarp = do_timewarp,
	    .view_count = (int32_t)view_count,
	};

	return spec;
}

#ifdef __cplusplus
}
#endif
