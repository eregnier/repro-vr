#include "xrfg/depth_reprojection.hpp"

#include <algorithm>
#include <cmath>

namespace xrfg {
namespace {

constexpr float kEpsilon = 1.0e-6F;
constexpr float kMinLinearDepthMeters = 1.0e-4F;

[[nodiscard]] bool is_finite_vec3(const Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] float median_of_9(std::array<float, 9> values) noexcept {
    std::nth_element(values.begin(), values.begin() + 4, values.end());
    return values[4];
}

}  // namespace

Vec3 rotate_vector_by_quaternion(const Quaternion& q, const Vec3& v) noexcept {
    const Quaternion nq = normalize(q);
    // t = 2 * cross(nq.xyz, v)
    const float tx = 2.0F * (nq.y * v.z - nq.z * v.y);
    const float ty = 2.0F * (nq.z * v.x - nq.x * v.z);
    const float tz = 2.0F * (nq.x * v.y - nq.y * v.x);
    // v' = v + nq.w * t + cross(nq.xyz, t)
    return {
        v.x + nq.w * tx + (nq.y * tz - nq.z * ty),
        v.y + nq.w * ty + (nq.z * tx - nq.x * tz),
        v.z + nq.w * tz + (nq.x * ty - nq.y * tx),
    };
}

Quaternion conjugate_quaternion(const Quaternion& q) noexcept {
    const Quaternion nq = normalize(q);
    return {-nq.x, -nq.y, -nq.z, nq.w};
}

Quaternion multiply_quaternions(
    const Quaternion& left,
    const Quaternion& right) noexcept {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    };
}

float linearize_hardware_depth(
    float z_hw,
    const DepthProjectionParams& params) noexcept {
    if (!std::isfinite(z_hw) || !std::isfinite(params.camera_near) ||
        params.camera_near <= 0.0F) {
        return 0.0F;
    }
    const float near_plane = std::max(params.camera_near, kMinLinearDepthMeters);
    const float clamped_z = std::clamp(z_hw, 0.0F, 1.0F);

    if (params.depth_infinite || !std::isfinite(params.camera_far) ||
        params.camera_far <= near_plane) {
        const float denom = params.depth_inverted
            ? std::max(clamped_z, kEpsilon)
            : std::max(1.0F - clamped_z, kEpsilon);
        return near_plane / denom;
    }

    const float far_plane = params.camera_far;
    const float denom = params.depth_inverted
        ? (near_plane + clamped_z * (far_plane - near_plane))
        : (far_plane - clamped_z * (far_plane - near_plane));
    return (near_plane * far_plane) / std::max(denom, kEpsilon);
}

float hardware_depth_from_linear(
    float linear_depth_meters,
    const DepthProjectionParams& params) noexcept {
    if (!std::isfinite(linear_depth_meters) || linear_depth_meters <= 0.0F ||
        !std::isfinite(params.camera_near) || params.camera_near <= 0.0F) {
        return params.depth_inverted ? 0.0F : 1.0F;
    }
    const float near_plane = std::max(params.camera_near, kMinLinearDepthMeters);
    const float z = std::max(linear_depth_meters, near_plane);

    if (params.depth_infinite || !std::isfinite(params.camera_far) ||
        params.camera_far <= near_plane) {
        const float ratio = near_plane / z;
        return std::clamp(params.depth_inverted ? ratio : (1.0F - ratio), 0.0F, 1.0F);
    }

    const float far_plane = params.camera_far;
    const float clamped_linear = std::min(z, far_plane);
    const float range = std::max(far_plane - near_plane, kEpsilon);
    if (params.depth_inverted) {
        const float z_hw =
            (far_plane * (clamped_linear - near_plane)) / (clamped_linear * range);
        return std::clamp(1.0F - z_hw, 0.0F, 1.0F);
    }
    const float z_hw =
        (far_plane * (clamped_linear - near_plane)) / (clamped_linear * range);
    return std::clamp(z_hw, 0.0F, 1.0F);
}

