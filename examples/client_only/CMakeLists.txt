# Copyright 2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.20)

set(MONADO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${MONADO_ROOT}/cmake/MonadoSetOptions.cmake)
monado_set_options(
	ENABLE
		XRT_MODULE_COMPOSITOR
		# TODO: afaict this shouldn't be strictly necessary in the client-only build,
		#  but XRT_FEATURE_OPENXR currently requires either
		#  XRT_MODULE_COMPOSITOR_NULL or XRT_MODULE_COMPOSITOR_MAIN,
		#  without considering the presence or absence of anything else.
		XRT_MODULE_COMPOSITOR_NULL
		XRT_MODULE_COMPOSITOR_CLIENT
		XRT_MODULE_IPC
		XRT_MODULE_OPENXR_STATE_TRACKER
		XRT_FEATURE_OPENXR
		XRT_FEATURE_CLIENT_WITHOUT_SERVICE
	KEEP_SET OPENXR_FEATURES
	KEEP # TODO: is there more that should be included here?
	     XRT_FEATURE_SSE2
	     XRT_HAVE_VULKAN
	     XRT_HAVE_OPENGL
	     XRT_HAVE_OPENGLES
	     XRT_HAVE_EGL
	     XRT_HAVE_OPENGL_GLX
	     XRT_HAVE_XLIB
	     XRT_HAVE_D3D11
	     XRT_HAVE_D3D12
	     XRT_HAVE_SYSTEM_CJSON
	)
add_subdirectory(${MONADO_ROOT} monado)
