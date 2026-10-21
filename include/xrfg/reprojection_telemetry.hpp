// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "xrfg/depth_reprojection.hpp"
#include "xrfg/pose.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace xrfg {

enum class PoseCenteringMode : std::uint32_t {
    // Legacy V008 behavior: synthetic frame is synthesized in frame B's camera frustum
    // and submitted with B.pose (causes 45 Hz ATW sawtooth judder & border jumps during head turns).
    target_b_legacy = 0,
    // Phase 2 default: synthetic frame is synthesized in the interpolated camera pose
    // P_S = slerp(P_A, P_B, alpha), with symmetric A->P_S and B->P_S warping, and
    // submitted with P_S (zero ATW step judder, 50% lower border excursion).
    midpoint_slerp = 1,
    // Zero-latency extrapolation mode with flyaway clamp.
    extrapolated_asw = 2,
    // Meta SpaceWarp single-source forward projection (0 crossfade ghosting)
    meta_asw_single_source = 3,
};

enum class VramProfile : std::uint32_t {
    // Full legacy allocation footprint (unclamped mip chains, 4 staging + 3 history slots + 4 DLSS slots).
    legacy_deep = 0,
    // Balanced footprint: MipLevels=1 on internal resources, 2 staging slots, 3 history slots.
    balanced = 1,
    // Lean footprint (Phase 2 default): MipLevels=1 everywhere, direct history-to-runtime handover
    // (0 current_staging textures), 2 history slots, 2 DLSS slots, lazy depth allocation.
    lean = 2,
};

enum class InputPacingMode : std::uint32_t {
    // Legacy pacing: unbounded virtual clock lead (up to 2 virtual periods) + synthetic age hold.
    end_frame_legacy = 0,
    // Low-latency controller input mode (Phase 2 default): virtual clock lead clamped to 1 period,
    // adaptive drift re-anchoring, and immediate readiness handover without artificial age hold.
    low_latency_clamp = 1,
};

struct Phase2TuningConfig {
    PoseCenteringMode pose_centering{PoseCenteringMode::target_b_legacy};
    VramProfile vram_profile{VramProfile::legacy_deep};
    InputPacingMode input_pacing{InputPacingMode::end_frame_legacy};
    bool keep_synthetic_depth{false};
    bool depth_edge_dilation{true};
    bool static_motion_reject{true};
    float edge_feather_pixels{10.0F};
    float max_extrapolation_ms{18.0F};

    [[nodiscard]] static constexpr Phase2TuningConfig recommended_phase2() noexcept {
        Phase2TuningConfig cfg{};
        cfg.pose_centering = PoseCenteringMode::midpoint_slerp;
        cfg.vram_profile = VramProfile::lean;
        cfg.input_pacing = InputPacingMode::low_latency_clamp;
        cfg.keep_synthetic_depth = true;
        cfg.depth_edge_dilation = true;
        cfg.static_motion_reject = true;
        cfg.edge_feather_pixels = 10.0F;
        cfg.max_extrapolation_ms = 18.0F;
        return cfg;
    }
};

struct VramFootprintEstimate {
    std::uint64_t single_image_mip0_bytes{};
    std::uint64_t single_image_full_mip_bytes{};
    std::uint64_t private_swapchains_bytes{};
    std::uint64_t staging_textures_bytes{};
    std::uint64_t history_color_bytes{};
    std::uint64_t history_depth_bytes{};
    std::uint64_t synthesizer_work_bytes{};
    std::uint64_t dlss_guide_slots_bytes{};
    std::uint64_t total_bytes{};

    [[nodiscard]] double total_megabytes() const noexcept {
        return static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    }
};

struct VramLiveSnapshot {
    std::uint64_t private_swapchains_bytes{};
    std::uint64_t staging_textures_bytes{};
    std::uint64_t history_color_bytes{};
    std::uint64_t history_depth_bytes{};
    std::uint64_t synthesizer_work_bytes{};
    std::uint64_t dlss_guide_slots_bytes{};

    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return private_swapchains_bytes + staging_textures_bytes +
               history_color_bytes + history_depth_bytes +
               synthesizer_work_bytes + dlss_guide_slots_bytes;
    }

    [[nodiscard]] double total_megabytes() const noexcept {
        return static_cast<double>(total_bytes()) / (1024.0 * 1024.0);
    }
};

struct KinematicFrameMetrics {
    // Angular error in degrees between the pose declared by the submitted frame
    // and the true HMD pose at the display scanout instant.
    float atw_pose_error_deg{};
    // Positional error in millimeters between declared frame pose and scanout HMD pose.
    float atw_position_error_mm{};
    // Frame-to-frame variation in ATW angular error (direct measure of 45 Hz micro-judder).
    float inter_frame_angular_jerk_deg{};
    // Border excursion in pixels caused by camera rotation between capture and scanout.
    float border_excursion_pixels{};
    // End-to-end input-to-photon pipeline latency in milliseconds.
    float pipeline_latency_ms{};
};