CameraMapping6Dof make_camera_mapping_6dof(
    const Pose& source_pose,
    const FieldOfViewTangents& source_tangents,
    const ViewRect2D& source_rect,
    const Pose& target_pose,
    const FieldOfViewTangents& target_tangents,
    const ViewRect2D& target_rect,
    const DepthProjectionParams& depth_params) noexcept {
    const Quaternion source_q = normalize(source_pose.orientation);
    const Quaternion target_q = normalize(target_pose.orientation);
    const Quaternion inv_source_q = conjugate_quaternion(source_q);
    const Quaternion inv_target_q = conjugate_quaternion(target_q);

    const Quaternion target_to_source_rot =
        normalize(multiply_quaternions(inv_source_q, target_q));

    const Vec3 world_delta_tgt_minus_src{
        target_pose.position.x - source_pose.position.x,
        target_pose.position.y - source_pose.position.y,
        target_pose.position.z - source_pose.position.z,
    };
    const Vec3 world_delta_src_minus_tgt{
        -world_delta_tgt_minus_src.x,
        -world_delta_tgt_minus_src.y,
        -world_delta_tgt_minus_src.z,
    };

    CameraMapping6Dof mapping{};
    mapping.target_to_source_rotation = target_to_source_rot;
    mapping.target_to_source_translation =
        rotate_vector_by_quaternion(inv_source_q, world_delta_tgt_minus_src);
    mapping.source_to_target_translation =
        rotate_vector_by_quaternion(inv_target_q, world_delta_src_minus_tgt);
    mapping.source_tangents = source_tangents;
    mapping.target_tangents = target_tangents;
    mapping.source_rect = source_rect;
    mapping.target_rect = target_rect;
    mapping.depth_params = depth_params;
    return mapping;
}

MappedPoint6Dof map_target_to_source_6dof(
    float target_x,
    float target_y,
    float linear_depth_meters,
    const CameraMapping6Dof& mapping) noexcept {
    MappedPoint6Dof output{};
    if (!std::isfinite(target_x) || !std::isfinite(target_y) ||
        mapping.target_rect.width <= 0.0F || mapping.target_rect.height <= 0.0F ||
        mapping.source_rect.width <= 0.0F || mapping.source_rect.height <= 0.0F) {
        return output;
    }

    const float tgt_min_x = mapping.target_rect.offset_x;
    const float tgt_min_y = mapping.target_rect.offset_y;
    const float tgt_max_x = tgt_min_x + mapping.target_rect.width - 1.0F;
    const float tgt_max_y = tgt_min_y + mapping.target_rect.height - 1.0F;
    const bool target_inside =
        target_x >= tgt_min_x - 0.5F && target_x <= tgt_max_x + 0.5F &&
        target_y >= tgt_min_y - 0.5F && target_y <= tgt_max_y + 0.5F;

    const float u = (target_x - tgt_min_x + 0.5F) / mapping.target_rect.width;
    const float v = (target_y - tgt_min_y + 0.5F) / mapping.target_rect.height;

    const float tan_x = mapping.target_tangents.left +
        (mapping.target_tangents.right - mapping.target_tangents.left) * u;
    const float tan_y = mapping.target_tangents.up +
        (mapping.target_tangents.down - mapping.target_tangents.up) * v;

    const Vec3 unit_target_ray{tan_x, tan_y, -1.0F};
    Vec3 rotated_ray = rotate_vector_by_quaternion(
        mapping.target_to_source_rotation,
        unit_target_ray);

    const bool has_valid_depth =
        std::isfinite(linear_depth_meters) &&
        linear_depth_meters > kMinLinearDepthMeters &&
        is_finite_vec3(mapping.target_to_source_translation);

    Vec3 source_ray = rotated_ray;
    if (has_valid_depth) {
        const float inv_z = 1.0F / linear_depth_meters;
        source_ray.x += mapping.target_to_source_translation.x * inv_z;
        source_ray.y += mapping.target_to_source_translation.y * inv_z;
        source_ray.z += mapping.target_to_source_translation.z * inv_z;
        output.reprojected_linear_depth = -source_ray.z * linear_depth_meters;
    } else {
        output.reprojected_linear_depth = 0.0F;
    }

    const bool source_in_front = source_ray.z < -1.0e-5F;
    const float inv_ray_z = 1.0F / std::max(-source_ray.z, 1.0e-5F);
    const float src_tan_x = source_ray.x * inv_ray_z;
    const float src_tan_y = source_ray.y * inv_ray_z;

    const float src_fov_w =
        mapping.source_tangents.right - mapping.source_tangents.left;
    const float src_fov_h =
        mapping.source_tangents.down - mapping.source_tangents.up;
    const bool valid_fov =
        src_fov_w > 1.0e-7F && std::abs(src_fov_h) > 1.0e-7F;
    if (!valid_fov) {
        return output;
    }

    const float src_u = (src_tan_x - mapping.source_tangents.left) / src_fov_w;
    const float src_v = (src_tan_y - mapping.source_tangents.up) / src_fov_h;

    const float src_min_x = mapping.source_rect.offset_x;
    const float src_min_y = mapping.source_rect.offset_y;
    const float src_max_x = src_min_x + mapping.source_rect.width;
    const float src_max_y = src_min_y + mapping.source_rect.height;

    output.x = src_min_x + src_u * mapping.source_rect.width - 0.5F;
    output.y = src_min_y + src_v * mapping.source_rect.height - 0.5F;
    output.valid = target_inside && source_in_front &&
        output.x >= src_min_x - 0.5F && output.x <= src_max_x - 0.5F &&
        output.y >= src_min_y - 0.5F && output.y <= src_max_y - 0.5F;
    return output;
}

