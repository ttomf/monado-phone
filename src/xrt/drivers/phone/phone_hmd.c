// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Phone HMD device for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_internals.h"
#include "phone_interface.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"


// Update inputs, no-op
static xrt_result_t
phone_hmd_update_inputs(struct xrt_device *xdev)
{
	return u_device_noop_update_inputs(xdev);
}

// Get tracked pose, return from relation history
static xrt_result_t
phone_hmd_get_tracked_pose(struct xrt_device *xdev,
                           enum xrt_input_name name,
                           int64_t at_timestamp_ns,
                           struct xrt_space_relation *out_relation)
{
	struct phone_hmd *hmd = (struct phone_hmd *)(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		// Only supported is head pose
		U_LOG_XDEV_UNSUPPORTED_INPUT(&hmd->base, hmd->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	// Initialize as zero
	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;

	// Get relation from history
	enum m_relation_history_result history_result =
	    m_relation_history_get(hmd->relation_hist, at_timestamp_ns, &relation);
	if (history_result == M_RELATION_HISTORY_RESULT_INVALID) {
		U_LOG_E("phone: no poses pushed");
	}
	if (relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) {
		// Provide only normalized orientation
		math_quat_normalize(&relation.pose.orientation);
	}

	*out_relation = relation;
	return XRT_SUCCESS;
}

// Get view poses, return default
static xrt_result_t
phone_hmd_get_view_poses(struct xrt_device *xdev,
                         const struct xrt_vec3 *default_eye_relation,
                         int64_t at_timestamp_ns,
                         enum xrt_view_type view_type,
                         uint32_t view_count,
                         struct xrt_space_relation *out_head_relation,
                         struct xrt_fov *out_fovs,
                         struct xrt_pose *out_poses)
{
	return u_device_get_view_poses(xdev, default_eye_relation, at_timestamp_ns, view_type, view_count,
	                               out_head_relation, out_fovs, out_poses);
}

// Get visibility mask, return default
static xrt_result_t
phone_hmd_get_visibility_mask(struct xrt_device *xdev,
                              enum xrt_visibility_mask_type type,
                              uint32_t view_index,
                              struct xrt_visibility_mask **out_mask)
{
	u_visibility_mask_get_default(type, &xdev->hmd->distortion.fov[view_index], out_mask);
	return XRT_SUCCESS;
}

// Compute distortion from stored values
static xrt_result_t
phone_hmd_compute_distortion(struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *result)
{
	struct phone_hmd *hmd = (struct phone_hmd *)xdev;
	u_compute_distortion_cardboard(&hmd->distortion.values[view], u, v, result);
	return XRT_SUCCESS;
}

// Destroy phone hmd and free resources
static void
phone_hmd_destroy(struct xrt_device *xdev)
{
	struct phone_hmd *hmd = (struct phone_hmd *)(xdev);

	// Stop the pose receiver thread before it can write to a freed history.
	net_pose_destroy();
	net_config_destroy();
	m_relation_history_destroy(&hmd->relation_hist);
	u_device_free(&hmd->base);
}


struct xrt_device *
phone_hmd_create(struct sockaddr_in *phone_addr)
{
	// Create hmd
	struct phone_hmd *hmd =
	    U_DEVICE_ALLOCATE(struct phone_hmd, (U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE), 1, 0);

	// Initialize blend modes
	hmd->base.hmd->blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_mode_count = 1;

	// Set function pointers
	hmd->base.update_inputs = phone_hmd_update_inputs;
	hmd->base.get_tracked_pose = phone_hmd_get_tracked_pose;
	hmd->base.get_view_poses = phone_hmd_get_view_poses;
	hmd->base.get_visibility_mask = phone_hmd_get_visibility_mask;
	hmd->base.compute_distortion = phone_hmd_compute_distortion;
	hmd->base.destroy = phone_hmd_destroy;

	hmd->phone_addr = *phone_addr;

	hmd->log_level = U_LOGGING_DEBUG;
	strcpy(hmd->base.str, "Phone HMD");
	strcpy(hmd->base.serial, "Phone HMD");

	m_relation_history_create(&hmd->relation_hist);

	if (!net_pose_create(hmd->relation_hist)) {
		U_LOG_W("phone: failed to start pose receiver, tracking will not work");
	}

	if (!net_config_create(&hmd->phone_addr)) {
		U_LOG_W("phone: failed to start config receiver, config will use default values");
	}

	// Setup device properties
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking = true;

	// Set screen refresh rate to 60Hz
	hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 60.0f);

	const int screen_w = atoi(config_get("screen_w"));
	const int screen_h = atoi(config_get("screen_h"));

	hmd->base.hmd->screens[0].w_pixels = screen_w;
	hmd->base.hmd->screens[0].h_pixels = screen_h;

	// Set both eyes
	for (uint8_t eye = 0; eye < 2; ++eye) {
		hmd->base.hmd->views[eye].display.w_pixels = screen_w / 2;
		hmd->base.hmd->views[eye].display.h_pixels = screen_h;
		hmd->base.hmd->views[eye].viewport.y_pixels = 0;
		hmd->base.hmd->views[eye].viewport.w_pixels = screen_w / 2;
		hmd->base.hmd->views[eye].viewport.h_pixels = screen_h;
		hmd->base.hmd->views[eye].rot = u_device_rotation_ident;
	}
	hmd->base.hmd->views[0].viewport.x_pixels = 0;
	hmd->base.hmd->views[1].viewport.x_pixels = screen_w / 2;

	hmd->parts.view_count = 2;

	// Setup distortion model
	const double hFOV = 100 * (M_PI / 180.0);
	const double vFOV = 100 * (M_PI / 180.0);

	// Distortion information, fills in xdev->compute_distortion().
	const struct u_cardboard_distortion_arguments distortion = {
	    .distortion_k =
	        {
	            atof(config_get("k1")),
	            atof(config_get("k2")),
	            0,
	            0,
	            0,
	        },
	    .screen.w_pixels = screen_w,
	    .screen.h_pixels = screen_h,
	    .screen.w_meters = atof(config_get("screen_w_m")),
	    .screen.h_meters = atof(config_get("screen_h_m")),
	    .inter_lens_distance_meters = atof(config_get("inter_lens")),
	    .screen_to_lens_distance_meters = atof(config_get("screen_to_lens")),
	    .tray_to_lens_distance_meters = atof(config_get("tray_to_lens")),
	    .fov =
	        {
	            .angle_left = -hFOV / 2,
	            .angle_right = hFOV / 2,
	            .angle_up = vFOV / 2,
	            .angle_down = -vFOV / 2,
	        },
	    .vertical_alignment = U_CARDBOARD_VERTICAL_ALIGNMENT_CENTER,
	};
	u_distortion_cardboard_calculate(&distortion, &hmd->parts, &hmd->distortion);
	u_distortion_mesh_fill_in_compute(&hmd->base);

	// Set fovs from cardboard parameters
	hmd->base.hmd->distortion.fov[0] = distortion.fov;
	hmd->base.hmd->distortion.fov[1] = distortion.fov;

	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking = true;

	// Push initial pose to history
	struct xrt_space_relation identity = XRT_SPACE_RELATION_ZERO;
	identity.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                          XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
	uint64_t now = os_monotonic_get_ns();
	m_relation_history_push(hmd->relation_hist, &identity, now);

	U_LOG_I("phone: HMD created");

	return &hmd->base;
}
