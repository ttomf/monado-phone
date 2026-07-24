// Copyright 2019, Collabora, Ltd.
// Copyright 2016, Sensics, Inc.
// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Base implementations for math library.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup aux_math
 *
 * Based in part on inc/osvr/Util/EigenQuatExponentialMap.h in OSVR-Core
 */
// IWYU pragma: no_include "src/Core/MatrixBase.h"

#pragma once

#include "math/m_api.h"
#include "math/m_eigen_interop.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <assert.h>
#include <cmath>


namespace xrt::auxiliary::math {

template <typename Scalar> struct FourthRootMachineEps;

template <> struct FourthRootMachineEps<double>
{
	/// machine epsilon is 1e-53, so fourth root is roughly 1e-13
	static double
	get()
	{
		return 1.e-13;
	}
};
template <> struct FourthRootMachineEps<float>
{
	/// machine epsilon is 1e-24, so fourth root is 1e-6
	static float
	get()
	{
		return 1.e-6f;
	}
};

/// Computes the "historical" (un-normalized) sinc(Theta)
/// (sine(theta)/theta for theta != 0, defined as the limit value of 0
/// at theta = 0)
template <typename Scalar>
inline Scalar
sinc(Scalar theta)
{
	/// fourth root of machine epsilon is recommended cutoff for taylor
	/// series expansion vs. direct computation per
	/// Grassia, F. S. (1998). Practical Parameterization of Rotations
	/// Using the Exponential Map. Journal of Graphics Tools, 3(3),
	/// 29-48. http://doi.org/10.1080/10867651.1998.10487493
	Scalar ret;
	if (theta < FourthRootMachineEps<Scalar>::get()) {
		// taylor series expansion.
		ret = Scalar(1.f) - theta * theta / Scalar(6.f);
		return ret;
	}
	// direct computation.
	ret = sin(theta) / theta;
	return ret;
}

/// fully-templated free function for quaternion expontiation
template <typename Derived>
inline Eigen::Quaternion<typename Derived::Scalar>
quat_exp(Eigen::MatrixBase<Derived> const &vec)
{
	EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Derived, 3);
	using Scalar = typename Derived::Scalar;
	/// Implementation inspired by
	/// Grassia, F. S. (1998). Practical Parameterization of Rotations
	/// Using the Exponential Map. Journal of Graphics Tools, 3(3),
	/// 29–48. http://doi.org/10.1080/10867651.1998.10487493
	///
	/// @note This is not the SO(3) version of the exponential map, this is the standard form of quaternion
	///       exponentiation, which is why we do not include the factor of 1/2.
	/// @todo Update the tests to take into account the difference between the pure quaternion exponential map and
	///       the SO(3) version of the exponential map as defined by Grassia.
	Scalar theta = vec.norm();
	Scalar vecscale = sinc(theta);
	Eigen::Quaternion<Scalar> ret;
	ret.vec() = vecscale * vec;
	ret.w() = cos(theta);
	return ret.normalized();
}

/// fully-templated free function for quaternion exponentiation, SO(3) version as described by Grassia.
template <typename Derived>
inline Eigen::Quaternion<typename Derived::Scalar>
quat_exp_so3(Eigen::MatrixBase<Derived> const &vec)
{
	EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Derived, 3);
	using Scalar = typename Derived::Scalar;
	/// Implementation inspired by
	/// Grassia, F. S. (1998). Practical Parameterization of Rotations
	/// Using the Exponential Map. Journal of Graphics Tools, 3(3),
	/// 29–48. http://doi.org/10.1080/10867651.1998.10487493
	const Scalar theta = vec.norm();
	const Scalar half_theta = Scalar(0.5) * theta;

	const Scalar vecscale = Scalar(0.5) * sinc(half_theta);
	Eigen::Quaternion<Scalar> ret;
	ret.vec() = vecscale * vec;
	ret.w() = cos(half_theta);
	// @note We don't normalize here since we're using this for Jets and normalization changes the differentials
	//       slightly (along with slight numerical errors). The quaternion is already going to be close enough to
	//       unit length given the input.
	return ret;
}

/// Taylor series expansion of theta over sin(theta), also known as cosecant, for
/// use near 0 when you want continuity and validity at 0.
template <typename Scalar>
inline Scalar
cscTaylorExpansion(Scalar theta)
{
	return Scalar(1) +
	       // theta ^ 2 / 6
	       (theta * theta) / Scalar(6) +
	       // 7 theta^4 / 360
	       (Scalar(7) * theta * theta * theta * theta) / Scalar(360) +
	       // 31 theta^6/15120
	       (Scalar(31) * theta * theta * theta * theta * theta * theta) / Scalar(15120);
}

/// fully-templated free function for quaternion log map.
///
/// Assumes a unit quaternion.
template <typename Scalar>
inline Eigen::Matrix<Scalar, 3, 1>
quat_ln(Eigen::Quaternion<Scalar> const &quat)
{
	// ln q = ( (phi)/(norm of vec) vec, ln(norm of quat))
	// When we assume a unit quaternion, ln(norm of quat) = 0
	// so then we scale the vector part by phi/sin(phi) to get the
	// result (i.e., ln(qv, qw) = (phi/sin(phi)) * qv )
	Scalar vecnorm = quat.vec().norm();

	// "best for numerical stability" vs asin or acos
	Scalar phi = atan2(vecnorm, quat.w());

	// Here is where we compute the coefficient to scale the vector part
	// by, which is nominally phi / std::sin(phi).
	// When the angle approaches zero, we compute the coefficient
	// differently, since it gets a bit like sinc in that we want it
	// continuous but 0 is undefined.
	Scalar phiOverSin = vecnorm < 1e-4 ? cscTaylorExpansion<Scalar>(phi) : (phi / sin(phi));
	return quat.vec() * phiOverSin;
}

} // namespace xrt::auxiliary::math