MappedPoint6Dof map_source_to_target_6dof(
    float source_x,
    float source_y,
    float linear_depth_meters,
    const CameraMapping6Dof& mapping) noexcept {
    CameraMapping6Dof inverse_mapping{};
    inverse_mapping.target_to_source_rotation =
        conjugate_quaternion(mapping.target_to_source_rotation);
    inverse_mapping.target_to_source_translation =
        mapping.source_to_target_translation;
    inverse_mapping.source_to_target_translation =
        mapping.target_to_source_translation;
    inverse_mapping.source_tangents = mapping.target_tangents;
    inverse_mapping.target_tangents = mapping.source_tangents;
    inverse_mapping.source_rect = mapping.target_rect;
    inverse_mapping.target_rect = mapping.source_rect;
    inverse_mapping.depth_params = mapping.depth_params;
    return map_target_to_source_6dof(
        source_x,
        source_y,
        linear_depth_meters,
        inverse_mapping);
}

DecomposedMotionVector decompose_motion_vector_6dof(
    float current_x,
    float current_y,
    float total_mv_x,
    float total_mv_y,
    float current_linear_depth,
    const CameraMapping6Dof& current_to_previous_mapping) noexcept {
    DecomposedMotionVector result{};
    if (!std::isfinite(total_mv_x) || !std::isfinite(total_mv_y)) {
        return result;
    }
    const MappedPoint6Dof static_prev = map_target_to_source_6dof(
        current_x,
        current_y,
        current_linear_depth,
        current_to_previous_mapping);
    if (!static_prev.valid) {
        return result;
    }
    result.camera_mv_x = static_prev.x - current_x;
    result.camera_mv_y = static_prev.y - current_y;
    result.object_mv_x = total_mv_x - result.camera_mv_x;
    result.object_mv_y = total_mv_y - result.camera_mv_y;
    result.valid = true;
    return result;
}

float asw_kinematic_pseudo_depth(float mv_x, float mv_y) noexcept {
    if (!std::isfinite(mv_x) || !std::isfinite(mv_y)) {
        return 0.0F;
    }
    return mv_x * mv_x + mv_y * mv_y;
}

MotionDepthSample asw_adaptive_median_3x3(
    const std::array<MotionDepthSample, 9>& neighborhood) noexcept {
    std::array<float, 9> xs{};
    std::array<float, 9> ys{};
    std::array<float, 9> depths{};
    bool any_hw_depth = false;
    for (std::size_t i = 0; i < 9; ++i) {
        xs[i] = std::isfinite(neighborhood[i].mv_x) ? neighborhood[i].mv_x : 0.0F;
        ys[i] = std::isfinite(neighborhood[i].mv_y) ? neighborhood[i].mv_y : 0.0F;
        depths[i] = std::isfinite(neighborhood[i].linear_depth) &&
                neighborhood[i].linear_depth > 0.0F
            ? neighborhood[i].linear_depth
            : 1000.0F;
        any_hw_depth = any_hw_depth || neighborhood[i].has_hardware_depth;
    }

    const float med_x = median_of_9(xs);
    const float med_y = median_of_9(ys);
    const float med_depth = median_of_9(depths);

    // Find the sample in the 3x3 neighborhood closest to the vector median
    // while respecting depth continuity when hardware depth is present.
    std::size_t best_index = 4;
    float best_score = 1.0e30F;
    for (std::size_t i = 0; i < 9; ++i) {
        const float dx = xs[i] - med_x;
        const float dy = ys[i] - med_y;
        float score = dx * dx + dy * dy;
        if (any_hw_depth) {
            const float rel_depth_diff =
                std::abs(depths[i] - med_depth) / std::max(med_depth, 0.1F);
            score += rel_depth_diff * 4.0F;
        }
        if (score < best_score) {
            best_score = score;
            best_index = i;
        }
    }

    MotionDepthSample output = neighborhood[best_index];
    output.mv_x = med_x;
    output.mv_y = med_y;
    output.linear_depth = med_depth;
    output.has_hardware_depth = any_hw_depth;
    return output;
}

