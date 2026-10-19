#pragma once

#include "xrfg/pose.hpp"

#include <array>
#include <cstdint>

namespace xrfg {

struct DepthProjectionParams {
    float camera_near{0.1F};
    float camera_far{1000.0F};
    bool depth_inverted{true};   // Reversed-Z (1.0 at near, 0.0 at far), standard in UE5/modern DX12
    bool depth_infinite{false};  // Infinite far plane
};

struct FieldOfViewTangents {
    float left{-1.0F};
    float right{1.0F};
    float up{1.0F};
    float down{-1.0F};
};

struct ViewRect2D {
    float offset_x{0.0F};
    float offset_y{0.0F};
    float width{1.0F};
    float height{1.0F};
};

struct CameraMapping6Dof {
    Quaternion target_to_source_rotation{};
    // Position of the target camera origin expressed in the source camera's view frame
    // (OpenXR right-handed view space: +X right, +Y up, -Z forward).
    Vec3 target_to_source_translation{};
    // Position of the source camera origin expressed in the target camera's view frame.
    Vec3 source_to_target_translation{};
    FieldOfViewTangents source_tangents{};
    FieldOfViewTangents target_tangents{};
    ViewRect2D source_rect{};
    ViewRect2D target_rect{};
    DepthProjectionParams depth_params{};
};

struct MappedPoint6Dof {
    float x{};
    float y{};
    float reprojected_linear_depth{};
    bool valid{};
};

struct DecomposedMotionVector {
    float camera_mv_x{};
    float camera_mv_y{};
    float object_mv_x{};
    float object_mv_y{};
    bool valid{};
};

struct MotionDepthSample {
    float mv_x{};
    float mv_y{};
    float linear_depth{1000.0F};
    bool has_hardware_depth{false};
};

struct OcclusionResolvedFlow {
    float foreground_mv_x{};
    float foreground_mv_y{};
    float background_mv_x{};
    float background_mv_y{};
    float foreground_depth{};
    float background_depth{};
    float disocclusion_factor{};  // 0.0 = coherent surface, 1.0 = disoccluded hole behind moving foreground
};

struct CrossEyeSynthesisSample {
    float other_eye_x{};
    float other_eye_y{};
    bool other_eye_valid{};
    float temporal_fallback_x{};
    float temporal_fallback_y{};
    bool temporal_fallback_valid{};
    // Blend weight in [0, 1]: 1.0 uses the fresh opposite eye (L_t -> R_t),
    // 0.0 falls back to the temporal history of the same eye (R_{t-1} -> R_t)
    // when a stereo disocclusion is detected.
    float other_eye_weight{};
};

// Converts normalized hardware depth z_hw in [0, 1] into positive eye-space view distance Z > 0 (meters).
[[nodiscard]] float linearize_hardware_depth(
    float z_hw,
    const DepthProjectionParams& params) noexcept;

// Converts positive eye-space view distance Z > 0 (meters) back into normalized hardware depth z_hw in [0, 1].
[[nodiscard]] float hardware_depth_from_linear(
    float linear_depth_meters,
    const DepthProjectionParams& params) noexcept;

[[nodiscard]] Vec3 rotate_vector_by_quaternion(
    const Quaternion& q,
    const Vec3& v) noexcept;

[[nodiscard]] Quaternion conjugate_quaternion(const Quaternion& q) noexcept;

[[nodiscard]] Quaternion multiply_quaternions(
    const Quaternion& left,
    const Quaternion& right) noexcept;

// Builds a full 6-DOF camera mapping between a source view and a target view.
[[nodiscard]] CameraMapping6Dof make_camera_mapping_6dof(
    const Pose& source_pose,
    const FieldOfViewTangents& source_tangents,
    const ViewRect2D& source_rect,
    const Pose& target_pose,
    const FieldOfViewTangents& target_tangents,
    const ViewRect2D& target_rect,
    const DepthProjectionParams& depth_params = {}) noexcept;

// Maps a pixel in the target view at linear depth Z (meters) into the source view
// with full 6-DOF rotation and positional parallax (Positional TimeWarp).
// If linear_depth_meters <= 0 or non-finite, falls back to pure 3-DOF rotational mapping.
[[nodiscard]] MappedPoint6Dof map_target_to_source_6dof(
    float target_x,
    float target_y,
    float linear_depth_meters,
    const CameraMapping6Dof& mapping) noexcept;

// Maps a pixel in the source view at linear depth Z (meters) into the target view
// with full 6-DOF rotation and positional parallax.
[[nodiscard]] MappedPoint6Dof map_source_to_target_6dof(
    float source_x,
    float source_y,
    float linear_depth_meters,
    const CameraMapping6Dof& mapping) noexcept;

// Separates total pixel motion (current -> previous) into 6-DOF camera motion
// and residual dynamic object motion in screen space.
[[nodiscard]] DecomposedMotionVector decompose_motion_vector_6dof(
    float current_x,
    float current_y,
    float total_mv_x,
    float total_mv_y,
    float current_linear_depth,
    const CameraMapping6Dof& current_to_previous_mapping) noexcept;

// Meta ASW 2.0 kinematic pseudo-depth fallback (ovr_755690: dp2(mv, mv))
// used when no hardware depth buffer is available: faster-moving macroblocks
// are assumed closer to the camera for occlusion sorting.
[[nodiscard]] float asw_kinematic_pseudo_depth(float mv_x, float mv_y) noexcept;

// Meta ASW 2.0 3x3 depth-aware adaptive vector median filter (ovr_7530b0).
[[nodiscard]] MotionDepthSample asw_adaptive_median_3x3(
    const std::array<MotionDepthSample, 9>& neighborhood) noexcept;

// Resolves foreground vs. background motion vectors and disocclusion factor
// across a 3x3 neighborhood using either hardware depth (smallest linear Z wins)
// or ASW kinematic pseudo-depth (largest ||MV||^2 wins).
[[nodiscard]] OcclusionResolvedFlow resolve_depth_ordered_occlusion(
    const std::array<MotionDepthSample, 9>& neighborhood) noexcept;

// Extrapolates a pose beyond alpha = 1.0 (e.g. alpha = 1.5 or 2.0 for zero-latency ASW mode)
// as well as supporting standard interpolation in [0, 1].
[[nodiscard]] Pose extrapolate_pose(
    const Pose& previous,
    const Pose& current,
    float alpha) noexcept;

// Computes cross-eye stereo-temporal synthesis coordinates (PureDark AFW / Luke Ross AER mode):
// projects target eye pixel into the freshly rendered opposite eye using 6-DOF IPD baseline + depth,
// and detects stereo disocclusions by comparing target depth against the opposite eye's depth at
// the projected coordinate, falling back to temporal history of the target eye when occluded.
[[nodiscard]] CrossEyeSynthesisSample reproject_cross_eye_stereo(
    float target_eye_x,
    float target_eye_y,
    float target_linear_depth,
    float other_eye_sampled_linear_depth,
    const CameraMapping6Dof& target_to_other_eye_mapping,
    const CameraMapping6Dof& target_to_previous_same_eye_mapping,
    float object_mv_x = 0.0F,
    float object_mv_y = 0.0F) noexcept;

}  // namespace xrfg
