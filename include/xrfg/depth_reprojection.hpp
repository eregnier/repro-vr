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

// Kinematic pseudo-depth fallback (squared motion magnitude ||MV||^2)
// used when no hardware depth buffer is available: faster-moving macroblocks
// are assumed closer to the camera for occlusion sorting.
[[nodiscard]] float asw_kinematic_pseudo_depth(float mv_x, float mv_y) noexcept;

// 3x3 depth-aware adaptive vector median filter.
[[nodiscard]] MotionDepthSample asw_adaptive_median_3x3(
    const std::array<MotionDepthSample, 9>& neighborhood) noexcept;

// Resolves foreground vs. background motion vectors and disocclusion factor
// across a 3x3 neighborhood using either hardware depth (smallest linear Z wins)
// or kinematic pseudo-depth (largest ||MV||^2 wins).
[[nodiscard]] OcclusionResolvedFlow resolve_depth_ordered_occlusion(
    const std::array<MotionDepthSample, 9>& neighborhood) noexcept;

// Extrapolates a pose beyond alpha = 1.0 (e.g. alpha = 1.5 or 2.0 for zero-latency extrapolation)
// as well as supporting standard interpolation in [0, 1].
[[nodiscard]] Pose extrapolate_pose(
    const Pose& previous,
    const Pose& current,
    float alpha) noexcept;

// Computes cross-eye stereo-temporal synthesis coordinates (alternate-eye reconstruction):
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

// ============================================================================
// Phase 2 — Symmetric Pose-Centered Synthesis, Edge Feathering & Depth/MV Refinement
// ============================================================================

struct SymmetricPoseMappings6Dof {
    Pose synthetic_pose{};
    FieldOfViewTangents synthetic_tangents{};
    // Maps a pixel in the synthetic frame's camera P_S back into frame A (previous capture).
    CameraMapping6Dof synthetic_to_previous{};
    // Maps a pixel in the synthetic frame's camera P_S into frame B (current capture).
    CameraMapping6Dof synthetic_to_current{};
    float clamped_alpha{};
};

struct DepthNeighborhood3x3 {
    float foreground_dilated_depth{};  // Minimum linear Z in 3x3 neighborhood (closest surface)
    float background_eroded_depth{};   // Maximum linear Z in 3x3 neighborhood (farthest surface)
    float center_depth{};
    float relative_discontinuity{};
    bool lock_foreground{};            // True when pixel sits on a sharp foreground silhouette
};

struct RingCandidateMotionSample {
    float mv_x{};
    float mv_y{};
    float linear_depth{};
    float distrust{};  // 0.0 = trusted, 1.0 = distrusted (depth/photometric mismatch)
};

// Builds symmetric 6-DOF mappings centered on the synthetic display pose P_S = slerp(P_A, P_B, alpha)
// (with flyaway clamping when extrapolating beyond 1.0). Eliminates the 45 Hz ATW sawtooth judder
// and halves border excursion during rapid head rotation.
[[nodiscard]] SymmetricPoseMappings6Dof make_symmetric_pose_mappings_6dof(
    const Pose& previous_pose,
    const FieldOfViewTangents& previous_tangents,
    const ViewRect2D& previous_rect,
    const Pose& current_pose,
    const FieldOfViewTangents& current_tangents,
    const ViewRect2D& current_rect,
    float alpha,
    const DepthProjectionParams& depth_params = {},
    float max_extrapolation_alpha = 1.75F) noexcept;

// Geodesic angular distance in degrees between two unit quaternions in SO(3).
[[nodiscard]] float pose_angular_error_degrees(
    const Quaternion& first,
    const Quaternion& second) noexcept;

// Euclidean positional distance in millimeters between two 3D positions (meters -> mm).
[[nodiscard]] float pose_positional_error_millimeters(
    const Vec3& first,
    const Vec3& second) noexcept;

// Smooth border attenuation factor in [0, 1] inside a viewport rectangle:
// returns 1.0 in the interior and smoothly ramps down to 0.0 over `feather_pixels`
// near the rectangle edges to eliminate hard black frame-border steps during head turns.
[[nodiscard]] float compute_edge_feather_weight(
    float pixel_x,
    float pixel_y,
    const ViewRect2D& rect,
    float feather_pixels) noexcept;

// 9-tap morphological depth neighborhood analysis (min/max dilation + silhouette lock).
[[nodiscard]] DepthNeighborhood3x3 analyze_depth_neighborhood_3x3(
    const std::array<float, 9>& linear_depths_meters,
    float discontinuity_threshold_ratio = 0.04F) noexcept;

// Damps false optical-flow vectors on static/low-contrast surfaces (HUD, fine grids)
// when photometric squared difference and depth discontinuity are both below threshold.
[[nodiscard]] float compute_static_motion_damping_factor(
    float mv_x,
    float mv_y,
    float luma_abs_difference,
    float min_motion_pixels = 0.35F,
    float full_motion_pixels = 1.75F,
    float static_luma_threshold = 0.025F) noexcept;

// Repairs a distrusted motion vector by selecting the best trusted neighbor from a ring
// whose linear depth matches the target surface depth.
[[nodiscard]] MotionDepthSample repair_distrusted_motion_vector_ring(
    const RingCandidateMotionSample& center,
    const std::array<RingCandidateMotionSample, 8>& ring_neighbors) noexcept;

}  // namespace xrfg
