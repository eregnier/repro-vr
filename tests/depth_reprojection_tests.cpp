#include "xrfg/depth_reprojection.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near_equal(float a, float b, float tolerance = 1.0e-3F) {
    return std::abs(a - b) <= tolerance;
}

xrfg::Pose make_pose(float x, float y, float z, float qy = 0.0F, float qw = 1.0F) {
    return {{0.0F, qy, 0.0F, qw}, {x, y, z}};
}

void test_depth_linearization() {
    xrfg::DepthProjectionParams rev_inf{};
    rev_inf.camera_near = 0.1F;
    rev_inf.camera_far = 1000.0F;
    rev_inf.depth_inverted = true;
    rev_inf.depth_infinite = true;

    // Reversed-Z infinite far: z_linear = z_near / z_hw
    const float z_at_half = xrfg::linearize_hardware_depth(0.5F, rev_inf);
    require(near_equal(z_at_half, 0.2F, 1.0e-5F), "Reversed-Z infinite depth at 0.5 should be 0.2m");

    const float z_at_tenth = xrfg::linearize_hardware_depth(0.1F, rev_inf);
    require(near_equal(z_at_tenth, 1.0F, 1.0e-5F), "Reversed-Z infinite depth at 0.1 should be 1.0m");

    const float roundtrip_hw = xrfg::hardware_depth_from_linear(1.0F, rev_inf);
    require(near_equal(roundtrip_hw, 0.1F, 1.0e-5F), "hardware_depth_from_linear round-trip must match");

    // Standard finite depth: z_hw = 0 -> z_near, z_hw = 1 -> z_far
    xrfg::DepthProjectionParams std_fin{};
    std_fin.camera_near = 0.1F;
    std_fin.camera_far = 100.0F;
    std_fin.depth_inverted = false;
    std_fin.depth_infinite = false;

    require(
        near_equal(xrfg::linearize_hardware_depth(0.0F, std_fin), 0.1F, 1.0e-4F),
        "Standard finite depth at 0.0 should equal z_near");
    require(
        near_equal(xrfg::linearize_hardware_depth(1.0F, std_fin), 100.0F, 1.0e-2F),
        "Standard finite depth at 1.0 should equal z_far");
}

void test_6dof_reprojection_roundtrip_and_parallax() {
    const xrfg::FieldOfViewTangents fov{-1.0F, 1.0F, 1.0F, -1.0F};
    const xrfg::ViewRect2D rect{0.0F, 0.0F, 200.0F, 200.0F};
    const auto source_pose = make_pose(0.0F, 0.0F, 0.0F);
    const auto target_pose = make_pose(0.05F, -0.02F, 0.01F);

    const auto mapping = xrfg::make_camera_mapping_6dof(
        source_pose, fov, rect, target_pose, fov, rect);

    // Near foreground point (Z = 1.0m) vs far background point (Z = 20.0m)
    const auto near_reproj = xrfg::map_target_to_source_6dof(99.5F, 99.5F, 1.0F, mapping);
    const auto far_reproj = xrfg::map_target_to_source_6dof(99.5F, 99.5F, 20.0F, mapping);

    require(near_reproj.valid && far_reproj.valid, "Both 6-DOF reprojections must be valid");

    const float near_shift = std::abs(near_reproj.x - 99.5F);
    const float far_shift = std::abs(far_reproj.x - 99.5F);
    require(
        near_shift > far_shift * 10.0F,
        "Near surface must exhibit ~20x larger translational parallax shift than far surface");

    // Exact inverse round-trip using reprojected_linear_depth
    const auto roundtrip = xrfg::map_source_to_target_6dof(
        near_reproj.x,
        near_reproj.y,
        near_reproj.reprojected_linear_depth,
        mapping);
    require(roundtrip.valid, "Inverse 6-DOF reprojection must be valid");
    require(
        near_equal(roundtrip.x, 99.5F, 1.0e-3F) &&
            near_equal(roundtrip.y, 99.5F, 1.0e-3F),
        "6-DOF target->source->target round-trip must recover exact pixel coordinate");
}