OcclusionResolvedFlow resolve_depth_ordered_occlusion(
    const std::array<MotionDepthSample, 9>& neighborhood) noexcept {
    OcclusionResolvedFlow resolved{};
    bool use_hw_depth = false;
    for (const auto& sample : neighborhood) {
        if (sample.has_hardware_depth && std::isfinite(sample.linear_depth) &&
            sample.linear_depth > 0.0F) {
            use_hw_depth = true;
            break;
        }
    }

    std::size_t fg_idx = 4;
    std::size_t bg_idx = 4;

    if (use_hw_depth) {
        float min_depth = 1.0e30F;
        float max_depth = 0.0F;
        for (std::size_t i = 0; i < 9; ++i) {
            const float d = (std::isfinite(neighborhood[i].linear_depth) &&
                             neighborhood[i].linear_depth > 0.0F)
                ? neighborhood[i].linear_depth
                : 1000.0F;
            if (d < min_depth) {
                min_depth = d;
                fg_idx = i;
            }
            if (d > max_depth) {
                max_depth = d;
                bg_idx = i;
            }
        }
        resolved.foreground_depth = min_depth;
        resolved.background_depth = max_depth;
    } else {
        // Meta ASW 2.0 fallback (ovr_755690): kinematic pseudo-depth = dot(MV, MV).
        // Larger velocity magnitude corresponds to foreground objects moving across background.
        float max_pseudo = -1.0F;
        float min_pseudo = 1.0e30F;
        for (std::size_t i = 0; i < 9; ++i) {
            const float pseudo =
                asw_kinematic_pseudo_depth(neighborhood[i].mv_x, neighborhood[i].mv_y);
            if (pseudo > max_pseudo) {
                max_pseudo = pseudo;
                fg_idx = i;
            }
            if (pseudo < min_pseudo) {
                min_pseudo = pseudo;
                bg_idx = i;
            }
        }
        resolved.foreground_depth = 1.0F / std::max(1.0F + std::sqrt(max_pseudo), 1.0F);
        resolved.background_depth = 1.0F / std::max(1.0F + std::sqrt(min_pseudo), 1.0F);
    }

    resolved.foreground_mv_x = neighborhood[fg_idx].mv_x;
    resolved.foreground_mv_y = neighborhood[fg_idx].mv_y;
    resolved.background_mv_x = neighborhood[bg_idx].mv_x;
    resolved.background_mv_y = neighborhood[bg_idx].mv_y;

    const float dv_x = resolved.foreground_mv_x - resolved.background_mv_x;
    const float dv_y = resolved.foreground_mv_y - resolved.background_mv_y;
    const float velocity_divergence = std::sqrt(dv_x * dv_x + dv_y * dv_y);

    float depth_weight = 1.0F;
    if (use_hw_depth) {
        const float rel_depth_gap =
            (resolved.background_depth - resolved.foreground_depth) /
            std::max(resolved.foreground_depth, 0.05F);
        depth_weight = std::clamp(rel_depth_gap / 0.05F, 0.0F, 1.0F);
    }

    resolved.disocclusion_factor =
        std::clamp((velocity_divergence - 1.5F) / 8.0F, 0.0F, 1.0F) * depth_weight;
    return resolved;
}

