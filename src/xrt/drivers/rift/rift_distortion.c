// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Handles calculating the distortion profile for Oculus Rift devices.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "rift_distortion.h"

#include "math/m_vec2.h"
#include "math/m_vec3.h"


static float
rift_catmull_rom_spline(const struct rift_catmull_rom_distortion_data *catmull, float scaled_value)
{
	float scaled_value_floor = floorf(scaled_value);
	scaled_value_floor = CLAMP(scaled_value_floor, 0, CATMULL_COEFFICIENTS - 1);

	float t = scaled_value - scaled_value_floor;
	int k = (int)scaled_value_floor;

	float p0, p1, m0, m1;
	switch (k) {
	case 0:
		p0 = 1.0f;
		m0 = (catmull->k[1] - catmull->k[0]);
		p1 = catmull->k[1];
		m1 = 0.5f * (catmull->k[2] - catmull->k[0]);
		break;
	default:
		p0 = catmull->k[k];
		m0 = 0.5f * (catmull->k[k + 1] - catmull->k[k - 1]);
		p1 = catmull->k[k + 1];
		m1 = 0.5f * (catmull->k[k + 2] - catmull->k[k]);
		break;
	case CATMULL_COEFFICIENTS - 2:
		p0 = catmull->k[CATMULL_COEFFICIENTS - 2];
		m0 = 0.5f * (catmull->k[CATMULL_COEFFICIENTS - 1] - catmull->k[CATMULL_COEFFICIENTS - 2]);
		p1 = catmull->k[CATMULL_COEFFICIENTS - 1];
		m1 = catmull->k[CATMULL_COEFFICIENTS - 1] - catmull->k[CATMULL_COEFFICIENTS - 2];
		break;
	case CATMULL_COEFFICIENTS - 1:
		p0 = catmull->k[CATMULL_COEFFICIENTS - 1];
		m0 = catmull->k[CATMULL_COEFFICIENTS - 1] - catmull->k[CATMULL_COEFFICIENTS - 2];
		p1 = p0 + m0;
		m1 = m0;
		break;
	}

	float omt = 1.0f - t;

	float res = (p0 * (1.0f + 2.0f * t) + m0 * t) * omt * omt + (p1 * (1.0f + 2.0f * omt) - m1 * omt) * t * t;

	return res;
}

static float
rift_distortion_distance_scale_squared(const struct rift_lens_distortion *lens_distortion, float distance_squared)
{
	float scale = 1.0f;

	switch (lens_distortion->distortion_version) {
	case RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1: {
		struct rift_catmull_rom_distortion_data data = lens_distortion->data.lcsv_catmull_rom_10;

		float scaled_distance_squared =
		    (float)(CATMULL_COEFFICIENTS - 1) * distance_squared / (data.max_r * data.max_r);

		return rift_catmull_rom_spline(&data, scaled_distance_squared);
	}
	default: return scale;
	}
}

struct xrt_vec3
rift_distortion_distance_scale_squared_split_chroma(const struct rift_lens_distortion *lens_distortion,
                                                    float distance_squared)
{
	float scale = rift_distortion_distance_scale_squared(lens_distortion, distance_squared);

	struct xrt_vec3 scale_split = {scale, scale, scale};

	switch (lens_distortion->distortion_version) {
	case RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1: {
		struct rift_catmull_rom_distortion_data data = lens_distortion->data.lcsv_catmull_rom_10;

		scale_split.x *= 1.0f + data.chromatic_abberation[0] + distance_squared * data.chromatic_abberation[1];
		scale_split.z *= 1.0f + data.chromatic_abberation[2] + distance_squared * data.chromatic_abberation[3];
		break;
	}
	}

	return scale_split;
}