void test_mv_decomposition_and_asw_median() {
    const xrfg::FieldOfViewTangents fov{-1.0F, 1.0F, 1.0F, -1.0F};
    const xrfg::ViewRect2D rect{0.0F, 0.0F, 200.0F, 200.0F};
    const auto prev_pose = make_pose(0.0F, 0.0F, 0.0F);
    const auto curr_pose = make_pose(0.04F, 0.0F, 0.0F);

    // Mapping from target = curr_pose to source = prev_pose:
    const auto curr_to_prev = xrfg::make_camera_mapping_6dof(
        prev_pose, fov, rect, curr_pose, fov, rect);

    const auto prev_proj = xrfg::map_target_to_source_6dof(99.5F, 99.5F, 2.0F, curr_to_prev);
    require(prev_proj.valid, "Previous projection must be valid");

    const float pure_camera_mv_x = prev_proj.x - 99.5F;
    const float pure_camera_mv_y = prev_proj.y - 99.5F;

    // Add a dynamic object motion of (+6.0, -3.0) pixels on top of camera motion:
    const auto decomposed = xrfg::decompose_motion_vector_6dof(
        99.5F,
        99.5F,
        pure_camera_mv_x + 6.0F,
        pure_camera_mv_y - 3.0F,
        2.0F,
        curr_to_prev);
    require(decomposed.valid, "MV decomposition must succeed");
    require(
        near_equal(decomposed.object_mv_x, 6.0F, 1.0e-3F) &&
            near_equal(decomposed.object_mv_y, -3.0F, 1.0e-3F),
        "MV decomposition must isolate exact dynamic object velocity from 6-DOF camera motion");

    // ASW 3x3 adaptive vector median must reject a huge outlier spike at the center:
    const std::array<xrfg::MotionDepthSample, 9> neighborhood{{
        {4.0F, -2.0F, 2.0F, true}, {4.1F, -2.0F, 2.0F, true}, {3.9F, -2.1F, 2.0F, true},
        {4.0F, -1.9F, 2.0F, true}, {95.0F, 80.0F, 2.0F, true}, {4.0F, -2.0F, 2.0F, true},
        {3.9F, -2.0F, 2.0F, true}, {4.1F, -2.1F, 2.0F, true}, {4.0F, -2.0F, 2.0F, true},
    }};
    const auto filtered = xrfg::asw_adaptive_median_3x3(neighborhood);
    require(
        near_equal(filtered.mv_x, 4.0F, 0.2F) &&
            near_equal(filtered.mv_y, -2.0F, 0.2F),
        "ASW 3x3 vector median must reject isolated outlier spikes");
}

