// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Phone controller device for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_internals.h"


struct phone_controller
{
	struct xrt_device base;
	struct phone_hmd *hmd;
	enum xrt_hand hand;
};


static xrt_result_t
phone_controller_update_inputs(struct xrt_device *xdev)
{
	return u_device_noop_update_inputs(xdev);
}

static xrt_result_t
phone_controller_get_tracked_pose(struct xrt_device *xdev,
                                  enum xrt_input_name name,
                                  int64_t at_timestamp_ns,
                                  struct xrt_space_relation *out_relation)
{
	struct phone_controller *ctrl = (struct phone_controller *)xdev;

	if (name != XRT_INPUT_SIMPLE_GRIP_POSE && name != XRT_INPUT_SIMPLE_AIM_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	// Snapshot hand packet
	struct hand_packet packet;
	os_mutex_lock(&ctrl->hmd->hand_lock);
	packet = *ctrl->hmd->hand_packet;
	os_mutex_unlock(&ctrl->hmd->hand_lock);

	bool active = (ctrl->hand == XRT_HAND_LEFT) ? (packet.flags & (1 << 0)) : (packet.flags & (1 << 1));
	if (!active) {
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		out_relation->relation_flags = 0;
		return XRT_SUCCESS;
	}

	// Get head relation
	struct xrt_space_relation head = XRT_SPACE_RELATION_ZERO;
	enum m_relation_history_result result =
	    m_relation_history_get(ctrl->hmd->relation_hist, at_timestamp_ns, &head);
	if (result == M_RELATION_HISTORY_RESULT_INVALID) {
		U_LOG_E("phone controller: no poses pushed");
	}
	if (head.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) {
		math_quat_normalize(&head.pose.orientation);
	}

	const float *lm = (ctrl->hand == XRT_HAND_LEFT) ? packet.left : packet.right;

	// Wrist position (joint index 0)
	struct xrt_vec3 wrist = {
	    .x = lm[0],
	    .y = lm[1],
	    .z = lm[2],
	};

	// Transform to global space
	if (!(packet.flags & (1 << 2))) {
		math_quat_rotate_vec3(&head.pose.orientation, &wrist, &wrist);
	}
	wrist.x += head.pose.position.x;
	wrist.y += head.pose.position.y;
	wrist.z += head.pose.position.z;

	out_relation->pose.position = wrist;

	if (name == XRT_INPUT_SIMPLE_GRIP_POSE) {
		// Grip: forward = wrist -> middle metacarpal (9)
		struct xrt_vec3 middle_pos = {
		    .x = lm[3 * 9 + 0],
		    .y = lm[3 * 9 + 1],
		    .z = lm[3 * 9 + 2],
		};

		if (!(packet.flags & (1 << 2))) {
			math_quat_rotate_vec3(&head.pose.orientation, &middle_pos, &middle_pos);
		}

		struct xrt_vec3 forward = m_vec3_sub(middle_pos, wrist);
		math_vec3_normalize(&forward);

		// Right = cross(forward, up_hint)
		struct xrt_vec3 up_hint = {0.f, 1.f, 0.f};
		struct xrt_vec3 right;
		math_vec3_cross(&forward, &up_hint, &right);
		math_vec3_normalize(&right);

		struct xrt_vec3 neg_forward = {-forward.x, -forward.y, -forward.z};

		math_quat_from_plus_x_z(&right, &neg_forward, &out_relation->pose.orientation);
	} else {
		// Aim: orientation from index finger direction
		struct xrt_vec3 proximal = {
		    .x = lm[3 * 5 + 0],
		    .y = lm[3 * 5 + 1],
		    .z = lm[3 * 5 + 2],
		};
		struct xrt_vec3 tip = {
		    .x = lm[3 * 8 + 0],
		    .y = lm[3 * 8 + 1],
		    .z = lm[3 * 8 + 2],
		};

		if (!(packet.flags & (1 << 2))) {
			math_quat_rotate_vec3(&head.pose.orientation, &proximal, &proximal);
			math_quat_rotate_vec3(&head.pose.orientation, &tip, &tip);
		}

		struct xrt_vec3 forward = m_vec3_sub(tip, proximal);
		math_vec3_normalize(&forward);

		struct xrt_vec3 neg_forward = {-forward.x, -forward.y, -forward.z};

		// Right = cross(forward, up_hint), where up_hint = (0,1,0)
		struct xrt_vec3 up_hint = {0.f, 1.f, 0.f};
		struct xrt_vec3 right;
		math_vec3_cross(&forward, &up_hint, &right);
		math_vec3_normalize(&right);

		math_quat_from_plus_x_z(&right, &neg_forward, &out_relation->pose.orientation);
	}

	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	return XRT_SUCCESS;
}

static xrt_result_t
phone_controller_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	return XRT_SUCCESS;
}

static void
phone_controller_destroy(struct xrt_device *xdev)
{
	struct phone_controller *ctrl = (struct phone_controller *)xdev;
	u_device_free(&ctrl->base);
}

struct xrt_device *
phone_controller_create(struct phone_hmd *hmd, enum xrt_hand hand)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;

	struct phone_controller *ctrl = U_DEVICE_ALLOCATE(struct phone_controller, flags, 4, 0);

	u_device_populate_function_pointers(&ctrl->base, phone_controller_get_tracked_pose, phone_controller_destroy);
	ctrl->base.update_inputs = phone_controller_update_inputs;
	ctrl->base.set_output = phone_controller_set_output;

	ctrl->hmd = hmd;
	ctrl->hand = hand;

	ctrl->base.tracking_origin = hmd->base.tracking_origin;
	ctrl->base.supported.orientation_tracking = true;
	ctrl->base.supported.position_tracking = true;

	ctrl->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	ctrl->base.device_type =
	    (hand == XRT_HAND_LEFT) ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;

	const char *side = (hand == XRT_HAND_LEFT) ? "Left" : "Right";
	snprintf(ctrl->base.str, sizeof(ctrl->base.str), "Phone %s Controller", side);
	snprintf(ctrl->base.serial, sizeof(ctrl->base.serial), "Phone %s Controller", side);

	ctrl->base.inputs[0].name = XRT_INPUT_SIMPLE_GRIP_POSE;
	ctrl->base.inputs[1].name = XRT_INPUT_SIMPLE_AIM_POSE;
	ctrl->base.inputs[2].name = XRT_INPUT_SIMPLE_SELECT_CLICK;
	ctrl->base.inputs[3].name = XRT_INPUT_SIMPLE_MENU_CLICK;

	U_LOG_I("phone: %s controller created", side);

	return &ctrl->base;
}