struct rift_distortion_render_info
rift_get_distortion_render_info(struct rift_hmd *hmd, uint32_t view)
{
	const struct rift_lens_distortion *distortion = &hmd->lens_distortions[hmd->distortion_in_use];

	float display_width_meters = MICROMETERS_TO_METERS(hmd->display_info.display_width);
	float display_height_meters = MICROMETERS_TO_METERS(hmd->display_info.display_height);

	float lens_separation_meters = MICROMETERS_TO_METERS(hmd->display_info.lens_separation);
	float center_from_top_meters = MICROMETERS_TO_METERS(hmd->display_info.center_v);

	struct xrt_vec2 pixels_per_meter;
	pixels_per_meter.x =
	    (float)hmd->display_info.resolution_x / (display_width_meters - hmd->extra_display_info.screen_gap_meters);
	pixels_per_meter.y = (float)hmd->display_info.resolution_y / display_height_meters;

	struct xrt_vec2 pixels_per_tan_angle_at_center;
	pixels_per_tan_angle_at_center.x =
	    pixels_per_meter.x * distortion->data.lcsv_catmull_rom_10.meters_per_tan_angle_at_center;
	pixels_per_tan_angle_at_center.y =
	    pixels_per_meter.y * distortion->data.lcsv_catmull_rom_10.meters_per_tan_angle_at_center;

	struct xrt_vec2 tan_eye_angle_scale;
	tan_eye_angle_scale.x =
	    0.25f * (display_width_meters / distortion->data.lcsv_catmull_rom_10.meters_per_tan_angle_at_center);
	tan_eye_angle_scale.y =
	    0.5f * (display_height_meters / distortion->data.lcsv_catmull_rom_10.meters_per_tan_angle_at_center);

	float visible_width_of_one_eye = 0.5f * (display_width_meters - hmd->extra_display_info.screen_gap_meters);
	float center_from_left_meters = (display_width_meters - lens_separation_meters) * 0.5f;

	struct xrt_vec2 lens_center;
	lens_center.x = (center_from_left_meters / visible_width_of_one_eye) * 2.0f - 1.0f;
	lens_center.y = (center_from_top_meters / display_height_meters) * 2.0f - 1.0f;

	// if we're on the right eye, flip the lens center
	if (view != 0) {
		lens_center.x *= -1;
	}

	return (struct rift_distortion_render_info){
	    .distortion = distortion,
	    .lens_center = lens_center,
	    .pixels_per_tan_angle_at_center = pixels_per_tan_angle_at_center,
	    .tan_eye_angle_scale = tan_eye_angle_scale,
	};
}

static struct rift_viewport_fov_tan
rift_calculate_fov_from_eye_position(
    float eye_relief, float offset_to_right, float offset_downwards, float lens_diameter, float extra_eye_rotation)
{
	float half_lens_diameter = lens_diameter * 0.5f;

	struct rift_viewport_fov_tan fov_port;
	fov_port.up_tan = (half_lens_diameter + offset_downwards) / eye_relief;
	fov_port.down_tan = (half_lens_diameter - offset_downwards) / eye_relief;
	fov_port.left_tan = (half_lens_diameter + offset_to_right) / eye_relief;
	fov_port.right_tan = (half_lens_diameter - offset_to_right) / eye_relief;

	if (extra_eye_rotation > 0.0f) {
		extra_eye_rotation = CLAMP(extra_eye_rotation, 0, 30.0f);

		float eyeball_center_to_pupil = 0.0135f;
		float eyeball_lateral_pull = 0.001f * (extra_eye_rotation / DEG_TO_RAD(30.0f));
		float extra_translation = eyeball_center_to_pupil * sinf(extra_eye_rotation) + eyeball_lateral_pull;
		float extra_relief = eyeball_center_to_pupil * (1.0f - cosf(extra_eye_rotation));

		fov_port.up_tan = fmaxf(fov_port.up_tan, (half_lens_diameter + offset_downwards + extra_translation) /
		                                             (eye_relief + extra_relief));
		fov_port.down_tan =
		    fmaxf(fov_port.down_tan,
		          (half_lens_diameter - offset_downwards + extra_translation) / (eye_relief + extra_relief));
		fov_port.left_tan =
		    fmaxf(fov_port.left_tan,
		          (half_lens_diameter + offset_to_right + extra_translation) / (eye_relief + extra_relief));
		fov_port.right_tan =
		    fmaxf(fov_port.right_tan,
		          (half_lens_diameter - offset_to_right + extra_translation) / (eye_relief + extra_relief));
	}

	return fov_port;
}

