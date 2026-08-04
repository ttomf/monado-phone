// Copyright 2023, Shawn Wallace
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief SteamVR driver context implementation and entrypoint.
 * @author Shawn Wallace <yungwallace@live.com>
 * @ingroup drv_steamvr_lh
 */

#include "xrt/xrt_config_arch.h"
#include "xrt/xrt_system.h"

#include "interfaces/context.hpp"

#include "b_body_tracker.h"
#include "b_hand_tracker.h"

#include "util/u_var.h"
#include "util/u_device.h"
#include "util/u_misc.h"
#include "util/u_device.h"
#include "util/u_device.h"
#include "util/u_system_devices.h"

#include "vive/vive_bindings.h"

#include "math/m_api.h"

#include "openvr_driver.h"

#include "vdf_parser.hpp"

#include "steamvr_lh_interface.h"
#include "device.hpp"

#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <cmath>
#include <unordered_map>
#include <string_view>
#include <filesystem>
#include <istream>

namespace {

// based on the logic at
// <https://github.com/ValveSoftware/openvr/blob/ae46a8dd0172580648c8922658a100439115d3eb/src/vrcore/pathtools_public.h#L143-L153>.
#if defined(XRT_OS_WINDOWS)
#error "steamvr_lh does not yet support windows!"
#define OVR_PLAT_EXT ".dll"

#if defined(XRT_ARCH_X86_64)
#define OVR_PLAT_SUBDIR "win64"
#elif defined(XRT_ARCH_X86)
#define OVR_PLAT_SUBDIR "win32"
#else
#error "steamvr_lh: Unknown Windows Architecture"
#endif

#elif defined(XRT_OS_OSX)
#define OVR_PLAT_EXT ".dylib"
// contains all architectures in a universal binary
#define OVR_PLAT_SUBDIR "osx32"

#elif defined(XRT_OS_LINUX)
#define OVR_PLAT_EXT ".so"

#if defined(XRT_ARCH_X86)
#define OVR_PLAT_SUBDIR "linux32"
#elif defined(XRT_OS_ANDROID) && defined(XRT_ARCH_ARM)
#define OVR_PLAT_SUBDIR "androidarm32"
#elif defined(XRT_OS_ANDROID) && defined(XRT_ARCH_ARM64)
#define OVR_PLAT_SUBDIR "androidarm64"
#elif defined(XRT_ARCH_ARM64)
#define OVR_PLAT_SUBDIR "linuxarm64"
#elif defined(XRT_ARCH_X86_64)
#define OVR_PLAT_SUBDIR "linux64"
#else
#error "steamvr_lh: Unknown Linux Architecture"
#endif

#else
#error "steamvr_lh: Unknown OS"
#endif

DEBUG_GET_ONCE_LOG_OPTION(lh_log, "LIGHTHOUSE_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_BOOL_OPTION(lh_load_slimevr, "LH_LOAD_SLIMEVR", false)
DEBUG_GET_ONCE_NUM_OPTION(lh_discover_wait_ms, "LH_DISCOVER_WAIT_MS", 3000)
DEBUG_GET_ONCE_FLOAT_OPTION(lh_stick_deadzone, "LH_STICK_DEADZONE", 0)

static constexpr size_t MAX_CONTROLLERS = 16;


struct steamvr_lh_system
{
	// System devices wrapper.
	struct xrt_system_devices base;

	//! Pointer to driver context
	std::shared_ptr<Context> ctx;
};

struct steamvr_lh_system *svrs = U_TYPED_CALLOC(struct steamvr_lh_system);


// ~/.steam/root is a symlink to where the Steam root is
const std::string STEAM_INSTALL_DIR = std::string(getenv("HOME")) + "/.steam/root";
constexpr auto STEAMVR_APPID = "250820";

// Parse libraryfolder.vdf to find where SteamVR is installed
std::string
find_steamvr_install()
{
	using namespace tyti;
	u_logging_level level = debug_get_log_option_lh_log();
	std::ifstream file(STEAM_INSTALL_DIR + "/steamapps/libraryfolders.vdf");
	if (!file.is_open()) {
		U_LOG_IFL_E(level, "Failed to open libraryfolders.vdf");
		return std::string();
	}

	vdf::basic_object<char> root;
	try {
		root = vdf::read(file);
	} catch (std::exception &ex) {
		U_LOG_IFL_E(level, "Failed to read libraryfolders.vdf: %s", ex.what());
		return std::string();
	}

	assert(root.name == "libraryfolders");
	for (auto &[_, child] : root.childs) {
		U_LOG_D("Found library folder %s", child->attribs["path"].c_str());
		std::shared_ptr<vdf::object> apps = child->childs["apps"];
		for (auto &[appid, _] : apps->attribs) {
			if (appid == STEAMVR_APPID) {
				std::string path = child->attribs["path"] + "/steamapps/common/SteamVR";
				if (std::filesystem::exists(path)) {
					return path;
				}
			}
		}
	}
	return std::string();
}

} // namespace

#define CTX_ERR(...) U_LOG_IFL_E(log_level, __VA_ARGS__)
#define CTX_WARN(...) U_LOG_IFL_W(log_level, __VA_ARGS__)
#define CTX_INFO(...) U_LOG_IFL_I(log_level, __VA_ARGS__)
#define CTX_TRACE(...) U_LOG_IFL_T(log_level, __VA_ARGS__)
#define CTX_DEBUG(...) U_LOG_IFL_D(log_level, __VA_ARGS__)

/**
 * Since only the devices will live after our get_devices function is called, we make our Context
 * a shared ptr that is owned by the devices that exist, so that it is also cleaned up by the
 * devices that exist when they are all destroyed.
 */
std::shared_ptr<Context>
Context::create(const std::string &steam_install,
                const std::string &steamvr_install,
                std::vector<vr::IServerTrackedDeviceProvider *> providers)
{
	// xrt_tracking_origin initialization
	std::shared_ptr<Context> c =
	    std::make_shared<Context>(steam_install, steamvr_install, debug_get_log_option_lh_log());
	c->providers = std::move(providers);
	std::strncpy(c->name, "SteamVR Lighthouse Tracking", XRT_TRACKING_NAME_LEN);
	c->type = XRT_TRACKING_TYPE_LIGHTHOUSE;
	c->initial_offset = XRT_POSE_IDENTITY;
	for (vr::IServerTrackedDeviceProvider *const &driver : c->providers) {
		vr::EVRInitError err = driver->Init(c.get());
		if (err != vr::VRInitError_None) {
			U_LOG_IFL_E(c->log_level, "OpenVR driver initialization failed: error %u", err);
			return nullptr;
		}
	}
	c->frame_thread = std::thread([c] {
		while (c->frame_thread_run.load()) {
			using namespace std::chrono_literals;
			// SteamVR calls `RunFrame()` approximately every 10.1ms
			const std::chrono::time_point<std::chrono::steady_clock> next =
			    std::chrono::steady_clock::now() + 10ms;
			for (vr::IServerTrackedDeviceProvider *const &provider : c->providers)
				provider->RunFrame();
			c->frame_thread_event.try_acquire_until(next);
		}
	});
	return c;
}

Context::Context(const std::string &steam_install, const std::string &steamvr_install, u_logging_level level)
    : settings(steam_install, steamvr_install, this), resources(level, steamvr_install), log_level(level),
      frame_thread_run(true), frame_thread()
{}

Context::~Context()
{
	this->frame_thread_run.store(false);
	this->frame_thread_event.release();
	if (this->frame_thread.joinable())
		this->frame_thread.join();
	for (vr::IServerTrackedDeviceProvider *const &provider : providers)
		provider->Cleanup();
}

/***** IVRDriverContext methods *****/

void *
Context::GetGenericInterface(const char *pchInterfaceVersion, vr::EVRInitError *peError)
{
	CTX_DEBUG("Requested interface %s", pchInterfaceVersion);
#define MATCH_INTERFACE(version, interface)                                                                            \
	if (std::strcmp(pchInterfaceVersion, version) == 0) {                                                          \
		return interface;                                                                                      \
	}
#define MATCH_INTERFACE_THIS(interface) MATCH_INTERFACE(interface##_Version, static_cast<interface *>(this))

	// Known interfaces
	MATCH_INTERFACE_THIS(vr::IVRServerDriverHost);
	MATCH_INTERFACE_THIS(vr::IVRDriverInput);
	// This interface is not in a public header yet, but just passing IVRDriverInput_003 seems to work.
	MATCH_INTERFACE("IVRDriverInput_004", static_cast<vr::IVRDriverInput *>(this));
	MATCH_INTERFACE_THIS(vr::IVRProperties);
	MATCH_INTERFACE_THIS(vr::IVRDriverLog);
	MATCH_INTERFACE(vr::IVRSettings_Version, &settings);
	MATCH_INTERFACE(vr::IVRResources_Version, &resources);
	MATCH_INTERFACE(vr::IVRIOBuffer_Version, &iobuf);
	MATCH_INTERFACE(vr::IVRDriverManager_Version, &man);
	MATCH_INTERFACE(vr::IVRBlockQueue_Version, &blockqueue);
	MATCH_INTERFACE(vr::IVRPaths_Version, &paths);
	// This version of the interface is not in a public header.
	// Luckily it seems to be compatible with the previous version.
	MATCH_INTERFACE("IVRPaths_002", &paths);

	// Internal interfaces
	MATCH_INTERFACE("IVRServer_XXX", &server);
	MATCH_INTERFACE("IVRServerInternal_XXX", &server);
	return nullptr;
}

vr::DriverHandle_t
Context::GetDriverHandle()
{
	return 1;
}


/***** IVRServerDriverHost methods *****/

bool
Context::setup_hmd(const char *serial, vr::ITrackedDeviceServerDriver *driver)
{
	this->hmd = new HmdDevice(DeviceBuilder{this->shared_from_this(), driver, serial, STEAM_INSTALL_DIR});
#define VERIFY(expr, msg)                                                                                              \
	if (!(expr)) {                                                                                                 \
		CTX_ERR("Activating HMD failed: %s", msg);                                                             \
		delete this->hmd;                                                                                      \
		this->hmd = nullptr;                                                                                   \
		return false;                                                                                          \
	}
	vr::EVRInitError err = driver->Activate(0);
	VERIFY(err == vr::VRInitError_None, std::to_string(err).c_str());

	auto *display = static_cast<vr::IVRDisplayComponent *>(driver->GetComponent(vr::IVRDisplayComponent_Version));
	VERIFY(display, "IVRDisplayComponent is null");
#undef VERIFY

	auto hmd_parts = std::make_unique<HmdDevice::Parts>();
	hmd_parts->base.view_count = 2;
	for (size_t idx = 0; idx < 2; ++idx) {
		vr::EVREye eye = (idx == 0) ? vr::Eye_Left : vr::Eye_Right;
		xrt_view &view = hmd_parts->base.views[idx];

		display->GetEyeOutputViewport(eye, &view.viewport.x_pixels, &view.viewport.y_pixels,
		                              &view.viewport.w_pixels, &view.viewport.h_pixels);

		view.display.w_pixels = view.viewport.w_pixels;
		view.display.h_pixels = view.viewport.h_pixels;
		view.rot = u_device_rotation_ident;
	}

	hmd_parts->base.screens[0].w_pixels =
	    hmd_parts->base.views[0].display.w_pixels + hmd_parts->base.views[1].display.w_pixels;
	hmd_parts->base.screens[0].h_pixels = hmd_parts->base.views[0].display.h_pixels;
	// nominal frame interval will be set when lighthouse gives us the display frequency
	// see HmdDevice::handle_property_write

	hmd_parts->base.blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	hmd_parts->base.blend_mode_count = 1;

	auto &distortion = hmd_parts->base.distortion;
	distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	for (size_t idx = 0; idx < 2; ++idx) {
		xrt_fov &fov = distortion.fov[idx];
		float tan_left, tan_right, tan_top, tan_bottom;
		display->GetProjectionRaw((vr::EVREye)idx, &tan_left, &tan_right, &tan_top, &tan_bottom);
		fov.angle_left = atanf(tan_left);
		fov.angle_right = atanf(tan_right);
		fov.angle_up = atanf(tan_bottom);
		fov.angle_down = atanf(tan_top);
	}

	u_var_add_root(this->hmd, "SteamVR HMD Device", true);
	u_var_add_f32(this->hmd, &this->hmd->ipd, "IPD");

	u_var_add_f32(this->hmd, &distortion.fov[0].angle_up, "View 0 FovAngleUp");
	u_var_add_f32(this->hmd, &distortion.fov[0].angle_down, "View 0 FovAngleDown");
	u_var_add_f32(this->hmd, &distortion.fov[0].angle_left, "View 0 FovAngleLeft");
	u_var_add_f32(this->hmd, &distortion.fov[0].angle_right, "View 0 FovAngleRight");

	u_var_add_f32(this->hmd, &distortion.fov[1].angle_up, "View 1 FovAngleUp");
	u_var_add_f32(this->hmd, &distortion.fov[1].angle_down, "View 1 FovAngleDown");
	u_var_add_f32(this->hmd, &distortion.fov[1].angle_left, "View 1 FovAngleLeft");
	u_var_add_f32(this->hmd, &distortion.fov[1].angle_right, "View 1 FovAngleRight");

	hmd_parts->display = display;
	hmd->set_hmd_parts(std::move(hmd_parts));

	return true;
}

bool
Context::setup_controller(const char *serial, vr::ITrackedDeviceServerDriver *driver)
{
	// Find the first available slot for a new controller
	size_t device_idx = 0;
	for (; device_idx < MAX_CONTROLLERS; ++device_idx) {
		if (!controller[device_idx])
			break;
	}

	// Check if we've exceeded the maximum number of controllers
	if (device_idx == MAX_CONTROLLERS) {
		CTX_WARN("Attempted to activate more than %zu controllers - this is unsupported", MAX_CONTROLLERS);
		return false;
	}

	// Create the new controller
	controller[device_idx] = new ControllerDevice(
	    device_idx + 1, DeviceBuilder{this->shared_from_this(), driver, serial, STEAM_INSTALL_DIR});

	vr::EVRInitError err = driver->Activate(device_idx + 1);
	if (err != vr::VRInitError_None) {
		CTX_ERR("Activating controller failed: error %u", err);
		return false;
	}

	enum xrt_device_name name = controller[device_idx]->name;
	switch (name) {
	case XRT_DEVICE_VIVE_WAND:
		controller[device_idx]->binding_profiles = vive_binding_profiles_wand;
		controller[device_idx]->binding_profile_count = vive_binding_profiles_wand_count;
		break;

		break;
	case XRT_DEVICE_INDEX_CONTROLLER:
		controller[device_idx]->binding_profiles = vive_binding_profiles_index;
		controller[device_idx]->binding_profile_count = vive_binding_profiles_index_count;
		break;
	case XRT_DEVICE_FLIPVR:
		controller[device_idx]->binding_profiles = vive_binding_profiles_flipvr;
		controller[device_idx]->binding_profile_count = vive_binding_profiles_flipvr_count;
		break;
	default: break;
	}

	return true;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool
Context::TrackedDeviceAdded(const char *pchDeviceSerialNumber,
                            vr::ETrackedDeviceClass eDeviceClass,
                            vr::ITrackedDeviceServerDriver *pDriver)
{
	std::lock_guard lk(this->devices_mut);

	if (!this->in_setup) {
		// devices appearing after setup are not added to xrt_system_devices and would be leaked on exit
		CTX_WARN("Cannot add device after setup; consider increasing LH_DISCOVER_WAIT_MS");
		return false;
	}

	CTX_INFO("New device added: %s", pchDeviceSerialNumber);
	switch (eDeviceClass) {
	case vr::TrackedDeviceClass_HMD: {
		CTX_INFO("Found lighthouse HMD: %s", pchDeviceSerialNumber);
		return setup_hmd(pchDeviceSerialNumber, pDriver);
	}
	case vr::TrackedDeviceClass_Controller: {
		CTX_INFO("Found lighthouse controller: %s", pchDeviceSerialNumber);
		return setup_controller(pchDeviceSerialNumber, pDriver);
	}
	case vr::TrackedDeviceClass_TrackingReference: {
		CTX_INFO("Found lighthouse base station: %s", pchDeviceSerialNumber);
		return false;
	}
	case vr::TrackedDeviceClass_GenericTracker: {
		CTX_INFO("Found lighthouse tracker: %s", pchDeviceSerialNumber);
		return setup_controller(pchDeviceSerialNumber, pDriver);
	}
	default: {
		CTX_WARN("Attempted to add unsupported device class: %u", eDeviceClass);
		return false;
	}
	}
}

void
Context::TrackedDevicePoseUpdated(uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize)
{
	assert(sizeof(newPose) == unPoseStructSize);

	// Check for valid device index, allowing for the HMD plus up to MAX_CONTROLLERS controllers
	if (unWhichDevice > MAX_CONTROLLERS)
		return;

	Device *dev = nullptr;

	// If unWhichDevice is 0, it refers to the HMD; otherwise, it refers to one of the controllers
	if (unWhichDevice == 0) {
		dev = static_cast<Device *>(this->hmd);
	} else {
		// unWhichDevice - 1 will give the index into the controller array
		dev = static_cast<Device *>(this->controller[unWhichDevice - 1]);
	}

	assert(dev);
	dev->update_pose(newPose);
}

void
Context::VsyncEvent(double vsyncTimeOffsetSeconds)
{}

void
Context::add_event_locked(vr::VREvent_t event)
{
	this->events.emplace_back(std::chrono::steady_clock::now(), event);
	this->frame_thread_event.try_acquire();
	this->frame_thread_event.release();
}

void
Context::VendorSpecificEvent(uint32_t unWhichDevice,
                             vr::EVREventType eventType,
                             const vr::VREvent_Data_t &eventData,
                             double eventTimeOffset)
{
	std::lock_guard lk(event_queue_mut);
	this->add_event_locked({
	    .eventType = eventType,
	    .trackedDeviceIndex = unWhichDevice,
	    .eventAgeSeconds = {},
	    .data = eventData,
	});
}

bool
Context::IsExiting()
{
	return false;
}

size_t
Context::add_haptic_event(vr::VREvent_HapticVibration_t event, const size_t old_event_handle)
{
	vr::VREvent_t e;
	e.eventType = vr::EVREventType::VREvent_Input_HapticVibration;
	e.trackedDeviceIndex = event.containerHandle - 1;
	vr::VREvent_Data_t d;
	d.hapticVibration = event;
	e.data = d;

	std::unique_lock lk(event_queue_mut);
	while (events.size() >= 120) { // avoid unbounded allocation if misbehaving apps send too many events at once
		event_popped.wait(lk);
	}
	const size_t old_event_index = old_event_handle - events_tail;
	if (old_event_index < size_t(events.size())) {
		vr::VREvent_t *const old_event = &events[old_event_index].inner;
		if (old_event->eventType == e.eventType &&
		    old_event->data.hapticVibration.containerHandle == e.data.hapticVibration.containerHandle &&
		    old_event->data.hapticVibration.componentHandle == e.data.hapticVibration.componentHandle) {
			old_event->eventType = vr::EVREventType::VREvent_None;
		}
	}
	this->add_event_locked(e);
	return events_tail + events.size() - 1;
}

bool
Context::PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent)
{
	std::lock_guard lk(event_queue_mut);
	while (!events.empty()) {
		assert(sizeof(vr::VREvent_t) == uncbVREvent);
		Event e = events.front();
		events.pop_front();
		++events_tail;
		if (e.inner.eventType == vr::EVREventType::VREvent_None) {
			continue;
		}
		event_popped.notify_all();
		*pEvent = e.inner;
		using float_sec = std::chrono::duration<float>;
		float_sec event_age = std::chrono::steady_clock::now() - e.insert_time;
		pEvent->eventAgeSeconds = event_age.count();
		return true;
	}
	return false;
}

void
Context::GetRawTrackedDevicePoses(float fPredictedSecondsFromNow,
                                  vr::TrackedDevicePose_t *pTrackedDevicePoseArray,
                                  uint32_t unTrackedDevicePoseArrayCount)
{
	// This is the bare minimum required for SlimeVR's HMD feedback to work
	if (unTrackedDevicePoseArrayCount != 10 || this->hmd == nullptr)
		return;
	const uint64_t time =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count();
	xrt_space_relation head = {};
	xrt_device_get_tracked_pose(this->hmd, XRT_INPUT_GENERIC_HEAD_POSE, time, &head);
	xrt_matrix_3x3 rot = {};
	math_matrix_3x3_from_quat(&head.pose.orientation, &rot);
	pTrackedDevicePoseArray[0].mDeviceToAbsoluteTracking = {{
	    {rot.v[0], rot.v[3], rot.v[6], head.pose.position.x},
	    {rot.v[1], rot.v[4], rot.v[7], head.pose.position.y},
	    {rot.v[2], rot.v[5], rot.v[8], head.pose.position.z},
	}};
}

void
Context::RequestRestart(const char *pchLocalizedReason,
                        const char *pchExecutableToStart,
                        const char *pchArguments,
                        const char *pchWorkingDirectory)
{}

uint32_t
Context::GetFrameTimings(vr::Compositor_FrameTiming *pTiming, uint32_t nFrames)
{
	return 0;
}

void
Context::SetDisplayEyeToHead(uint32_t unWhichDevice,
                             const vr::HmdMatrix34_t &eyeToHeadLeft,
                             const vr::HmdMatrix34_t &eyeToHeadRight)
{
	hmd->SetDisplayEyeToHead(unWhichDevice, eyeToHeadLeft, eyeToHeadRight);
}

void
Context::SetDisplayProjectionRaw(uint32_t unWhichDevice, const vr::HmdRect2_t &eyeLeft, const vr::HmdRect2_t &eyeRight)
{}

void
Context::SetRecommendedRenderTargetSize(uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight)
{}

/***** IVRDriverInput methods *****/


vr::EVRInputError
Context::create_component_common(vr::PropertyContainerHandle_t container,
                                 const char *name,
                                 vr::VRInputComponentHandle_t *pHandle)
{
	*pHandle = vr::k_ulInvalidInputComponentHandle;
	Device *device = prop_container_to_device(container);
	if (!device) {
		return vr::VRInputError_InvalidHandle;
	}
	if (xrt_input *input = device->get_input_from_name(name); input) {
		CTX_DEBUG("creating component %s for %p", name, (void *)device);
		vr::VRInputComponentHandle_t handle = new_handle();
		handle_to_input[handle] = input;
		*pHandle = handle;
	}
	return vr::VRInputError_None;
}

xrt_input *
Context::update_component_common(vr::VRInputComponentHandle_t handle,
                                 double offset,
                                 std::chrono::steady_clock::time_point now)
{
	xrt_input *input{nullptr};
	if (handle != vr::k_ulInvalidInputComponentHandle) {
		input = handle_to_input[handle];
		std::chrono::duration<double, std::chrono::seconds::period> offset_dur(offset);
		std::chrono::duration offset = (now + offset_dur).time_since_epoch();
		int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(offset).count();
		input->active = true;
		input->timestamp = timestamp;
	}
	return input;
}

vr::EVRInputError
Context::CreateBooleanComponent(vr::PropertyContainerHandle_t ulContainer,
                                const char *pchName,
                                vr::VRInputComponentHandle_t *pHandle)
{
	return create_component_common(ulContainer, pchName, pHandle);
}

vr::EVRInputError
Context::UpdateBooleanComponent(vr::VRInputComponentHandle_t ulComponent, bool bNewValue, double fTimeOffset)
{
	xrt_input *input = update_component_common(ulComponent, fTimeOffset);
	if (input) {
		input->value.boolean = bNewValue;
	}
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateScalarComponent(vr::PropertyContainerHandle_t ulContainer,
                               const char *pchName,
                               vr::VRInputComponentHandle_t *pHandle,
                               vr::EVRScalarType eType,
                               vr::EVRScalarUnits eUnits)
{
	std::string_view name{pchName};
	// Lighthouse gives thumbsticks/trackpads as x/y components,
	// we need to combine them for Monado
	auto end = name.back();
	auto second_last = name.at(name.size() - 2);
	if (second_last == '/' && (end == 'x' || end == 'y')) {
		Device *device = prop_container_to_device(ulContainer);
		if (!device) {
			return vr::VRInputError_InvalidHandle;
		}
		bool x = end == 'x';
		name.remove_suffix(2);
		std::string n(name);
		xrt_input *input = device->get_input_from_name(n);
		if (!input) {
			return vr::VRInputError_None;
		}

		// Create the component mapping if it hasn't been created yet
		Vec2Components *components =
		    vec2_input_to_components.try_emplace(input, new Vec2Components).first->second.get();

		vr::VRInputComponentHandle_t new_handle = this->new_handle();
		if (x)
			components->x = new_handle;
		else
			components->y = new_handle;

		handle_to_input[new_handle] = input;
		*pHandle = new_handle;
		return vr::VRInputError_None;
	}
	return create_component_common(ulContainer, pchName, pHandle);
}

static struct xrt_vec2
applyDeadzone(struct xrt_vec2 input)
{
	static const float deadzone = [] {
		float raw = debug_get_float_option_lh_stick_deadzone();
		// we apply deadzone to the input's absolute value; valid range is 0..1
		float clamped = CLAMP(raw, 0.0f, 0.99f);
		if (raw != clamped) {
			U_LOG_W("LH_STICK_DEADZONE value of %.2f falls outside of expected range 0..1 - clamp to %.2f",
			        raw, clamped);
		}
		return clamped;
	}();

	if (input.x * input.x + input.y * input.y <= deadzone * deadzone) {
		input.x = 0.0f;
		input.y = 0.0f;
	}

	return input;
}

vr::EVRInputError
Context::UpdateScalarComponent(vr::VRInputComponentHandle_t ulComponent, float fNewValue, double fTimeOffset)
{
	if (auto h = handle_to_input.find(ulComponent); h != handle_to_input.end() && h->second) {
		xrt_input *input = update_component_common(ulComponent, fTimeOffset);
		if (XRT_GET_INPUT_TYPE(input->name) == XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE) {
			std::unique_ptr<Vec2Components> &components = vec2_input_to_components.at(input);
			if (components->x == ulComponent) {
				input->value.vec2.x = fNewValue;
			} else if (components->y == ulComponent) {
				input->value.vec2.y = fNewValue;
			} else {
				CTX_WARN("Attempted to update component with handle %" PRIu64
				         " but it was neither the x nor y "
				         "component of its associated input",
				         ulComponent);
			}
			input->value.vec2 = applyDeadzone(input->value.vec2);

		} else {
			input->value.vec1.x = fNewValue;
		}
	}
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateHapticComponent(vr::PropertyContainerHandle_t ulContainer,
                               const char *pchName,
                               vr::VRInputComponentHandle_t *pHandle)
{
	*pHandle = vr::k_ulInvalidInputComponentHandle;
	Device *d = prop_container_to_device(ulContainer);
	if (!d) {
		return vr::VRInputError_InvalidHandle;
	}

	// Assuming HMDs won't have haptics.
	// Maybe a wrong assumption.
	if (d == hmd) {
		CTX_WARN("Didn't expect HMD with haptics.");
		return vr::VRInputError_InvalidHandle;
	}

	auto *device = static_cast<ControllerDevice *>(d);
	vr::VRInputComponentHandle_t handle = new_handle();
	handle_to_input[handle] = nullptr;
	device->set_haptic_handle(handle);
	*pHandle = handle;

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateSkeletonComponent(vr::PropertyContainerHandle_t ulContainer,
                                 const char *pchName,
                                 const char *pchSkeletonPath,
                                 const char *pchBasePosePath,
                                 vr::EVRSkeletalTrackingLevel eSkeletalTrackingLevel,
                                 const vr::VRBoneTransform_t *pGripLimitTransforms,
                                 uint32_t unGripLimitTransformCount,
                                 vr::VRInputComponentHandle_t *pHandle)
{
	std::string_view path(pchSkeletonPath); // should be /skeleton/hand/left or /skeleton/hand/right
	std::string_view skeleton_pfx("/skeleton/hand/");
	if (!path.starts_with(skeleton_pfx)) {
		CTX_ERR("Got invalid skeleton path: %s", std::string(path).c_str());
		return vr::VRInputError_InvalidSkeleton;
	}

	if (auto ret = create_component_common(ulContainer, pchSkeletonPath, pHandle); ret != vr::VRInputError_None) {
		return ret;
	}

	auto *device = static_cast<ControllerDevice *>(prop_container_to_device(ulContainer));
	path.remove_prefix(skeleton_pfx.size());
	xrt_hand hand;
	if (path == "left") {
		hand = XRT_HAND_LEFT;
	} else if (path == "right") {
		hand = XRT_HAND_RIGHT;
	} else {
		CTX_ERR("Got invalid skeleton path suffix: %s", std::string(path).c_str());
		return vr::VRInputError_InvalidSkeleton;
	}

	device->set_skeleton(std::span(pGripLimitTransforms, unGripLimitTransformCount), hand,
	                     eSkeletalTrackingLevel == vr::VRSkeletalTracking_Estimated, pchSkeletonPath);
	skeleton_to_controller[*pHandle] = device;

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::UpdateSkeletonComponent(vr::VRInputComponentHandle_t ulComponent,
                                 vr::EVRSkeletalMotionRange eMotionRange,
                                 const vr::VRBoneTransform_t *pTransforms,
                                 uint32_t unTransformCount)
{
	if (eMotionRange != vr::VRSkeletalMotionRange_WithoutController) {
		return vr::VRInputError_None;
	}

	if (!update_component_common(ulComponent, 0)) {
		return vr::VRInputError_InvalidHandle;
	}

	auto *device = skeleton_to_controller[ulComponent];
	if (!device) {
		CTX_ERR("Got unknown component handle %" PRIu64, ulComponent);
		return vr::VRInputError_InvalidHandle;
	}

	device->update_skeleton_transforms(std::span(pTransforms, unTransformCount));

	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreatePoseComponent(vr::PropertyContainerHandle_t ulContainer,
                             const char *pchName,
                             vr::VRInputComponentHandle_t *pHandle)
{
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::UpdatePoseComponent(vr::VRInputComponentHandle_t ulComponent,
                             const vr::HmdMatrix34_t *pMatPoseOffset,
                             double fTimeOffset)
{
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::CreateEyeTrackingComponent(vr::PropertyContainerHandle_t ulContainer,
                                    const char *pchName,
                                    vr::VRInputComponentHandle_t *pHandle)
{
	return vr::VRInputError_None;
}

vr::EVRInputError
Context::UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t ulComponent,
                                    const vr::VREyeTrackingData_t *pEyeTrackingData,
                                    double fTimeOffset)
{
	return vr::VRInputError_None;
}

/***** IVRProperties methods *****/

vr::ETrackedPropertyError
Context::ReadPropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle,
                           vr::PropertyRead_t *pBatch,
                           uint32_t unBatchEntryCount)
{
	Device *device = prop_container_to_device(ulContainerHandle);
	if (!device)
		return vr::TrackedProp_InvalidContainer;
	if (!pBatch)
		return vr::TrackedProp_InvalidOperation; // not verified vs steamvr
	return device->handle_read_properties(pBatch, unBatchEntryCount);
}

vr::ETrackedPropertyError
Context::WritePropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle,
                            vr::PropertyWrite_t *pBatch,
                            uint32_t unBatchEntryCount)
{
	Device *device = prop_container_to_device(ulContainerHandle);
	if (!device)
		return vr::TrackedProp_InvalidContainer;
	if (!pBatch)
		return vr::TrackedProp_InvalidOperation; // not verified vs steamvr
	return device->handle_properties(pBatch, unBatchEntryCount);
}

const char *
Context::GetPropErrorNameFromEnum(vr::ETrackedPropertyError error)
{
	return nullptr;
}

Device *
Context::prop_container_to_device(vr::PropertyContainerHandle_t handle)
{
	switch (handle) {
	case 1: {
		return hmd;
		break;
	}
	default: {
		// If the handle corresponds to a controller
		if (handle >= 2 && handle <= MAX_CONTROLLERS + 1) {
			return controller[handle - 2];
		} else {
			return nullptr;
		}
		break;
	}
	}
}

vr::PropertyContainerHandle_t
Context::TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t nDevice)
{
	size_t container = nDevice + 1;
	if (nDevice == 0 && this->hmd) {
		return container;
	}
	if (nDevice >= 1 && nDevice <= MAX_CONTROLLERS && this->controller[nDevice - 1]) {
		return container;
	}

	return vr::k_ulInvalidPropertyContainer;
}

void
Context::Log(const char *pchLogMessage)
{
	CTX_TRACE("[lighthouse]: %s", pchLogMessage);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

xrt_result_t
get_roles(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles)
{
	bool update_gen = false;
	int head, eyes, face, left, right, gamepad;

	u_device_assign_xdev_roles(xsysd->static_xdevs, xsysd->static_xdev_count, &head, &eyes, &face, &left, &right,
	                           &gamepad);

	if (left != out_roles->left || right != out_roles->right || gamepad != out_roles->gamepad) {
		update_gen = true;
	}

	if (update_gen) {
		out_roles->generation_id++;

		out_roles->left = left;
		out_roles->right = right;
		out_roles->gamepad = gamepad;

		if (left != XRT_DEVICE_ROLE_UNASSIGNED) {
			auto *left_dev = static_cast<ControllerDevice *>(xsysd->static_xdevs[left]);
			left_dev->set_active_hand(XRT_HAND_LEFT);
		}

		if (right != XRT_DEVICE_ROLE_UNASSIGNED) {
			auto *right_dev = static_cast<ControllerDevice *>(xsysd->static_xdevs[right]);
			right_dev->set_active_hand(XRT_HAND_RIGHT);
		}
	}

	return XRT_SUCCESS;
}

void
destroy(struct xrt_system_devices *xsysd)
{
	for (uint32_t i = 0; i < ARRAY_SIZE(xsysd->static_xdevs); i++) {
		xrt_device_destroy(&xsysd->static_xdevs[i]);
	}

	svrs->ctx.reset();
	free(svrs);
}

extern "C" enum xrt_result
steamvr_lh_create_devices(struct xrt_prober *xp, struct xrt_system_devices **out_xsysd)
{
	u_logging_level level = debug_get_log_option_lh_log();
	// The driver likes to create a bunch of transient folders -
	// let's try to make sure they're created where they normally are.
	std::filesystem::path dir = STEAM_INSTALL_DIR + "/config/lighthouse";
	if (!std::filesystem::exists(dir)) {
		U_LOG_IFL_W(level,
		            "Couldn't find lighthouse config folder (%s)- transient folders will be created in current "
		            "working directory (%s)",
		            dir.c_str(), std::filesystem::current_path().c_str());
	} else {
		std::filesystem::current_path(dir);
	}

	std::string steamvr{};
	if (getenv("STEAMVR_PATH") != nullptr) {
		steamvr = getenv("STEAMVR_PATH");
	} else {
		steamvr = find_steamvr_install();
	}

	if (steamvr.empty()) {
		U_LOG_IFL_E(level, "Could not find where SteamVR is installed!");
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_IFL_I(level, "Found SteamVR install: %s", steamvr.c_str());

	std::vector<vr::IServerTrackedDeviceProvider *> drivers = {};
	const auto loadDriver = [&](const std::string &soPath, bool require) {
		// TODO: support windows?
		void *driver_lib = dlopen((steamvr + soPath).c_str(), RTLD_LAZY);
		if (!driver_lib) {
			U_LOG_IFL_E(level, "Couldn't open driver lib: %s", dlerror());
			return !require;
		}

		void *sym = dlsym(driver_lib, "HmdDriverFactory");
		if (!sym) {
			U_LOG_IFL_E(level, "Couldn't find HmdDriverFactory in driver lib: %s", dlerror());
			return false;
		}
		using HmdDriverFactory_t = void *(*)(const char *, int *);
		auto factory = reinterpret_cast<HmdDriverFactory_t>(sym);

		vr::EVRInitError err = vr::VRInitError_None;
		drivers.push_back(static_cast<vr::IServerTrackedDeviceProvider *>(
		    factory(vr::IServerTrackedDeviceProvider_Version, (int *)&err)));
		if (err != vr::VRInitError_None) {
			U_LOG_IFL_E(level, "Couldn't get tracked device driver: error %u", err);
			return false;
		}
		return true;
	};
	if (!loadDriver("/drivers/lighthouse/bin/" OVR_PLAT_SUBDIR "/driver_lighthouse" OVR_PLAT_EXT, true))
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	if (debug_get_bool_option_lh_load_slimevr() &&
	    !loadDriver("/drivers/slimevr/bin/" OVR_PLAT_SUBDIR "/driver_slimevr" OVR_PLAT_EXT, false))
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	svrs->ctx = Context::create(STEAM_INSTALL_DIR, steamvr, std::move(drivers));
	if (svrs->ctx == nullptr)
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;

	U_LOG_IFL_I(level, "Lighthouse initialization complete, giving time to setup connected devices...");
	// RunFrame needs to be called to detect controllers
	using namespace std::chrono_literals;
	auto end_time = std::chrono::steady_clock::now() + 1ms * debug_get_num_option_lh_discover_wait_ms();
	std::this_thread::sleep_until(end_time);
	U_LOG_IFL_I(level, "Device search time complete.");

	if (out_xsysd == NULL || *out_xsysd != NULL) {
		U_LOG_IFL_E(level, "Invalid output system pointer");
		return xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct xrt_system_devices *xsysd = &svrs->base;

	std::lock_guard lk(svrs->ctx->devices_mut);
	svrs->ctx->in_setup = false;

	u_system_devices_populate_function_pointers(xsysd, get_roles, destroy);
	xsysd->create_body_tracker = b_body_tracker_create;
	xsysd->create_hand_tracker = b_hand_tracker_create;

	// Include the HMD
	if (svrs->ctx->hmd) {
		if (svrs->ctx->hmd->variant == VIVE_VARIANT_PRO2 && !svrs->ctx->hmd->init_vive_pro_2(xp)) {
			U_LOG_IFL_W(level, "Found Vive Pro 2, but failed to initialize.");
		}

		// Always have a head at index 0 and iterate dev count.
		xsysd->static_xdevs[xsysd->static_xdev_count] = svrs->ctx->hmd;
		xsysd->static_roles.head = xsysd->static_xdevs[xsysd->static_xdev_count++];
	}

	// Include the controllers
	for (size_t i = 0; i < MAX_CONTROLLERS; i++) {
		if (svrs->ctx->controller[i]) {
			xsysd->static_xdevs[xsysd->static_xdev_count++] = svrs->ctx->controller[i];
		}
	}

	*out_xsysd = xsysd;

	return xrt_result::XRT_SUCCESS;
}