void test_depth_ordered_occlusion_and_pose_extrapolation() {
    // 3x3 neighborhood containing a moving foreground edge (Z = 1.0m, MV = (-8, 0))
    // over a stationary background (Z = 10.0m, MV = (0, 0)):
    const std::array<xrfg::MotionDepthSample, 9> edge_neighborhood{{
        {-8.0F, 0.0F, 1.0F, true}, {-8.0F, 0.0F, 1.0F, true}, {0.0F, 0.0F, 10.0F, true},
        {-8.0F, 0.0F, 1.0F, true}, {0.0F, 0.0F, 10.0F, true}, {0.0F, 0.0F, 10.0F, true},
        {-8.0F, 0.0F, 1.0F, true}, {0.0F, 0.0F, 10.0F, true}, {0.0F, 0.0F, 10.0F, true},
    }};
    const auto resolved = xrfg::resolve_depth_ordered_occlusion(edge_neighborhood);
    require(
        near_equal(resolved.foreground_depth, 1.0F, 1.0e-4F) &&
            near_equal(resolved.foreground_mv_x, -8.0F, 1.0e-4F),
        "Depth-ordered occlusion solver must identify Z=1.0m as foreground");
    require(
        near_equal(resolved.background_depth, 10.0F, 1.0e-4F) &&
            near_equal(resolved.background_mv_x, 0.0F, 1.0e-4F),
        "Depth-ordered occlusion solver must identify Z=10.0m as background");
    require(
        resolved.disocclusion_factor > 0.5F,
        "Depth-ordered occlusion solver must flag high disocclusion factor at foreground/background boundary");

    // 6-DOF pose extrapolation (alpha = 1.5 means 0.5 frame ahead of current):
    const auto prev_pose = make_pose(0.0F, 1.6F, 0.0F, 0.0F, 1.0F);
    const auto curr_pose = make_pose(0.02F, 1.6F, -0.01F, 0.05F, 0.9987492F);
    const auto extrap_pose = xrfg::extrapolate_pose(prev_pose, curr_pose, 1.5F);
    require(
        near_equal(extrap_pose.position.x, 0.03F, 1.0e-4F) &&
            near_equal(extrap_pose.position.z, -0.015F, 1.0e-4F),
        "Extrapolated pose position must advance linearly to 1.5x frame interval");
    require(
        extrap_pose.orientation.y > curr_pose.orientation.y,
        "Extrapolated pose orientation must advance along angular velocity");
}

void test_cross_eye_stereo_reprojection() {
    const xrfg::FieldOfViewTangents fov{-1.0F, 1.0F, 1.0F, -1.0F};
    const xrfg::ViewRect2D rect{0.0F, 0.0F, 200.0F, 200.0F};
    const auto left_eye = make_pose(-0.032F, 0.0F, 0.0F);
    const auto right_eye = make_pose(0.032F, 0.0F, 0.0F);

    const auto right_to_left = xrfg::make_camera_mapping_6dof(
        left_eye, fov, rect, right_eye, fov, rect);
    const auto right_to_prev_right = xrfg::make_camera_mapping_6dof(
        right_eye, fov, rect, right_eye, fov, rect);

    // Unoccluded point at Z = 2.0m in both eyes:
    const auto unoccluded = xrfg::reproject_cross_eye_stereo(
        99.5F,
        99.5F,
        2.0F,
        2.0F,
        right_to_left,
        right_to_prev_right);
    require(unoccluded.other_eye_valid, "Cross-eye projection must be inside opposite eye frustum");
    require(
        near_equal(unoccluded.other_eye_weight, 1.0F, 1.0e-4F),
        "Unoccluded stereo pixel must give full weight (1.0) to opposite eye");
    // For Z=2.0m, IPD=0.064m, 90deg FOV on 200px width (f = 100px):
    // disparity = 100 * 0.064 / 2.0 = 3.2 pixels
    require(
        near_equal(unoccluded.other_eye_x - 99.5F, 3.2F, 1.0e-2F),
        "Cross-eye disparity must equal f * IPD / Z = 3.2 pixels");

    // Occluded background point (Z = 5.0m in right eye, but blocked by Z = 1.0m occluder in left eye):
    const auto occluded = xrfg::reproject_cross_eye_stereo(
        99.5F,
        99.5F,
        5.0F,
        1.0F,
        right_to_left,
        right_to_prev_right);
    require(occluded.other_eye_valid, "Projected ray is still within frustum");
    require(
        near_equal(occluded.other_eye_weight, 0.0F, 1.0e-4F),
        "Occluded stereo pixel must fall back to temporal history (weight 0.0 for opposite eye)");
}

}  // namespace

int main() {
    test_depth_linearization();
    test_6dof_reprojection_roundtrip_and_parallax();
    test_mv_decomposition_and_asw_median();
    test_depth_ordered_occlusion_and_pose_extrapolation();
    test_cross_eye_stereo_reprojection();
    std::cout << "PASS: xrfg_depth_reprojection_tests\n";
    return EXIT_SUCCESS;
}