static struct xrt_vec2
rift_transform_screen_ndc_to_tan_fov_space(const struct rift_distortion_render_info *distortion,
                                           struct xrt_vec2 screen_ndc)
{
	struct xrt_vec2 tan_eye_angle_distorted = {
	    (screen_ndc.x - distortion->lens_center.x) * distortion->tan_eye_angle_scale.x,
	    (screen_ndc.y - distortion->lens_center.y) * distortion->tan_eye_angle_scale.y};

	float distance_squared = (tan_eye_angle_distorted.x * tan_eye_angle_distorted.x) +
	                         (tan_eye_angle_distorted.y * tan_eye_angle_distorted.y);

	float distortion_scale = rift_distortion_distance_scale_squared(distortion->distortion, distance_squared);

	return m_vec2_mul_scalar(tan_eye_angle_distorted, distortion_scale);
}

static struct rift_viewport_fov_tan
rift_fov_find_range(struct xrt_vec2 from,
                    struct xrt_vec2 to,
                    int num_steps,
                    const struct rift_distortion_render_info *distortion)
{
	struct rift_viewport_fov_tan fov_port = {0.0f, 0.0f, 0.0f, 0.0f};

	float step_scale = 1.0f / (num_steps - 1);
	for (int step = 0; step < num_steps; step++) {
		float lerp_factor = step_scale * (float)step;
		struct xrt_vec2 sample = m_vec2_add(from, m_vec2_mul_scalar(m_vec2_sub(to, from), lerp_factor));
		struct xrt_vec2 tan_eye_angle = rift_transform_screen_ndc_to_tan_fov_space(distortion, sample);

		fov_port.left_tan = fmaxf(fov_port.left_tan, -tan_eye_angle.x);
		fov_port.right_tan = fmaxf(fov_port.right_tan, tan_eye_angle.x);
		fov_port.up_tan = fmaxf(fov_port.up_tan, -tan_eye_angle.y);
		fov_port.down_tan = fmaxf(fov_port.down_tan, tan_eye_angle.y);
	}

	return fov_port;
}

static struct rift_viewport_fov_tan
rift_get_physical_screen_fov(const struct rift_distortion_render_info *distortion)
{
	struct xrt_vec2 lens_center = distortion->lens_center;

	struct rift_viewport_fov_tan left_fov_port =
	    rift_fov_find_range(lens_center, (struct xrt_vec2){-1.0f, lens_center.y}, 10, distortion);

	struct rift_viewport_fov_tan right_fov_port =
	    rift_fov_find_range(lens_center, (struct xrt_vec2){1.0f, lens_center.y}, 10, distortion);

	struct rift_viewport_fov_tan up_fov_port =
	    rift_fov_find_range(lens_center, (struct xrt_vec2){lens_center.x, -1.0f}, 10, distortion);

	struct rift_viewport_fov_tan down_fov_port =
	    rift_fov_find_range(lens_center, (struct xrt_vec2){lens_center.x, 1.0f}, 10, distortion);

	return (struct rift_viewport_fov_tan){.left_tan = left_fov_port.left_tan,
	                                      .right_tan = right_fov_port.right_tan,
	                                      .up_tan = up_fov_port.up_tan,
	                                      .down_tan = down_fov_port.down_tan};
}

static struct rift_viewport_fov_tan
rift_clamp_fov_to_physical_screen_fov(const struct rift_distortion_render_info *distortion,
                                      struct rift_viewport_fov_tan fov_port)
{
	struct rift_viewport_fov_tan result_fov_port;
	struct rift_viewport_fov_tan physical_fov_port = rift_get_physical_screen_fov(distortion);