// Analytical VRAM calculator for comparing Legacy vs. Balanced vs. Lean profiles.
[[nodiscard]] VramFootprintEstimate estimate_vram_footprint(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t array_size,
    std::uint32_t bytes_per_pixel,
    std::uint32_t application_mip_count,
    VramProfile profile,
    bool depth_actually_submitted = false,
    bool dlss_guides_active = false) noexcept;

// Evaluates the kinematic smoothness of a pair of submitted frames (Synthetic S at t_S, Real B at t_B)
// against the ground-truth HMD trajectory during a head turn or controller rotation.
[[nodiscard]] KinematicFrameMetrics evaluate_pair_kinematic_metrics(
    const Pose& capture_pose_a,
    const Pose& capture_pose_b,
    const Pose& submitted_synthetic_pose,
    const Pose& actual_hmd_pose_at_synthetic_scanout,
    float horizontal_fov_rad,
    std::uint32_t viewport_width_px,
    float pipeline_latency_ms) noexcept;

// Clamps virtual display clock lead to prevent controller input queuing (AdaptiveSrc-style guard).
[[nodiscard]] std::int64_t clamp_low_latency_virtual_display_time(
    std::int64_t runtime_predicted_display_time_ns,
    std::int64_t virtual_period_ns,
    std::int64_t last_served_virtual_time_ns,
    InputPacingMode mode) noexcept;

// Lock-free atomic tracker for live VRAM allocations and kinematic telemetry.
class ReprojectionTelemetryRegistry final {
public:
    static ReprojectionTelemetryRegistry& instance() noexcept;

    void reset() noexcept;

    void record_private_swapchains_bytes(std::uint64_t bytes) noexcept;
    void record_staging_textures_bytes(std::uint64_t bytes) noexcept;
    void record_history_color_bytes(std::uint64_t bytes) noexcept;
    void record_history_depth_bytes(std::uint64_t bytes) noexcept;
    void record_synthesizer_work_bytes(std::uint64_t bytes) noexcept;
    void record_dlss_guide_slots_bytes(std::uint64_t bytes) noexcept;

    [[nodiscard]] VramLiveSnapshot vram_snapshot() const noexcept;

    void record_kinematic_sample(const KinematicFrameMetrics& metrics) noexcept;
    [[nodiscard]] KinematicFrameMetrics latest_kinematic_metrics() const noexcept;
    [[nodiscard]] std::uint64_t kinematic_sample_count() const noexcept;

    void set_tuning_config(const Phase2TuningConfig& config) noexcept;
    [[nodiscard]] Phase2TuningConfig tuning_config() const noexcept;

    // Hot-reloads Phase 2 tuning parameters from `ofxr_bridge.ini` in `directory`
    // if at least 250 ms have elapsed and the file timestamp changed (or immediately
    // when `force_reload` is true).
    bool poll_hot_reload(
        const std::filesystem::path& directory,
        bool force_reload = false) noexcept;

    // Formats a compact single-line diagnostic summary for logs / HUD overlay.
    [[nodiscard]] std::string format_diagnostic_summary() const;

private:
    ReprojectionTelemetryRegistry() = default;

    std::atomic<std::uint64_t> private_swapchains_bytes_{0};
    std::atomic<std::uint64_t> staging_textures_bytes_{0};
    std::atomic<std::uint64_t> history_color_bytes_{0};
    std::atomic<std::uint64_t> history_depth_bytes_{0};
    std::atomic<std::uint64_t> synthesizer_work_bytes_{0};
    std::atomic<std::uint64_t> dlss_guide_slots_bytes_{0};

    std::atomic<float> last_atw_pose_error_deg_{0.0F};
    std::atomic<float> last_atw_position_error_mm_{0.0F};
    std::atomic<float> last_angular_jerk_deg_{0.0F};
    std::atomic<float> last_border_excursion_px_{0.0F};
    std::atomic<float> last_pipeline_latency_ms_{0.0F};
    std::atomic<std::uint64_t> kinematic_samples_{0};

    std::atomic<std::uint32_t> pose_centering_mode_{
        static_cast<std::uint32_t>(PoseCenteringMode::target_b_legacy)};
    std::atomic<std::uint32_t> vram_profile_{
        static_cast<std::uint32_t>(VramProfile::legacy_deep)};
    std::atomic<std::uint32_t> input_pacing_mode_{
        static_cast<std::uint32_t>(InputPacingMode::end_frame_legacy)};
    std::atomic<bool> keep_synthetic_depth_{false};
    std::atomic<bool> depth_edge_dilation_{true};
    std::atomic<bool> static_motion_reject_{true};
    std::atomic<float> edge_feather_pixels_{10.0F};
    std::atomic<float> max_extrapolation_ms_{18.0F};

    std::atomic<std::int64_t> last_poll_steady_ns_{0};
    std::atomic<std::int64_t> last_file_write_ticks_{0};
};

}  // namespace xrfg