Pose extrapolate_pose(
    const Pose& previous,
    const Pose& current,
    float alpha) noexcept {
    if (!std::isfinite(alpha)) {
        return current;
    }
    const float clamped_alpha = std::clamp(alpha, 0.0F, 3.0F);
    if (clamped_alpha <= 1.0F) {
        return interpolate_pose(previous, current, clamped_alpha);
    }

    const Vec3 position{
        previous.position.x +
            (current.position.x - previous.position.x) * clamped_alpha,
        previous.position.y +
            (current.position.y - previous.position.y) * clamped_alpha,
        previous.position.z +
            (current.position.z - previous.position.z) * clamped_alpha,
    };

    const Quaternion q_prev = normalize(previous.orientation);
    Quaternion q_curr = normalize(current.orientation);
    float dot_val =
        q_prev.x * q_curr.x + q_prev.y * q_curr.y +
        q_prev.z * q_curr.z + q_prev.w * q_curr.w;
    if (dot_val < 0.0F) {
        q_curr = {-q_curr.x, -q_curr.y, -q_curr.z, -q_curr.w};
        dot_val = -dot_val;
    }

    // Relative rotation delta_q = q_prev^{-1} * q_curr
    const Quaternion delta_q =
        normalize(multiply_quaternions(conjugate_quaternion(q_prev), q_curr));
    const float w_clamped = std::clamp(delta_q.w, -1.0F, 1.0F);
    const float half_angle = std::acos(w_clamped);
    const float sin_half = std::sin(half_angle);

    Quaternion extrapolated_orientation = q_curr;
    if (std::abs(sin_half) > 1.0e-6F) {
        const float scaled_half_angle = half_angle * clamped_alpha;
        const float scale = std::sin(scaled_half_angle) / sin_half;
        const Quaternion scaled_delta{
            delta_q.x * scale,
            delta_q.y * scale,
            delta_q.z * scale,
            std::cos(scaled_half_angle),
        };
        extrapolated_orientation =
            normalize(multiply_quaternions(q_prev, scaled_delta));
    }

    return {extrapolated_orientation, position};
}

CrossEyeSynthesisSample reproject_cross_eye_stereo(
    float target_eye_x,
    float target_eye_y,
    float target_linear_depth,
    float other_eye_sampled_linear_depth,
    const CameraMapping6Dof& target_to_other_eye_mapping,
    const CameraMapping6Dof& target_to_previous_same_eye_mapping,
    float object_mv_x,
    float object_mv_y) noexcept {
    CrossEyeSynthesisSample sample{};

    const MappedPoint6Dof other_eye_mapped = map_target_to_source_6dof(
        target_eye_x,
        target_eye_y,
        target_linear_depth,
        target_to_other_eye_mapping);
    sample.other_eye_x = other_eye_mapped.x;
    sample.other_eye_y = other_eye_mapped.y;
    sample.other_eye_valid = other_eye_mapped.valid;

    const MappedPoint6Dof temporal_mapped = map_target_to_source_6dof(
        target_eye_x,
        target_eye_y,
        target_linear_depth,
        target_to_previous_same_eye_mapping);
    if (temporal_mapped.valid) {
        sample.temporal_fallback_x = temporal_mapped.x + object_mv_x;
        sample.temporal_fallback_y = temporal_mapped.y + object_mv_y;
        const float min_x = target_to_previous_same_eye_mapping.source_rect.offset_x - 0.5F;
        const float min_y = target_to_previous_same_eye_mapping.source_rect.offset_y - 0.5F;
        const float max_x = min_x + target_to_previous_same_eye_mapping.source_rect.width;
        const float max_y = min_y + target_to_previous_same_eye_mapping.source_rect.height;
        sample.temporal_fallback_valid =
            sample.temporal_fallback_x >= min_x && sample.temporal_fallback_x <= max_x &&
            sample.temporal_fallback_y >= min_y && sample.temporal_fallback_y <= max_y;
    }

    if (!sample.other_eye_valid) {
        sample.other_eye_weight = 0.0F;
        return sample;
    }

    if (std::isfinite(other_eye_sampled_linear_depth) &&
        other_eye_sampled_linear_depth > kMinLinearDepthMeters &&
        other_eye_mapped.reprojected_linear_depth > kMinLinearDepthMeters) {
        // If the surface actually present at other_eye_mapped.(x, y) in the opposite eye
        // is significantly closer than the reprojected depth of our target pixel, then
        // a foreground object in the opposite eye is occluding our target surface
        // (stereo disocclusion).
        const float expected_depth = other_eye_mapped.reprojected_linear_depth;
        const float occlusion_delta = expected_depth - other_eye_sampled_linear_depth;
        const float tolerance = std::max(0.025F * expected_depth, 0.015F);
        if (occlusion_delta > tolerance) {
            const float fade = std::clamp(
                (occlusion_delta - tolerance) / std::max(tolerance * 2.0F, 0.01F),
                0.0F,
                1.0F);
            sample.other_eye_weight = 1.0F - fade;
        } else {
            sample.other_eye_weight = 1.0F;
        }
    } else {
        sample.other_eye_weight = 1.0F;
    }

    if (!sample.temporal_fallback_valid && sample.other_eye_valid) {
        sample.other_eye_weight = 1.0F;
    }
    return sample;
}