	result_fov_port.left_tan = fminf(fov_port.left_tan, physical_fov_port.left_tan);
	result_fov_port.right_tan = fminf(fov_port.right_tan, physical_fov_port.right_tan);
	result_fov_port.up_tan = fminf(fov_port.up_tan, physical_fov_port.up_tan);
	result_fov_port.down_tan = fminf(fov_port.down_tan, physical_fov_port.down_tan);

	return result_fov_port;
}

struct rift_viewport_fov_tan
rift_calculate_fov_from_hmd(struct rift_hmd *hmd, const struct rift_distortion_render_info *distortion, uint32_t view)
{
	float eye_relief = distortion->distortion->eye_relief;

	struct rift_viewport_fov_tan fov_port;
	fov_port = rift_calculate_fov_from_eye_position(eye_relief, 0, 0, hmd->extra_display_info.lens_diameter_meters,
	                                                DEFAULT_EXTRA_EYE_ROTATION);

	fov_port = rift_clamp_fov_to_physical_screen_fov(distortion, fov_port);

	return fov_port;
}

struct rift_scale_and_offset
rift_calculate_ndc_scale_and_offset_from_fov(const struct rift_viewport_fov_tan *fov)
{
	struct xrt_vec2 proj_scale = {.x = 2.0f / (fov->left_tan + fov->right_tan),
	                              .y = 2.0f / (fov->up_tan + fov->down_tan)};

	struct xrt_vec2 proj_offset = {.x = (fov->left_tan - fov->right_tan) * proj_scale.x * 0.5,
	                               .y = (fov->up_tan - fov->down_tan) * proj_scale.y * 0.5f};

	return (struct rift_scale_and_offset){.scale = proj_scale, .offset = proj_offset};
}

struct rift_scale_and_offset
rift_calculate_uv_scale_and_offset_from_ndc_scale_and_offset(struct rift_scale_and_offset eye_to_source_ndc)
{
	struct rift_scale_and_offset result = eye_to_source_ndc;
	result.scale = m_vec2_mul_scalar(result.scale, 0.5f);
	result.offset = m_vec2_add_scalar(m_vec2_mul_scalar(result.offset, 0.5f), 0.5f);
	return result;
}

static struct xrt_uv_triplet
rift_transform_screen_ndc_to_tan_fov_space_chroma(const struct rift_distortion_render_info *distortion,
                                                  struct xrt_vec2 screen_ndc)
{
	struct xrt_vec2 tan_eye_angle_distorted = {
	    (screen_ndc.x - distortion->lens_center.x) * distortion->tan_eye_angle_scale.x,
	    (screen_ndc.y - distortion->lens_center.y) * distortion->tan_eye_angle_scale.y};

	float distance_squared = (tan_eye_angle_distorted.x * tan_eye_angle_distorted.x) +
	                         (tan_eye_angle_distorted.y * tan_eye_angle_distorted.y);

	struct xrt_vec3 distortion_scales =
	    rift_distortion_distance_scale_squared_split_chroma(distortion->distortion, distance_squared);

	return (struct xrt_uv_triplet){
	    .r = {tan_eye_angle_distorted.x * distortion_scales.x, tan_eye_angle_distorted.y * distortion_scales.x},
	    .g = {tan_eye_angle_distorted.x * distortion_scales.y, tan_eye_angle_distorted.y * distortion_scales.y},
	    .b = {tan_eye_angle_distorted.x * distortion_scales.z, tan_eye_angle_distorted.y * distortion_scales.z},
	};
}

// unused math functions which may be useful in the future for stuff like calculating FOV.
// disabled to not give unused warnings
#if 0
static float
rift_distortion(struct rift_lens_distortion *lens_distortion, float distance)
{
	return distance * rift_distortion_distance_scale_squared(lens_distortion, distance * distance);
}

