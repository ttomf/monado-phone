// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the main logic for the constellation tracker.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#include "xrt/xrt_config_build.h"

#include "t_constellation_tracker_internal.hpp"
#include "t_constellation_tracker_dataset.hpp"

#ifdef XRT_FEATURE_RERUN
#include "constellation_tracker_rerun.hpp"
#endif

#include <string>


namespace xrt::tracking::constellation {

DEBUG_GET_ONCE_LOG_OPTION(constellation_tracker_log, "CONSTELLATION_TRACKER_LOG", U_LOGGING_WARN)
DEBUG_GET_ONCE_OPTION(constellation_tracker_data_recorder_output, "CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT", "")

// Unconditionally present to allow warning that the feature is not enabled.
DEBUG_GET_ONCE_BOOL_OPTION(constellation_tracker_enable_rerun, "CONSTELLATION_TRACKER_RERUN_ENABLE", false)
#ifdef XRT_FEATURE_RERUN
DEBUG_GET_ONCE_BOOL_OPTION(constellation_tracker_rerun_spawn, "CONSTELLATION_TRACKER_RERUN_SPAWN", true)
#endif

/*
 *
 * Helper functions
 *
 */

//! Gets the gravity vector of a pose, in the pose's local frame.
static void
get_pose_gravity_vector(xrt_pose &T_world_pose, xrt_vec3 &gravity)
{
	// Extract the gravity vector from the pose's orientation
	gravity = XRT_VEC3_UNIT_Y;

	xrt_quat T_pose_world_orientation;
	math_quat_invert(&T_world_pose.orientation, &T_pose_world_orientation);

	math_quat_rotate_vec3(&T_pose_world_orientation, &gravity, &gravity);
}

static uint32_t
num_blobs_for_device(CameraSample &sample, t_constellation_device_id_t device_id)
{
	uint32_t out_num_blobs = 0;
	for (uint32_t i = 0; i < sample.blob_count; i++) {
		t_blob &b = sample.blobs[i];
		if (b.matched_device_id == device_id) {
			out_num_blobs++;
		}
	}
	return out_num_blobs;
}

/*
 *
 * CameraSample implementations
 *
 */

std::optional<DeviceState *>
CameraSample::getDeviceState(t_constellation_device_id_t device_id)
{
	for (uint32_t i = 0; i < this->device_count; i++) {
		if (this->device_states[i].device_id == device_id) {
			return &this->device_states[i];
		}
	}

	return std::nullopt;
}

DeviceState &
CameraSample::putDeviceState(t_constellation_device_id_t device_id)
{
	assert(this->device_count < XRT_CONSTELLATION_MAX_DEVICES);

	DeviceState &new_device_state = this->device_states[this->device_count++];
	new_device_state = {
	    .device_id = device_id,
	    .Txr_world_device_prior = std::nullopt,
	    .found_pose = std::nullopt,
	    .needs_slow_processing = false,
	};

	return new_device_state;
}


CameraSample::CameraSample(t_blob_observation &blobservation, Camera *camera)
{
	// Copy the blob observation into this sample, since we need the data to be safe.
	this->source = blobservation.source;
	this->id = blobservation.id;
	this->timestamp_ns = blobservation.timestamp_ns;
	memcpy(blobs, blobservation.blobs, sizeof(t_blob) * blobservation.num_blobs);
	this->blob_count = blobservation.num_blobs;

	// Get the camera pose
	this->Txr_world_cam = camera->getWorldPose(blobservation.timestamp_ns);

	this->device_states = {};
	this->device_count = 0;

	auto mosaic = camera->mosaic.lock();
	U_ASSERT_WEAK_PTR_THROW(mosaic, "Camera's mosaic was destroyed while we still had a sample referencing it");

	this->mosaic_index = mosaic->index;
	this->camera_index = camera->index;
}

void
CameraSample::markMatchingBlobs(ConstellationTracker *ct,
                                t_constellation_tracker_led_model &led_model,
                                t_constellation_device_id_t device_id,
                                pose_metrics_blob_match_info &blob_match_info)
{
	// First clear existing blob labels for this device
	for (uint32_t i = 0; i < this->blob_count; i++) {
		t_blob &b = this->blobs[i];

		// Skip blobs which already have an ID not belonging to this device
		if (b.matched_device_id != device_id) {
			continue;
		}

		if (b.matched_device_led_id != XRT_CONSTELLATION_INVALID_LED_ID) {
			// @todo is this needed?
			// b.prev_led_id = b.led_id;
		}

		b.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
		b.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
	}

	// Iterate the visible LEDs and mark matching blobs with this device ID and LED ID
	for (int i = 0; i < blob_match_info.num_visible_leds; i++) {
		pose_metrics_visible_led_info &led_info = blob_match_info.visible_leds[i];
		t_constellation_tracker_led *led = led_info.led;

		if (led_info.matched_blob != NULL) {
			t_blob *b = led_info.matched_blob;

			b->matched_device_led_id = led->id;
			b->matched_device_id = device_id;

			CT_DEBUG(ct, "Marking LED %d/%d at %f,%f angle %f now %d (was %d)", device_id, led->id,
			         b->center.x, b->center.y, RAD_TO_DEG(acosf(led_info.facing_dot)),
			         b->matched_device_led_id, /* b->prev_led_id */ -1);
		} else {
			CT_DEBUG(ct, "No blob for device %d LED %d @ %f,%f size %f px angle %f", device_id, led->id,
			         led_info.pos_px.x, led_info.pos_px.y, 2 * led_info.led_radius_px,
			         RAD_TO_DEG(acosf(led_info.facing_dot)));
		}
	}
}

/*
 *
 * Camera implementations
 *
 */

Camera::Camera(ConstellationTracker *tracker,
               std::weak_ptr<CameraMosaic> mosaic,
               const t_constellation_tracker_camera &camera_params,
               enum u_logging_level *log_level_ptr,
               size_t index)
    : tracker(tracker), mosaic(mosaic), calibration(camera_params.calibration), index(index)
{
	this->tracker = tracker;
	this->mosaic = mosaic;
	this->calibration = camera_params.calibration;
	this->model = {
	    .width = calibration.image_size_pixels.w,
	    .height = calibration.image_size_pixels.h,
	    .calib = {},
	};
	t_camera_model_params_from_t_camera_calibration(&this->calibration, &this->model.calib);

	this->locked_data = {
	    .Txr_origin_cam = camera_params.pose_in_origin,
	    .has_concrete_pose = camera_params.has_concrete_pose,
	};

	this->slow_processing_thread_data.cs = correspondence_search_new(log_level_ptr, &this->model);
	this->fast_processing_thread_data.cs = correspondence_search_new(log_level_ptr, &this->model);

	if (!this->tracker->single_threaded) {
		if (os_thread_helper_init(&this->slow_processing_thread) < 0) {
			throw std::runtime_error("Slow processing thread failed to init");
		}
		if (os_thread_helper_init(&this->fast_processing_thread) < 0) {
			throw std::runtime_error("Fast processing thread failed to init");
		}

		if (os_thread_helper_start(&this->slow_processing_thread, constellation_tracker_camera_slow_thread,
		                           this) < 0) {
			throw std::runtime_error("Starting slow processing thread failed");
		}
		if (os_thread_helper_start(&this->fast_processing_thread, constellation_tracker_camera_fast_thread,
		                           this) < 0) {
			throw std::runtime_error("Starting fast processing thread failed");
		}
	}

	u_sink_debug_init(&this->slow_processing_thread_data.debug_sink);
	u_sink_debug_init(&this->fast_processing_thread_data.debug_sink);
}

Camera::~Camera()
{
	if (this->slow_processing_thread.initialized) {
		os_thread_helper_destroy(&this->slow_processing_thread);
	}

	if (this->fast_processing_thread.initialized) {
		os_thread_helper_destroy(&this->fast_processing_thread);
	}

	if (this->slow_processing_thread_data.cs) {
		correspondence_search_free(this->slow_processing_thread_data.cs);
		this->slow_processing_thread_data.cs = nullptr;
	}

	if (this->fast_processing_thread_data.cs) {
		correspondence_search_free(this->fast_processing_thread_data.cs);
		this->fast_processing_thread_data.cs = nullptr;
	}

	u_sink_debug_destroy(&this->slow_processing_thread_data.debug_sink);
	u_sink_debug_destroy(&this->fast_processing_thread_data.debug_sink);
}

std::optional<xrt_pose>
Camera::getWorldPose(timepoint_ns when_ns)
{
	std::shared_ptr<CameraMosaic> mosaic = this->mosaic.lock();
	U_ASSERT_WEAK_PTR_RET(
	    mosaic, "Camera's mosaic was destroyed while we still had a pointer to it, this should never happen",
	    std::nullopt);

	auto Txr_world_origin = mosaic->getTrackingOriginPose(when_ns);
	if (!Txr_world_origin.has_value()) {
		return std::nullopt;
	}

	std::unique_lock<os::Mutex> lock(this->processing_lock);

	if (!this->locked_data.has_concrete_pose) {
		return std::nullopt;
	}

	xrt_pose Txr_world_cam;
	math_pose_transform(&Txr_world_origin.value(), &this->locked_data.Txr_origin_cam, &Txr_world_cam);
	return Txr_world_cam;
}

void
Camera::deferSampleToSlowThread(CameraSample &sample)
{
	os_thread_helper_lock(&this->slow_processing_thread);
	{
		if (this->slow_processing_thread_data.sample.has_value()) {
			// Warn that we're dropping a frame.
			CT_WARN(this->tracker,
			        "Dropping slow sample %" PRIu64 " at ts %" PRIi64 ". Tracker is likely running slow.",
			        this->slow_processing_thread_data.sample->id,
			        this->slow_processing_thread_data.sample->timestamp_ns);
		}

		this->slow_processing_thread_data.sample = sample;
		os_thread_helper_signal_locked(&this->slow_processing_thread);
	}
	os_thread_helper_unlock(&this->slow_processing_thread);
}

bool
Camera::tryDevicePose(std::unique_ptr<Device> &device,
                      CameraSample &sample,
                      DeviceState &device_state,
                      xrt_pose &Tcv_cam_world,
                      std::optional<xrt_pose> &Tcv_world_device_prior,
                      xrt_pose &Tcv_world_device_candidate)
{
	xrt_pose Tcv_cam_device_candidate;
	math_pose_transform(&Tcv_cam_world, &Tcv_world_device_candidate, &Tcv_cam_device_candidate);

	pose_metrics score;
	if (Tcv_world_device_prior.has_value()) {
		xrt_pose Tcv_cam_device_prior;
		math_pose_transform(&Tcv_cam_world, &Tcv_world_device_prior.value(), &Tcv_cam_device_prior);

		pose_metrics_evaluate_pose_with_prior(&score, &Tcv_cam_device_candidate, false, &Tcv_cam_device_prior,
		                                      &device->prior_pos_error, &device->prior_rot_error, sample.blobs,
		                                      sample.blob_count, &device->params.led_model, device->id,
		                                      &this->model, NULL);
	} else {
		pose_metrics_evaluate_pose(&score, &Tcv_cam_device_candidate, sample.blobs, sample.blob_count,
		                           &device->params.led_model, device->id, &this->model, NULL);
	}


	if (POSE_HAS_FLAGS(&score, POSE_MATCH_GOOD | POSE_MATCH_LED_IDS)) {
		this->pushPose(sample,                   //
		               device_state,             //
		               device,                   //
		               score,                    //
		               Tcv_cam_device_candidate, //
		               false);                   //
		return true;
	}

	return false;
}

bool
Camera::tryDeviceBlobRecovery(std::unique_ptr<Device> &device,
                              CameraSample &sample,
                              DeviceState &device_state,
                              xrt_pose &Tcv_cam_world,
                              std::optional<xrt_pose> &Tcv_world_device_prior)
{
	auto tracker = this->tracker;

	const uint32_t needed_blobs = 4;
	uint32_t num_blobs = 0;
	for (uint32_t index = 0; index < sample.blob_count; index++) {
		t_blob &b = sample.blobs[index];
		if (b.matched_device_id == device->id) {
			num_blobs++;

			if (num_blobs >= needed_blobs) {
				break;
			}
		}
	}
	if (num_blobs < needed_blobs) {
		return false;
	}

	xrt_pose Tcv_cam_device_prior;
	if (Tcv_world_device_prior.has_value()) {
		math_pose_transform(&Tcv_cam_world, &Tcv_world_device_prior.value(), &Tcv_cam_device_prior);
	} else {
		// The RANSAC should still be able to run with an identity pose.
		Tcv_cam_device_prior = XRT_POSE_IDENTITY;
	}

	// RANSAC-PnP with the matched blobs
	xrt_pose Tcv_cam_device = Tcv_cam_device_prior;
	if (!ransac_pnp_pose(tracker->log_level, &Tcv_cam_device, sample.blobs, sample.blob_count,
	                     &device->params.led_model, device->id, &this->model, NULL, NULL)) {
		CT_DEBUG(tracker, "Camera %p RANSAC-PnP blob recovery for device %d from %u blobs failed", (void *)this,
		         device->id, sample.blob_count);
		return false;
	}

	pose_metrics score;
	// Evaluate the pose, using the prior if available
	if (Tcv_world_device_prior.has_value()) {
		pose_metrics_evaluate_pose_with_prior(&score, &Tcv_cam_device, true, &Tcv_cam_device_prior,
		                                      &device->prior_pos_error, &device->prior_rot_error, sample.blobs,
		                                      sample.blob_count, &device->params.led_model, device->id,
		                                      &this->model, NULL);
	} else {
		pose_metrics_evaluate_pose(&score, &Tcv_cam_device, sample.blobs, sample.blob_count,
		                           &device->params.led_model, device->id, &this->model, NULL);
	}

	if (POSE_HAS_FLAGS(&score, POSE_MATCH_GOOD)) {
		CT_DEBUG(tracker, "Camera %p RANSAC-PnP recovered pose for device %d from %u blobs", (void *)this,
		         device->id, sample.blob_count);
		this->pushPose(sample,         //
		               device_state,   //
		               device,         //
		               score,          //
		               Tcv_cam_device, //
		               true);          //

		return true;
	}

	return false;
}

void
Camera::processSampleSlow(CameraSample &sample)
{
	ConstellationTracker *tracker = this->tracker;

	auto &data = this->slow_processing_thread_data;

	CT_TRACE(tracker, "Starting slow processing for camera %p with %u blobs", (void *)this, sample.blob_count);

	correspondence_search_set_blobs(data.cs, sample.blobs, sample.blob_count);

	std::shared_ptr<CameraMosaic> mosaic = this->mosaic.lock();
	U_ASSERT_WEAK_PTR_RET(mosaic,
	                      "Camera mosaic was destroyed while processing a sample, this should never happen since "
	                      "the mosaic owns the camera");

	std::shared_lock lock(tracker->device_lock);

	auto Txr_world_cam = sample.Txr_world_cam;

	for (int i = 0; i < 2; i++) {
		for (std::unique_ptr<Device> &device : tracker->devices) {
			auto search_model = device->search_model;

			// Do a shallow search first go around
			correspondence_search_flags search_flags =
			    i == 0 ? CS_FLAG_SHALLOW_SEARCH : CS_FLAG_DEEP_SEARCH;

			search_flags = (correspondence_search_flags)(search_flags | CS_FLAG_STOP_FOR_STRONG_MATCH);

			auto device_state = sample.getDeviceState(device->id).value_or(nullptr);
			// If there was no device state in the sample, that means this device appeared after the
			// constellation tracker started this sample, so we need to fill out the device state here.
			if (device_state == nullptr) {
				device_state = &sample.putDeviceState(device->id);

				// we need to do a slow process for this device since it wasn't present in the fast
				// processing
				device_state->needs_slow_processing = true;
			}

			if (!device_state->needs_slow_processing) {
				continue; // we already did a fast process for this device and it succeeded, no need to
				          // do a slow one
			}

			xrt_pose Tcv_cam_device = XRT_POSE_IDENTITY;
			if (device_state->Txr_world_device_prior.has_value() && Txr_world_cam.has_value()) {
				xrt_pose Txr_cam_world;
				math_pose_invert(&Txr_world_cam.value(), &Txr_cam_world);

				xrt_pose Txr_cam_device;
				math_pose_transform(&Txr_cam_world, &device_state->Txr_world_device_prior.value(),
				                    &Txr_cam_device);

				math_pose_convert_opencv(&Txr_cam_device, &Tcv_cam_device);

				search_flags = (correspondence_search_flags)(search_flags | CS_FLAG_HAVE_POSE_PRIOR);
			}

			// Arbitrary threshold to prevent trusting a gravity vector if the device itself isn't confident
			// in it's own gravity.
			const float gravity_error_threshold_deg = 25.f;

			xrt_vec3 cv_camera_gravity_vector = {0.0, 1.0, 0.0};
			if ((search_flags & CS_FLAG_HAVE_POSE_PRIOR) != 0 &&
			    device->gravity_error_rad < DEG_TO_RAD(gravity_error_threshold_deg)) {
				// If we have a pose for the camera and we have a prior pose
				// (required by correspondence for search gravity matching)
				if (Txr_world_cam.has_value()) {
					xrt_pose Tcv_world_cam;
					math_pose_convert_opencv(&Txr_world_cam.value(), &Tcv_world_cam);

					// Acquire the camera's gravity vector under the processing lock
					get_pose_gravity_vector(Tcv_world_cam, cv_camera_gravity_vector);

					// Add in to check gravity
					search_flags =
					    (correspondence_search_flags)(search_flags | CS_FLAG_MATCH_GRAVITY);
				}
			}

			pose_metrics score;
			bool found_pose = correspondence_search_find_one_pose( //
			    data.cs,                                           //
			    search_model,                                      //
			    search_flags,                                      //
			    &Tcv_cam_device,                                   //
			    &device->prior_pos_error,                          //
			    &device->prior_rot_error,                          //
			    &cv_camera_gravity_vector,                         //
			    device->gravity_error_rad,                         //
			    &score);                                           //
			if (found_pose) {
				this->pushPose(sample,         //
				               *device_state,  //
				               device,         //
				               score,          //
				               Tcv_cam_device, //
				               false);         //

				// We found a pose for this device in this sample
				device_state->needs_slow_processing = false;
			} else {
				CT_TRACE(tracker, "Camera %p slow processing for device %d failed to find a pose",
				         (void *)this, device->id);
			}
		}
	}

	this->debugScribbleSample(sample, false);

#ifdef XRT_FEATURE_RERUN
	// If a slow sample was triggered by the fast processing thread, we always want to log the sample.
	if (tracker->rerun_stream) {
		tracker->rerun_stream->logSample(*tracker, sample);
	}
#endif
}

bool
Camera::processSampleFast(CameraSample &sample)
{
	ConstellationTracker *tracker = this->tracker;

	CT_TRACE(tracker, "Starting fast processing for camera %p with %u blobs", (void *)this, sample.blob_count);

	std::shared_ptr<CameraMosaic> mosaic = this->mosaic.lock();
	U_ASSERT_WEAK_PTR_RET(mosaic,
	                      "Camera mosaic was destroyed while processing a sample, this should never happen since "
	                      "the mosaic owns the camera",
	                      false);

	auto Txr_world_cam = sample.Txr_world_cam;
	if (!Txr_world_cam.has_value()) {
		CT_TRACE(tracker, "Camera %p has no world pose, cannot do fast processing", (void *)this);
		return false;
	}

	xrt_pose Tcv_world_cam;
	math_pose_convert_opencv(&Txr_world_cam.value(), &Tcv_world_cam);

	xrt_pose Tcv_cam_world;
	math_pose_invert(&Tcv_world_cam, &Tcv_cam_world);

	bool need_slow_search = false;
	std::shared_lock lock(tracker->device_lock);
	for (std::unique_ptr<Device> &device : tracker->devices) {
		xrt_space_relation device_predicted_relation = XRT_SPACE_RELATION_ZERO; //< AKA "the prior"

		if (device->params.tracking_source != nullptr) {
			t_constellation_tracker_tracking_source_get_tracked_pose(
			    device->params.tracking_source, sample.timestamp_ns, &device_predicted_relation);
		}

		std::optional<xrt_pose> Tcv_world_device_predicted = std::nullopt; //< AKA "the prior"
		// Whether the prior pose is actually valid
		if ((device_predicted_relation.relation_flags &
		     (XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) ==
		    (XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
			Tcv_world_device_predicted.emplace(xrt_pose{}); // initialize it to a value
			math_pose_convert_opencv(&device_predicted_relation.pose, &Tcv_world_device_predicted.value());
		}

		auto &device_state = sample.putDeviceState(device->id);
		device_state.Txr_world_device_prior = Tcv_world_device_predicted.has_value()
		                                          ? std::optional<xrt_pose>(device_predicted_relation.pose)
		                                          : std::nullopt;

		bool wipe_blob_associations = false;
		if (this->tryDeviceBlobRecovery(device, sample, device_state, Tcv_cam_world,
		                                Tcv_world_device_predicted)) {
			CT_DEBUG(tracker, "Fast processing for device %d succeeded with blob recovery", device->id);
			continue; // try the next device, we found a pose!
		} else {
			wipe_blob_associations = true;
		}

		// if we have a valid prior pose, try to use it for fast matching
		if (Tcv_world_device_predicted.has_value() &&
		    this->tryDevicePose(device, sample, device_state, Tcv_cam_world, Tcv_world_device_predicted,
		                        Tcv_world_device_predicted.value())) {
			CT_DEBUG(tracker, "Fast processing for device %d succeeded", device->id);
			continue; // try the next device, we found a pose!
		}

		// Try to get a last known pose
		bool has_last_known = false;
		xrt_pose Tcv_world_device_last_known;
		{
			std::unique_lock<os::Mutex> lock(device->data_lock);

			if (auto last_known_pose = device->locked_data.last_known_pose) {
				math_pose_convert_opencv(&last_known_pose->Txr_world_device,
				                         &Tcv_world_device_last_known);
				has_last_known = true;
			}
		}

		if (has_last_known && this->tryDevicePose(device, sample, device_state, Tcv_cam_world,
		                                          Tcv_world_device_predicted, Tcv_world_device_last_known)) {
			CT_DEBUG(tracker, "Fast processing for device %d succeeded with last known pose", device->id);
			continue; // try the next device, we found a pose!
		}

		if (wipe_blob_associations) {
			// Blob-based recovery failed, unmark all blobs for this device.
			for (uint32_t i = 0; i < sample.blob_count; i++) {
				t_blob &b = sample.blobs[i];
				if (b.matched_device_id == device->id) {
					b.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
					b.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
				}
			}
		}

		device_state.needs_slow_processing = true;

		need_slow_search = true;
	}

	this->debugScribbleSample(sample, true);

	// Only save samples on the fast processing thread, since the slow processing thread is *triggered* by the fast
	// processing thread.
	if (tracker->data_recorder) {
		tracker->data_recorder->recordSample(sample);
	}

#ifdef XRT_FEATURE_RERUN
	// We only want to log this sample to rerun if we aren't about to do a full search
	if (!need_slow_search && tracker->rerun_stream) {
		tracker->rerun_stream->logSample(*tracker, sample);
	}
#endif

	return need_slow_search;
}

void
Camera::pushPose(CameraSample &camera_sample,
                 DeviceState &device_state,
                 std::unique_ptr<Device> &device,
                 pose_metrics &score,
                 xrt_pose &Tcv_cam_device,
                 bool was_optimized)
{
	// We should never find two poses for the same device in a single frame
	assert(device_state.found_pose.has_value() == false);

	ConstellationTracker *tracker = this->tracker;

	uint32_t blobs_marked_before_update = num_blobs_for_device(camera_sample, device->id);

	// Match visible blobs to the pose we found
	pose_metrics_blob_match_info blob_match_info;
	pose_metrics_match_pose_to_blobs(&Tcv_cam_device, camera_sample.blobs, camera_sample.blob_count,
	                                 &device->params.led_model, device->id, &this->model, &blob_match_info);

	// Mark all the new blobs using the match info
	camera_sample.markMatchingBlobs(tracker, device->params.led_model, device->id, blob_match_info);

	// Only do an optimization if we haven't already optimized, or we marked new blobs.
	// This prevents us from optimizing a pose multiple times in a single frame.
	if (!was_optimized || num_blobs_for_device(camera_sample, device->id) > blobs_marked_before_update) {
		// Try to optimize the pose again, unmarking outliers
		uint32_t num_leds_out;
		uint32_t num_inliers;
		if (!ransac_pnp_pose(tracker->log_level, &Tcv_cam_device, camera_sample.blobs, camera_sample.blob_count,
		                     &device->params.led_model, device->id, &this->model, &num_leds_out,
		                     &num_inliers)) {
			CT_DEBUG(tracker,
			         "Camera %d (group %d) RANSAC-PnP refinement for device %d from %u "
			         "blobs failed",
			         0, 0, device->id, camera_sample.blob_count);
		} else {
			CT_DEBUG(tracker,
			         "Camera %d (group %d) RANSAC-PnP refinement for device %d from %u "
			         "blobs had %d LEDs with %d inliers. Produced pose %f,%f,%f,%f pos %f,%f,%f",
			         0, 0, device->id, camera_sample.blob_count, num_leds_out, num_inliers,
			         Tcv_cam_device.orientation.x, Tcv_cam_device.orientation.y,
			         Tcv_cam_device.orientation.z, Tcv_cam_device.orientation.w, Tcv_cam_device.position.x,
			         Tcv_cam_device.position.y, Tcv_cam_device.position.z);
		}

		// We need to re-evaluate the pose after optimization, since the reprojection error may have changed.
		pose_metrics_evaluate_pose(&score, &Tcv_cam_device, camera_sample.blobs, camera_sample.blob_count,
		                           &device->params.led_model, device->id, &this->model, NULL);
	}

	// Move to OpenXR space
	xrt_pose Txr_cam_device;
	math_pose_convert_opencv(&Tcv_cam_device, &Txr_cam_device);

	CT_DEBUG(tracker, "Pose: orient %f %f %f %f pos %f %f %f", Txr_cam_device.orientation.x,
	         Txr_cam_device.orientation.y, Txr_cam_device.orientation.z, Txr_cam_device.orientation.w,
	         Txr_cam_device.position.x, Txr_cam_device.position.y, Txr_cam_device.position.z);

	std::shared_ptr<CameraMosaic> mosaic = this->mosaic.lock();
	U_ASSERT_WEAK_PTR_RET(mosaic,
	                      "Camera mosaic was destroyed while processing a sample, this should never happen since "
	                      "the mosaic owns the camera");

	float average_brightness = 0.0f;
	uint32_t used_blobs = 0;
	for (uint32_t i = 0; i < camera_sample.blob_count; i++) {
		struct t_blob &b = camera_sample.blobs[i];
		if (b.matched_device_id == device->id) {
			average_brightness += b.brightness;
			used_blobs++;
		}
	}

	if (used_blobs > 0) {
		average_brightness /= used_blobs;
	} else {
		average_brightness = 1.0f;
	}

	// Mark that we found a pose
	device_state.found_pose = {
	    .Tcv_cam_device = Tcv_cam_device,
	    .average_blob_brightness = average_brightness,
	};

	auto Txr_world_cam = camera_sample.Txr_world_cam;
	if (!Txr_world_cam.has_value()) {
		// Can't do anything if we can't locate the camera in the world.
		return;
	}

	xrt_pose Txr_world_device;
	math_pose_transform(&Txr_world_cam.value(), &Txr_cam_device, &Txr_world_device);

	// Compute the metrics
	t_constellation_tracker_sample_metrics metrics = {
	    .matched_blob_count = score.matched_blobs,
	    .visible_led_count = score.visible_leds,
	    .reprojection_error = score.matched_blobs > 0 ? sqrtf(score.reprojection_error / score.matched_blobs) : 0.0,
	};

	// Push the sample to the device
	t_constellation_tracker_sample sample = {
	    .timestamp_ns = camera_sample.timestamp_ns,
	    .pose = Txr_world_device,
	    .mosaic_index = mosaic->index,
	    .camera_index = this->index,
	    .average_brightness = average_brightness, // @todo compute this
	    .metrics = metrics,
	};
	t_constellation_tracker_device_push_sample(device->device, &sample);

	{
		std::unique_lock<os::Mutex> lock(device->data_lock);

		// If we already found a pose in the future, then don't mark blobs, since the device has definitely
		// moved.
		if (!device->locked_data.last_known_pose.has_value() ||
		    device->locked_data.last_known_pose->timestamp_ns <= camera_sample.timestamp_ns) {
			// Call back to the blobwatch to update the blobs for this device. Done after pose optimization
			// since the RANSAC process will unlabel any outliers.
			auto tbo = camera_sample.toBlobObservation();
			t_blobwatch_mark_blob_device(camera_sample.source, &tbo, device->id);
		}

		device->locked_data.last_known_pose = DeviceLastPose(Txr_world_device, camera_sample.timestamp_ns);
	}

	CT_DEBUG(tracker, "Found pose for device %d", device->id);
}

/*
 *
 * CameraMosaic implementations
 *
 */

CameraMosaic::CameraMosaic(ConstellationTracker *tracker,
                           const t_constellation_tracker_camera_mosaic &mosaic_params,
                           size_t index)
    : index(index)
{
	this->tracking_origin = mosaic_params.tracking_origin;

	// NOTE: the stability of this vector is important since we're passing to C callbacks and APIs!
	this->cameras.reserve(mosaic_params.num_cameras);
}

std::optional<xrt_pose>
CameraMosaic::getTrackingOriginPose(timepoint_ns when_ns)
{
	if (this->tracking_origin) {
		xrt_space_relation relation;
		t_constellation_tracker_tracking_source_get_tracked_pose(this->tracking_origin, when_ns, &relation);

		// If the tracking source has a valid position, grab it
		if ((relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
		    (relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
			return relation.pose;
		} else {
			return std::nullopt;
		}
	}

	return std::optional<xrt_pose>(XRT_POSE_IDENTITY);
}

/*
 *
 * DeviceLastPose implementations
 *
 */

DeviceLastPose::DeviceLastPose(xrt_pose Txr_world_device, timepoint_ns timestamp_ns)
    : Txr_world_device(Txr_world_device), timestamp_ns(timestamp_ns)
{}

/*
 *
 * Device implementations
 *
 */

Device::Device(t_constellation_tracker_device_params *params,
               t_constellation_tracker_device *device,
               t_constellation_device_id_t id)
    : params(*params), device(device), id(id), data_lock(), locked_data({.last_known_pose = std::nullopt})
{
	// Copy the LED model leds into safe memory, since we want to mutate it into OpenCV space
	this->params.led_model.leds = new t_constellation_tracker_led[this->params.led_model.led_count];
	memcpy(this->params.led_model.leds, params->led_model.leds,
	       sizeof(t_constellation_tracker_led) * this->params.led_model.led_count);

	// flip all LEDs from OpenXR -> OpenCV coordinate space, since the tracker works in OpenCV space
	for (size_t i = 0; i < this->params.led_model.led_count; i++) {
		t_constellation_tracker_led &dst = this->params.led_model.leds[i];
		t_constellation_tracker_led &src = params->led_model.leds[i];

		dst = src;
		dst.position.y = -dst.position.y;
		dst.position.z = -dst.position.z;
		dst.normal.y = -dst.normal.y;
		dst.normal.z = -dst.normal.z;
	}

	this->search_model = t_constellation_search_model_new(this->id, &this->params.led_model);
}

Device::~Device()
{
	if (this->search_model) {
		t_constellation_search_model_free(this->search_model);
		this->search_model = nullptr;
	}

	if (this->params.led_model.leds) {
		delete[] this->params.led_model.leds;
		this->params.led_model.leds = nullptr;
	}
}

/*
 *
 * ConstellationTracker implementations
 *
 */

ConstellationTracker::ConstellationTracker(t_constellation_tracker_params *params)
{
	// If the deterministic flag is set, we force single-threaded processing.
	if ((params->flags & T_CONSTELLATION_TRACKER_FLAGS_DETERMINISTIC) != 0) {
		this->single_threaded = true;
	}

	this->log_level = debug_get_log_option_constellation_tracker_log();

	this->mosaics.reserve(params->num_mosaics);

	// Fill in our internal data structures based on the provided params
	for (size_t mosaic_idx = 0; mosaic_idx < params->num_mosaics; mosaic_idx++) {
		const t_constellation_tracker_camera_mosaic &mosaic_params = params->mosaics[mosaic_idx];

		std::shared_ptr<CameraMosaic> mosaic = std::make_shared<CameraMosaic>(this, mosaic_params, mosaic_idx);

		// Assert pointer stability!
		assert(mosaic->cameras.capacity() >= mosaic_params.num_cameras);

		for (size_t cam_idx = 0; cam_idx < mosaic_params.num_cameras; cam_idx++) {
			const t_constellation_tracker_camera &camera_params = mosaic_params.cameras[cam_idx];

			// This can't be in the constructor since the `shared_ptr` of the `mosaic` isn't formed
			// yet, but the camera needs to own a weak_ptr to it's mosaic.
			mosaic->cameras.push_back(
			    std::make_unique<Camera>(this, mosaic, camera_params, &this->log_level, cam_idx));
		}

		this->mosaics.push_back(mosaic);
	}

	// Fill in the blob sinks for each camera
	for (size_t i = 0; i < this->mosaics.size(); i++) {
		std::shared_ptr<CameraMosaic> &mosaic = this->mosaics[i];

		for (size_t j = 0; j < mosaic->cameras.size(); j++) {
			Camera *camera = mosaic->cameras[j].get();

			params->mosaics[i].cameras[j].blob_sink = &camera->base;
		}
	}

	this->params = *params;

	std::string data_recorder_output = debug_get_option_constellation_tracker_data_recorder_output();
	if (!data_recorder_output.empty()) {
		this->data_recorder = std::make_unique<DataRecorder>(this, data_recorder_output);
		CT_INFO(this, "Constellation tracker data recorder enabled, outputting to %s",
		        data_recorder_output.c_str());
	}

	if (debug_get_bool_option_constellation_tracker_enable_rerun()) {
#ifdef XRT_FEATURE_RERUN
		this->rerun_stream = std::make_unique<RerunContext>();
		CT_INFO(this, "Constellation tracker Rerun stream enabled, outputting to constellation_tracker.rerun");

		if (debug_get_bool_option_constellation_tracker_rerun_spawn()) {
			this->rerun_stream->stream->spawn().exit_on_failure();
		} else {
			this->rerun_stream->stream->connect_grpc().exit_on_failure();
		}
#else
		CT_ERROR(this, "Rerun stream requested but XRT_FEATURE_RERUN is not enabled");
#endif
	}

	CT_DEBUG(this, "Created constellation tracker with %zu mosaics", this->mosaics.size());
}

ConstellationTracker::~ConstellationTracker()
{
	CT_DEBUG(this, "Destroying constellation tracker");
}

void
ConstellationTracker::setupVariableTracking()
{
	u_var_add_root(this, "Constellation Tracker", true);
	u_var_add_log_level(this, &this->log_level, "Log Level");

	uint32_t mosaic_idx = 0;
	for (auto &mosaic : this->mosaics) {
		uint32_t camera_idx = 0;
		for (auto &camera : mosaic->cameras) {
			auto str =
			    "Camera " + std::to_string(camera_idx) + " (Mosaic " + std::to_string(mosaic_idx) + ")";
			u_var_add_gui_header(this, nullptr, str.c_str());

			u_var_add_sink_debug(this, &camera->fast_processing_thread_data.debug_sink,
			                     "Blob Debug Sink (Fast)");
			u_var_add_sink_debug(this, &camera->slow_processing_thread_data.debug_sink,
			                     "Blob Debug Sink (Slow)");

			u_var_add_pose(this, &camera->locked_data.Txr_origin_cam, "Camera Origin Pose");
			u_var_add_bool(this, &camera->locked_data.has_concrete_pose, "Has Concrete Pose");

			camera->scribble_settings.setupDebugTracking(this);

			camera_idx++;
		}
		mosaic_idx++;
	}
}

t_constellation_device_id_t
ConstellationTracker::addDevice(t_constellation_tracker_device_params *params, t_constellation_tracker_device *device)
{
	std::unique_lock lock(this->device_lock);

	if (this->devices.size() >= XRT_CONSTELLATION_MAX_DEVICES) {
		throw std::runtime_error("Maximum number of devices already added to constellation tracker");
	}

	t_constellation_device_id_t id = this->next_device_id++;

	this->devices.push_back(std::make_unique<Device>(params, device, id));

	CT_DEBUG(this, "Added device with ID %d to constellation tracker", id);

	if (this->data_recorder) {
		this->data_recorder->recordDeviceInfo(*this->devices.back());
	}

	return id;
}

void
ConstellationTracker::removeDevice(t_constellation_device_id_t device_id)
{
	std::unique_lock lock(this->device_lock);

	size_t index = 0;
	for (auto &device : this->devices) {
		if (device->id == device_id) {
			break;
		}
		index++;
	}

	if (index == this->devices.size()) {
		throw std::invalid_argument("The device ID is not present in the device list.");
	}

	// Remove the device
	this->devices.erase(this->devices.begin() + index);
}

}; // namespace xrt::tracking::constellation

using namespace xrt::tracking::constellation;

void *
constellation_tracker_camera_slow_thread(void *ptr)
{
	Camera *camera = (Camera *)ptr;

	os_thread_helper_lock(&camera->slow_processing_thread);
	while (os_thread_helper_is_running_locked(&camera->slow_processing_thread)) {
		os_thread_helper_wait_locked(&camera->slow_processing_thread);

		std::optional<CameraSample> maybe_sample = camera->slow_processing_thread_data.sample;
		camera->slow_processing_thread_data.sample.reset();

		os_thread_helper_unlock(&camera->slow_processing_thread);

		if (auto sample = maybe_sample) {
			camera->processSampleSlow(*sample);
		}

		os_thread_helper_lock(&camera->slow_processing_thread);
	}
	os_thread_helper_unlock(&camera->slow_processing_thread);

	return NULL;
}

void *
constellation_tracker_camera_fast_thread(void *ptr)
{
	Camera *camera = (Camera *)ptr;

	os_thread_helper_lock(&camera->fast_processing_thread);
	while (os_thread_helper_is_running_locked(&camera->fast_processing_thread)) {
		os_thread_helper_wait_locked(&camera->fast_processing_thread);

		std::optional<CameraSample> maybe_sample = camera->fast_processing_thread_data.sample;
		camera->fast_processing_thread_data.sample.reset();

		os_thread_helper_unlock(&camera->fast_processing_thread);

		if (auto sample = maybe_sample) {
			if (camera->processSampleFast(*sample)) {
				CT_TRACE(camera->tracker,
				         "Fast processing for camera %p failed, deferring to slow thread",
				         (void *)camera);
				camera->deferSampleToSlowThread(*sample);
			}
		}

		os_thread_helper_lock(&camera->fast_processing_thread);
	}
	os_thread_helper_unlock(&camera->fast_processing_thread);

	return NULL;
}

void
constellation_tracker_camera_push_blobs(t_blob_sink *tbs, t_blob_observation *tbo)
{
	Camera *camera = Camera::Get(tbs);
	ConstellationTracker *tracker = camera->tracker;

	CT_TRACE(tracker, "Received blob observation with %u blobs", tbo->num_blobs);

	if (tbo->num_blobs == 0) {
		CT_TRACE(tracker, "No blobs in observation, skipping processing");
		return;
	}

	if (tracker->single_threaded) {
		auto sample = CameraSample(*tbo, camera);

		// If we're in single-threaded mode, just process the sample immediately on the fast thread
		if (camera->processSampleFast(sample)) {
			CT_TRACE(tracker,
			         "Fast processing for camera %p failed in single-threaded mode, doing slow processing",
			         (void *)camera);

			camera->processSampleSlow(sample);
		}
	} else {
		// Send to the fast thread
		os_thread_helper_lock(&camera->fast_processing_thread);
		{
			if (camera->fast_processing_thread_data.sample.has_value()) {
				// Warn that we're dropping a frame
				CT_WARN(tracker,
				        "Dropping fast sample %" PRIu64 " at ts %" PRIi64
				        ". Tracker is likely running slow.",
				        camera->fast_processing_thread_data.sample->id,
				        camera->fast_processing_thread_data.sample->timestamp_ns);
			}

			camera->fast_processing_thread_data.sample = CameraSample(*tbo, camera);
			os_thread_helper_signal_locked(&camera->fast_processing_thread);
		}
		os_thread_helper_unlock(&camera->fast_processing_thread);
	}
}

void
constellation_tracker_camera_destroy(t_blob_sink *tbs)
{
	// do nothing, the constellation tracker will clean up the blob sinks when it is destroyed.
}

void
constellation_tracker_node_break_apart(xrt_frame_node *node)
{
	ConstellationTracker *tracker = ConstellationTracker::Get(node);

	tracker->running = false;

	// Stop all the threads
	for (auto &mosaic : tracker->mosaics) {
		for (auto &camera : mosaic->cameras) {
			if (camera->slow_processing_thread.initialized) {
				os_thread_helper_stop_and_wait(&camera->slow_processing_thread);
			}

			if (camera->fast_processing_thread.initialized) {
				os_thread_helper_stop_and_wait(&camera->fast_processing_thread);
			}
		}
	}
}

void
constellation_tracker_node_destroy(xrt_frame_node *node)
{
	ConstellationTracker *tracker = ConstellationTracker::Get(node);

	u_var_remove_root(tracker);

	delete tracker;
}

int
t_constellation_tracker_create(xrt_frame_context *xfctx,
                               t_constellation_tracker_params *params,
                               t_constellation_tracker **out_tracker)
{
	try {
		ConstellationTracker *tracker = new ConstellationTracker(params);

		// Add us to the frame context
		xrt_frame_context_add(xfctx, &tracker->node);

		*out_tracker = (t_constellation_tracker *)tracker;

		tracker->setupVariableTracking();
	} catch (const std::exception &e) {
		U_LOG_E("Failed to create constellation tracker: %s", e.what());
		return -1;
	}

	return 0;
}

int
t_constellation_tracker_add_device(t_constellation_tracker *raw_tracker,
                                   t_constellation_tracker_device_params *params,
                                   t_constellation_tracker_device *device,
                                   t_constellation_device_id_t *out_device_id)
{
	ConstellationTracker *tracker = ConstellationTracker::Get(raw_tracker);

	try {
		t_constellation_device_id_t device_id = tracker->addDevice(params, device);
		*out_device_id = device_id;
	} catch (const std::exception &e) {
		CT_ERROR(tracker, "Failed to add device to constellation tracker: %s", e.what());
		return -1;
	}

	return 0;
}

int
t_constellation_tracker_remove_device(t_constellation_tracker *raw_tracker, t_constellation_device_id_t device)
{
	ConstellationTracker *tracker = ConstellationTracker::Get(raw_tracker);

	try {
		tracker->removeDevice(device);
	} catch (const std::exception &e) {
		CT_ERROR(tracker, "Failed to remove device from constellation tracker: %s", e.what());
		return -1;
	}

	return 0;
}

xrt_tracking_origin *
t_constellation_tracker_get_tracking_origin(t_constellation_tracker *raw_tracker)
{
	ConstellationTracker *tracker = ConstellationTracker::Get(raw_tracker);

	return &tracker->tracking_origin;
}