SymmetricPoseMappings6Dof make_symmetric_pose_mappings_6dof(
    const Pose& previous_pose,
    const FieldOfViewTangents& previous_tangents,
    const ViewRect2D& previous_rect,
    const Pose& current_pose,
    const FieldOfViewTangents& current_tangents,
    const ViewRect2D& current_rect,
    float alpha,
    const DepthProjectionParams& depth_params,
    float max_extrapolation_alpha) noexcept {
    SymmetricPoseMappings6Dof result{};
    const float safe_max = std::clamp(max_extrapolation_alpha, 1.0F, 3.0F);
    const float clamped_alpha = std::isfinite(alpha)
        ? std::clamp(alpha, 0.0F, safe_max)
        : 0.5F;
    result.clamped_alpha = clamped_alpha;
    result.synthetic_pose = extrapolate_pose(previous_pose, current_pose, clamped_alpha);

    const float fov_blend = std::clamp(clamped_alpha, 0.0F, 1.0F);
    result.synthetic_tangents = {
        previous_tangents.left + (current_tangents.left - previous_tangents.left) * fov_blend,
        previous_tangents.right + (current_tangents.right - previous_tangents.right) * fov_blend,
        previous_tangents.up + (current_tangents.up - previous_tangents.up) * fov_blend,
        previous_tangents.down + (current_tangents.down - previous_tangents.down) * fov_blend,
    };

    // Mapping from synthetic camera P_S to previous capture camera P_A (source = P_A, target = P_S)
    result.synthetic_to_previous = make_camera_mapping_6dof(
        previous_pose,
        previous_tangents,
        previous_rect,
        result.synthetic_pose,
        result.synthetic_tangents,
        current_rect,
        depth_params);

    // Mapping from synthetic camera P_S to current capture camera P_B (source = P_B, target = P_S)
    result.synthetic_to_current = make_camera_mapping_6dof(
        current_pose,
        current_tangents,
        current_rect,
        result.synthetic_pose,
        result.synthetic_tangents,
        current_rect,
        depth_params);

    return result;
}

float pose_angular_error_degrees(
    const Quaternion& first,
    const Quaternion& second) noexcept {
    const Quaternion q1 = normalize(first);
    const Quaternion q2 = normalize(second);
    const float abs_dot = std::clamp(
        std::abs(q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w),
        0.0F,
        1.0F);
    const float angle_rad = 2.0F * std::acos(abs_dot);
    constexpr float kRadToDeg = 180.0F / 3.14159265358979323846F;
    return angle_rad * kRadToDeg;
}

float pose_positional_error_millimeters(
    const Vec3& first,
    const Vec3& second) noexcept {
    const float dx = first.x - second.x;
    const float dy = first.y - second.y;
    const float dz = first.z - second.z;
    const float dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::isfinite(dist_m) ? (dist_m * 1000.0F) : 0.0F;
}

float compute_edge_feather_weight(
    float pixel_x,
    float pixel_y,
    const ViewRect2D& rect,
    float feather_pixels) noexcept {
    if (!std::isfinite(pixel_x) || !std::isfinite(pixel_y) ||
        rect.width <= 0.0F || rect.height <= 0.0F) {
        return 0.0F;
    }
    const float min_x = rect.offset_x - 0.5F;
    const float min_y = rect.offset_y - 0.5F;
    const float max_x = min_x + rect.width;
    const float max_y = min_y + rect.height;
    if (pixel_x < min_x || pixel_x > max_x || pixel_y < min_y || pixel_y > max_y) {
        return 0.0F;
    }
    if (!(feather_pixels > 0.0F) || !std::isfinite(feather_pixels)) {
        return 1.0F;
    }
    const float dist_left = pixel_x - min_x;
    const float dist_right = max_x - pixel_x;
    const float dist_top = pixel_y - min_y;
    const float dist_bottom = max_y - pixel_y;
    const float min_dist = std::min(
        std::min(dist_left, dist_right),
        std::min(dist_top, dist_bottom));
    const float t = std::clamp(min_dist / feather_pixels, 0.0F, 1.0F);
    // Hermite smoothstep for C1 continuous edge transition
    return t * t * (3.0F - 2.0F * t);
}