static float
rift_distortion_distance_inverse(struct rift_lens_distortion *lens_distortion, float distance)
{
	assert(distance <= 20.0f);

	float delta = distance * 0.25f;

	float s = distance * 0.25f;
	float d = fabsf(distance - rift_distortion(lens_distortion, s));

	for (int i = 0; i < 20; i++) {
		float s_up = s + delta;
		float s_down = s - delta;
		float d_up = fabsf(distance - rift_distortion(lens_distortion, s_up));
		float d_down = fabsf(distance - rift_distortion(lens_distortion, s_down));

		if (d_up < d) {
			s = s_up;
			d = d_up;
		} else if (d_down < d) {
			s = s_down;
			d = d_down;
		} else {
			delta *= 0.5f;
		}
	}

	return s;
}

static struct xrt_vec2
rift_transform_tan_fov_space_to_render_target_tex_uv(struct rift_scale_and_offset *eye_to_source_uv,
                                                     struct xrt_vec2 tan_eye_angle)
{
	return m_vec2_add(m_vec2_mul(tan_eye_angle, eye_to_source_uv->scale), eye_to_source_uv->offset);
}

static struct xrt_vec2
rift_transform_render_target_ndc_to_tan_fov_space(struct rift_scale_and_offset *eye_to_source_ndc, struct xrt_vec2 ndc)
{
	return m_vec2_div(m_vec2_sub(ndc, eye_to_source_ndc->offset), eye_to_source_ndc->scale);
}

static struct xrt_vec2
rift_transform_tan_fov_space_to_screen_ndc(struct rift_distortion_render_info *distortion,
                                           struct xrt_vec2 tan_eye_angle)
{
	float tan_eye_angle_radius = m_vec2_len(tan_eye_angle);
	float tan_eye_angle_distorted_radius =
	    rift_distortion_distance_inverse(distortion->distortion, tan_eye_angle_radius);

	struct xrt_vec2 tan_eye_angle_distorted = tan_eye_angle;
	if (tan_eye_angle_radius > 0) {
		tan_eye_angle_distorted =
		    m_vec2_mul_scalar(tan_eye_angle, tan_eye_angle_distorted_radius / tan_eye_angle_radius);
	}

	return m_vec2_add(m_vec2_div(tan_eye_angle_distorted, distortion->tan_eye_angle_scale),
	                  distortion->lens_center);
}
#endif

xrt_result_t
rift_hmd_compute_distortion(struct xrt_device *dev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result)
{
#define TO_NDC(x) ((x * 2) - 1)

	struct rift_hmd *hmd = rift_hmd(dev);

	struct xrt_vec2 source_ndc = {TO_NDC(u), TO_NDC(v)};

	// Flip the input x coordinate for the left eye
	if (view == 0) {
		source_ndc.x *= -1.0f;
	}

	struct rift_distortion_render_info distortion_render_info = rift_get_distortion_render_info(hmd, 0);

	struct xrt_uv_triplet tan_fov_chroma =
	    rift_transform_screen_ndc_to_tan_fov_space_chroma(&distortion_render_info, source_ndc);

	struct rift_scale_and_offset *eye_to_source_uv = &hmd->extra_display_info.eye_to_source_uv;

#if 0
	// no chromatic aberration correction, pulled from the green channel, which has no correction applied
	struct xrt_uv_triplet sample_tex_coord = {
	    .r = m_vec2_add(m_vec2_mul(tan_fov_chroma.g, eye_to_source_uv->scale), eye_to_source_uv->offset),
	    .g = m_vec2_add(m_vec2_mul(tan_fov_chroma.g, eye_to_source_uv->scale), eye_to_source_uv->offset),
	    .b = m_vec2_add(m_vec2_mul(tan_fov_chroma.g, eye_to_source_uv->scale), eye_to_source_uv->offset)};
#else
	struct xrt_uv_triplet sample_tex_coord = {
	    .r = m_vec2_add(m_vec2_mul(tan_fov_chroma.r, eye_to_source_uv->scale), eye_to_source_uv->offset),
	    .g = m_vec2_add(m_vec2_mul(tan_fov_chroma.g, eye_to_source_uv->scale), eye_to_source_uv->offset),
	    .b = m_vec2_add(m_vec2_mul(tan_fov_chroma.b, eye_to_source_uv->scale), eye_to_source_uv->offset)};
#endif

	// Flip the output x coordinate for the left eye back to the right space, this in effect reverses the distortion
	// in the left eye, which is correct.
	if (view == 0) {
		sample_tex_coord.r.x = 1 - sample_tex_coord.r.x;
		sample_tex_coord.g.x = 1 - sample_tex_coord.g.x;
		sample_tex_coord.b.x = 1 - sample_tex_coord.b.x;
	}

	*out_result = sample_tex_coord;

	return XRT_SUCCESS;

#undef TO_NDC
}

// @todo remove clang-format off when CI is updated
// clang-format off
static const struct rift_lens_distortion DK2_DISTORTIONS[] = {
    {
        .distortion_version = RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1,
        .eye_relief = 0.008f,
        .data =
            {
                .lcsv_catmull_rom_10 =
                    {
                        .meters_per_tan_angle_at_center = 0.036f,
                        .max_r = 1.0f,
                        .chromatic_abberation = {-0.0112f, -0.015f, 0.0187f, 0.015f},
                        .k =
                            {
                                1.003f,
                                1.02f,
                                1.042f,
                                1.066f,
                                1.094f,
                                1.126f,
                                1.162f,
                                1.203f,
                                1.25f,
                                1.31f,
                                1.38f,
                            },
                    },
            },
    },
    {
        .distortion_version = RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1,
        .eye_relief = 0.018f,
        .data =
            {
                .lcsv_catmull_rom_10 =
                    {
                        .meters_per_tan_angle_at_center = 0.036f,
                        .max_r = 1.0f,
                        .chromatic_abberation = {-0.015f, -0.02f, 0.025f, 0.02f},
                        .k =
                            {
                                1.003f,
                                1.02f,
                                1.042f,
                                1.066f,
                                1.094f,
                                1.126f,
                                1.162f,
                                1.203f,
                                1.25f,
                                1.31f,
                                1.38f,
                            },
                    },
            },
    }};

static const struct rift_lens_distortion CV1_DISTORTIONS[] = {{
    .distortion_version = RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1,
    .eye_relief = 0.015f,
    .data =
        {
            .lcsv_catmull_rom_10 =
                {
                    .meters_per_tan_angle_at_center = 0.0438f,
                    .max_r = 1.0f,
                    .chromatic_abberation = {-0.008f, -0.005f, 0.015f, 0.005f},
                    .k =
                        {
                            1.000f,
                            1.0312999f,
                            1.0698f,
                            1.1155f,
                            1.173f,
                            1.2460001f,
                            1.336f,
                            1.457f,
                            1.630f,
                            1.900f,
                            2.3599999f,
                        },
                },
        },
}};
// clang-format on

void
rift_fill_in_default_distortions(struct rift_hmd *hmd)
{
	switch (hmd->variant) {
	case RIFT_VARIANT_DK2: {
		hmd->num_lens_distortions = ARRAY_SIZE(DK2_DISTORTIONS);
		hmd->lens_distortions = DK2_DISTORTIONS;

		// TODO: let the user specify which distortion is in use with an env var,
		//       and interpolate the distortions for the user's specific eye relief setting
		hmd->distortion_in_use = 1;

		break;
	}
	case RIFT_VARIANT_CV1: {
		hmd->num_lens_distortions = ARRAY_SIZE(CV1_DISTORTIONS);
		hmd->lens_distortions = CV1_DISTORTIONS;

		// TODO: let the user specify which distortion is in use with an env var,
		//       and interpolate the distortions for the user's specific eye relief setting
		hmd->distortion_in_use = 0;

		break;
	}
	}
}