DepthNeighborhood3x3 analyze_depth_neighborhood_3x3(
    const std::array<float, 9>& linear_depths_meters,
    float discontinuity_threshold_ratio) noexcept {
    DepthNeighborhood3x3 out{};
    out.center_depth = linear_depths_meters[4];
    float min_z = 1.0e9F;
    float max_z = 0.0F;
    bool any_valid = false;
    for (float z : linear_depths_meters) {
        if (std::isfinite(z) && z > kMinLinearDepthMeters) {
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);
            any_valid = true;
        }
    }
    if (!any_valid) {
        return out;
    }
    out.foreground_dilated_depth = min_z;
    out.background_eroded_depth = max_z;
    const float rel_gap = (max_z - min_z) / std::max(min_z, 0.01F);
    out.relative_discontinuity = rel_gap;
    const float threshold = std::max(discontinuity_threshold_ratio, 0.005F);
    out.lock_foreground = rel_gap > threshold;
    return out;
}

float compute_static_motion_damping_factor(
    float mv_x,
    float mv_y,
    float luma_abs_difference,
    float min_motion_pixels,
    float full_motion_pixels,
    float static_luma_threshold) noexcept {
    if (!std::isfinite(mv_x) || !std::isfinite(mv_y) ||
        !std::isfinite(luma_abs_difference)) {
        return 0.0F;
    }
    const float mag = std::sqrt(mv_x * mv_x + mv_y * mv_y);
    const float ramp_span = std::max(full_motion_pixels - min_motion_pixels, 1.0e-3F);
    const float t_mag = std::clamp((mag - min_motion_pixels) / ramp_span, 0.0F, 1.0F);
    const float smooth_mag = t_mag * t_mag * (3.0F - 2.0F * t_mag);

    const float luma_thresh = std::max(static_luma_threshold, 1.0e-4F);
    const float t_luma = std::clamp(luma_abs_difference / luma_thresh, 0.0F, 1.0F);
    const float smooth_luma = t_luma * t_luma * (3.0F - 2.0F * t_luma);

    // If both motion magnitude is small AND photometric difference is tiny,
    // damp the motion vector to zero to prevent HUD/grid shimmer.
    return std::max(smooth_mag, smooth_luma);
}

MotionDepthSample repair_distrusted_motion_vector_ring(
    const RingCandidateMotionSample& center,
    const std::array<RingCandidateMotionSample, 8>& ring_neighbors) noexcept {
    MotionDepthSample output{
        center.mv_x,
        center.mv_y,
        center.linear_depth,
        center.linear_depth > kMinLinearDepthMeters,
    };
    if (center.distrust <= 0.5F) {
        return output;
    }

    const float target_z = std::max(center.linear_depth, 0.01F);
    float best_score = 1.0e9F;
    std::size_t best_index = 8;
    for (std::size_t i = 0; i < ring_neighbors.size(); ++i) {
        const auto& cand = ring_neighbors[i];
        if (!std::isfinite(cand.mv_x) || !std::isfinite(cand.mv_y) ||
            cand.distrust > 0.75F) {
            continue;
        }
        const float rel_z_diff =
            std::abs(cand.linear_depth - target_z) / target_z;
        const float score = cand.distrust * 2.0F + rel_z_diff * 10.0F;
        if (score < best_score) {
            best_score = score;
            best_index = i;
        }
    }

    if (best_index < ring_neighbors.size()) {
        output.mv_x = ring_neighbors[best_index].mv_x;
        output.mv_y = ring_neighbors[best_index].mv_y;
        output.linear_depth = ring_neighbors[best_index].linear_depth;
    }
    return output;
}

}  // namespace xrfg
