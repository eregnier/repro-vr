// SPDX-License-Identifier: MIT OR Apache-2.0
#include "xrfg/reprojection_telemetry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string_view>

namespace xrfg {
namespace {

[[nodiscard]] std::uint64_t compute_mip_chain_bytes(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t array_size,
    std::uint32_t bytes_per_pixel,
    std::uint32_t mip_count) noexcept {
    std::uint64_t total = 0;
    std::uint32_t w = std::max(width, 1U);
    std::uint32_t h = std::max(height, 1U);
    const std::uint32_t layers = std::max(array_size, 1U);
    const std::uint32_t mips = std::max(mip_count, 1U);
    for (std::uint32_t m = 0; m < mips; ++m) {
        total += static_cast<std::uint64_t>(w) *
                 static_cast<std::uint64_t>(h) *
                 static_cast<std::uint64_t>(layers) *
                 static_cast<std::uint64_t>(bytes_per_pixel);
        w = std::max(w >> 1U, 1U);
        h = std::max(h >> 1U, 1U);
    }
    return total;
}

[[nodiscard]] std::string trim_ascii(std::string_view sv) {
    std::size_t start = 0;
    while (start < sv.size() &&
           std::isspace(static_cast<unsigned char>(sv[start])) != 0) {
        ++start;
    }
    std::size_t end = sv.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(sv[end - 1])) != 0) {
        --end;
    }
    std::string out(sv.substr(start, end - start));
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

VramFootprintEstimate estimate_vram_footprint(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t array_size,
    std::uint32_t bytes_per_pixel,
    std::uint32_t application_mip_count,
    VramProfile profile,
    bool depth_actually_submitted,
    bool dlss_guides_active) noexcept {
    VramFootprintEstimate est{};
    const std::uint32_t app_mips = std::max(application_mip_count, 1U);
    est.single_image_mip0_bytes = compute_mip_chain_bytes(
        width, height, array_size, bytes_per_pixel, 1U);
    est.single_image_full_mip_bytes = compute_mip_chain_bytes(
        width, height, array_size, bytes_per_pixel, app_mips);

    // Depth is 4 bytes/pixel (D32_FLOAT), MV is 4 bytes/pixel (R16G16_FLOAT)
    const std::uint64_t depth_mip0_bytes = compute_mip_chain_bytes(
        width, height, array_size, 4U, 1U);
    const std::uint64_t mv_mip0_bytes = compute_mip_chain_bytes(
        width, height, array_size, 4U, 1U);

    // OpenXR runtime allocates 3 swapchain images per private swapchain.
    constexpr std::uint64_t kRuntimeImagesPerSwapchain = 3;

    if (profile == VramProfile::legacy_deep) {
        // Legacy behavior: native D3D12 kept application mip_count on private swapchains,
        // staging textures, and history slots; allocated 4 staging textures (2 current + 2 synthetic),
        // 3 history color slots, 3 history depth slots unconditionally, 3 work slots, and 4 DLSS slots.
        est.private_swapchains_bytes =
            2ULL * kRuntimeImagesPerSwapchain * est.single_image_full_mip_bytes;
        est.staging_textures_bytes = 4ULL * est.single_image_full_mip_bytes;
        est.history_color_bytes = 3ULL * est.single_image_full_mip_bytes;
        est.history_depth_bytes = 3ULL * depth_mip0_bytes;
        // 3 work slots with half-res R8 luma + flow + packed RGBA8 fallback
        est.synthesizer_work_bytes = 3ULL * (est.single_image_mip0_bytes / 4ULL);
        est.dlss_guide_slots_bytes = 4ULL * (mv_mip0_bytes + depth_mip0_bytes);
    } else if (profile == VramProfile::balanced) {
        // Balanced: MipLevels=1 forced on all internal resources, 2 synthetic staging textures
        // (0 current_staging by copying directly from history), 3 history slots, 2 work slots.
        est.private_swapchains_bytes =
            2ULL * kRuntimeImagesPerSwapchain * est.single_image_mip0_bytes;
        est.staging_textures_bytes = 2ULL * est.single_image_mip0_bytes;
        est.history_color_bytes = 3ULL * est.single_image_mip0_bytes;
        est.history_depth_bytes = depth_actually_submitted
            ? (3ULL * depth_mip0_bytes)
            : 0ULL;
        est.synthesizer_work_bytes = 2ULL * (est.single_image_mip0_bytes / 8ULL);
        est.dlss_guide_slots_bytes = dlss_guides_active
            ? (2ULL * (mv_mip0_bytes + depth_mip0_bytes))
            : 0ULL;
    } else {
        // Lean (Phase 2 default): MipLevels=1 everywhere, 1 synthetic staging buffer
        // (or direct private swapchain write), 2 history color slots, lazy 2-slot depth,
        // 2 work slots (R8 luma only), lazy 2-slot DLSS guides.
        est.private_swapchains_bytes =
            2ULL * kRuntimeImagesPerSwapchain * est.single_image_mip0_bytes;
        est.staging_textures_bytes = 1ULL * est.single_image_mip0_bytes;
        est.history_color_bytes = 2ULL * est.single_image_mip0_bytes;
        est.history_depth_bytes = depth_actually_submitted
            ? (2ULL * depth_mip0_bytes)
            : 0ULL;
        est.synthesizer_work_bytes = 2ULL * (est.single_image_mip0_bytes / 12ULL);
        est.dlss_guide_slots_bytes = dlss_guides_active
            ? (2ULL * (mv_mip0_bytes + depth_mip0_bytes))
            : 0ULL;
    }

    est.total_bytes =
        est.private_swapchains_bytes +
        est.staging_textures_bytes +
        est.history_color_bytes +
        est.history_depth_bytes +
        est.synthesizer_work_bytes +
        est.dlss_guide_slots_bytes;
    return est;
}

KinematicFrameMetrics evaluate_pair_kinematic_metrics(
    const Pose& capture_pose_a,
    const Pose& capture_pose_b,
    const Pose& submitted_synthetic_pose,
    const Pose& actual_hmd_pose_at_synthetic_scanout,
    float horizontal_fov_rad,
    std::uint32_t viewport_width_px,
    float pipeline_latency_ms) noexcept {
    KinematicFrameMetrics m{};
    m.atw_pose_error_deg = pose_angular_error_degrees(
        submitted_synthetic_pose.orientation,
        actual_hmd_pose_at_synthetic_scanout.orientation);
    m.atw_position_error_mm = pose_positional_error_millimeters(
        submitted_synthetic_pose.position,
        actual_hmd_pose_at_synthetic_scanout.position);

    // At frame B's scanout (t_B), B's declared pose matches capture_pose_b (0 deg error).
    // Therefore the 45 Hz alternating ATW step jerk between S and B equals S's ATW pose error!
    m.inter_frame_angular_jerk_deg = m.atw_pose_error_deg;

    // Border excursion in pixels: how far the compositor must shift the frame border
    // to align the submitted synthetic frustum with the scanout HMD orientation, plus
    // the maximum camera rotation angle between the source captures and the synthetic pose.
    const float max_source_to_synth_deg = std::max(
        pose_angular_error_degrees(
            capture_pose_a.orientation,
            submitted_synthetic_pose.orientation),
        pose_angular_error_degrees(
            capture_pose_b.orientation,
            submitted_synthetic_pose.orientation));
    const float effective_border_angle_deg =
        max_source_to_synth_deg + m.atw_pose_error_deg;
    constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
    const float fov = std::max(horizontal_fov_rad, 0.5F);
    const float px_per_rad = static_cast<float>(viewport_width_px) / fov;
    m.border_excursion_pixels =
        effective_border_angle_deg * kDegToRad * px_per_rad;
    m.pipeline_latency_ms = std::max(pipeline_latency_ms, 0.0F);
    return m;
}

std::int64_t clamp_low_latency_virtual_display_time(
    std::int64_t runtime_predicted_display_time_ns,
    std::int64_t virtual_period_ns,
    std::int64_t last_served_virtual_time_ns,
    InputPacingMode mode) noexcept {
    if (virtual_period_ns <= 0) {
        return std::max(
            runtime_predicted_display_time_ns,
            last_served_virtual_time_ns + 1);
    }
    const std::int64_t anchor =
        runtime_predicted_display_time_ns + virtual_period_ns;
    // In low-latency mode, cap virtual clock lead to 1.25x virtual_period ahead of
    // runtime_predicted_display_time (instead of 2.0x in legacy mode) so the game thread
    // never queues multiple controller-input frames ahead of display scanout.
    const std::int64_t ceiling = (mode == InputPacingMode::low_latency_clamp)
        ? (runtime_predicted_display_time_ns + virtual_period_ns + (virtual_period_ns / 4))
        : (anchor + virtual_period_ns);

    std::int64_t candidate = anchor;
    if (last_served_virtual_time_ns != 0 &&
        candidate <= last_served_virtual_time_ns) {
        candidate = (mode == InputPacingMode::low_latency_clamp)
            ? (last_served_virtual_time_ns + 1)
            : (last_served_virtual_time_ns + virtual_period_ns);
    }
    if (candidate > ceiling) {
        candidate = ceiling;
    }
    if (last_served_virtual_time_ns != 0 &&
        candidate <= last_served_virtual_time_ns) {
        candidate = last_served_virtual_time_ns + 1;
    }
    return candidate;
}

ReprojectionTelemetryRegistry& ReprojectionTelemetryRegistry::instance() noexcept {
    static ReprojectionTelemetryRegistry reg;
    return reg;
}

void ReprojectionTelemetryRegistry::reset() noexcept {
    private_swapchains_bytes_.store(0, std::memory_order_relaxed);
    staging_textures_bytes_.store(0, std::memory_order_relaxed);
    history_color_bytes_.store(0, std::memory_order_relaxed);
    history_depth_bytes_.store(0, std::memory_order_relaxed);
    synthesizer_work_bytes_.store(0, std::memory_order_relaxed);
    dlss_guide_slots_bytes_.store(0, std::memory_order_relaxed);

    last_atw_pose_error_deg_.store(0.0F, std::memory_order_relaxed);
    last_atw_position_error_mm_.store(0.0F, std::memory_order_relaxed);
    last_angular_jerk_deg_.store(0.0F, std::memory_order_relaxed);
    last_border_excursion_px_.store(0.0F, std::memory_order_relaxed);
    last_pipeline_latency_ms_.store(0.0F, std::memory_order_relaxed);
    kinematic_samples_.store(0, std::memory_order_relaxed);

    set_tuning_config(Phase2TuningConfig{});
    last_poll_steady_ns_.store(0, std::memory_order_relaxed);
    last_file_write_ticks_.store(0, std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::record_private_swapchains_bytes(
    std::uint64_t bytes) noexcept {
    private_swapchains_bytes_.store(bytes, std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::record_staging_textures_bytes(
    std::uint64_t bytes) noexcept {
    staging_textures_bytes_.store(bytes, std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::record_history_color_bytes(
    std::uint64_t bytes) noexcept {
    history_color_bytes_.store(bytes, std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::record_history_depth_bytes(
    std::uint64_t bytes) noexcept {
    history_depth_bytes_.store(bytes, std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::record_synthesizer_work_bytes(
    std::uint64_t bytes) noexcept {
    synthesizer_work_bytes_.store(bytes, std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::record_dlss_guide_slots_bytes(
    std::uint64_t bytes) noexcept {
    dlss_guide_slots_bytes_.store(bytes, std::memory_order_relaxed);
}

VramLiveSnapshot ReprojectionTelemetryRegistry::vram_snapshot() const noexcept {
    return {
        private_swapchains_bytes_.load(std::memory_order_relaxed),
        staging_textures_bytes_.load(std::memory_order_relaxed),
        history_color_bytes_.load(std::memory_order_relaxed),
        history_depth_bytes_.load(std::memory_order_relaxed),
        synthesizer_work_bytes_.load(std::memory_order_relaxed),
        dlss_guide_slots_bytes_.load(std::memory_order_relaxed),
    };
}

void ReprojectionTelemetryRegistry::record_kinematic_sample(
    const KinematicFrameMetrics& metrics) noexcept {
    last_atw_pose_error_deg_.store(metrics.atw_pose_error_deg, std::memory_order_relaxed);
    last_atw_position_error_mm_.store(metrics.atw_position_error_mm, std::memory_order_relaxed);
    last_angular_jerk_deg_.store(metrics.inter_frame_angular_jerk_deg, std::memory_order_relaxed);
    last_border_excursion_px_.store(metrics.border_excursion_pixels, std::memory_order_relaxed);
    last_pipeline_latency_ms_.store(metrics.pipeline_latency_ms, std::memory_order_relaxed);
    kinematic_samples_.fetch_add(1, std::memory_order_relaxed);
}

KinematicFrameMetrics ReprojectionTelemetryRegistry::latest_kinematic_metrics() const noexcept {
    return {
        last_atw_pose_error_deg_.load(std::memory_order_relaxed),
        last_atw_position_error_mm_.load(std::memory_order_relaxed),
        last_angular_jerk_deg_.load(std::memory_order_relaxed),
        last_border_excursion_px_.load(std::memory_order_relaxed),
        last_pipeline_latency_ms_.load(std::memory_order_relaxed),
    };
}

std::uint64_t ReprojectionTelemetryRegistry::kinematic_sample_count() const noexcept {
    return kinematic_samples_.load(std::memory_order_relaxed);
}

void ReprojectionTelemetryRegistry::set_tuning_config(
    const Phase2TuningConfig& config) noexcept {
    pose_centering_mode_.store(
        static_cast<std::uint32_t>(config.pose_centering),
        std::memory_order_relaxed);
    vram_profile_.store(
        static_cast<std::uint32_t>(config.vram_profile),
        std::memory_order_relaxed);
    input_pacing_mode_.store(
        static_cast<std::uint32_t>(config.input_pacing),
        std::memory_order_relaxed);
    keep_synthetic_depth_.store(
        config.keep_synthetic_depth,
        std::memory_order_relaxed);
    depth_edge_dilation_.store(
        config.depth_edge_dilation,
        std::memory_order_relaxed);
    static_motion_reject_.store(
        config.static_motion_reject,
        std::memory_order_relaxed);
    edge_feather_pixels_.store(
        config.edge_feather_pixels,
        std::memory_order_relaxed);
    max_extrapolation_ms_.store(
        config.max_extrapolation_ms,
        std::memory_order_relaxed);
}

Phase2TuningConfig ReprojectionTelemetryRegistry::tuning_config() const noexcept {
    Phase2TuningConfig cfg{};
    cfg.pose_centering = static_cast<PoseCenteringMode>(
        pose_centering_mode_.load(std::memory_order_relaxed));
    cfg.vram_profile = static_cast<VramProfile>(
        vram_profile_.load(std::memory_order_relaxed));
    cfg.input_pacing = static_cast<InputPacingMode>(
        input_pacing_mode_.load(std::memory_order_relaxed));
    cfg.keep_synthetic_depth =
        keep_synthetic_depth_.load(std::memory_order_relaxed);
    cfg.depth_edge_dilation =
        depth_edge_dilation_.load(std::memory_order_relaxed);
    cfg.static_motion_reject =
        static_motion_reject_.load(std::memory_order_relaxed);
    cfg.edge_feather_pixels =
        edge_feather_pixels_.load(std::memory_order_relaxed);
    cfg.max_extrapolation_ms =
        max_extrapolation_ms_.load(std::memory_order_relaxed);
    return cfg;
}

bool ReprojectionTelemetryRegistry::poll_hot_reload(
    const std::filesystem::path& directory,
    bool force_reload) noexcept {
    try {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto prev_ns = last_poll_steady_ns_.load(std::memory_order_relaxed);
        if (!force_reload && prev_ns != 0 && (now_ns - prev_ns) < 250'000'000LL) {
            return false;
        }
        last_poll_steady_ns_.store(now_ns, std::memory_order_relaxed);

        const std::filesystem::path ini_path = directory / "ofxr_bridge.ini";
        std::error_code ec;
        if (!std::filesystem::exists(ini_path, ec) || ec) {
            if (force_reload) {
                last_file_write_ticks_.store(0, std::memory_order_relaxed);
                set_tuning_config(Phase2TuningConfig{});
            }
            return false;
        }
        const auto ftime = std::filesystem::last_write_time(ini_path, ec);
        if (ec) {
            return false;
        }
        const auto ticks = static_cast<std::int64_t>(ftime.time_since_epoch().count());
        if (!force_reload &&
            ticks == last_file_write_ticks_.load(std::memory_order_relaxed)) {
            return false;
        }
        last_file_write_ticks_.store(ticks, std::memory_order_relaxed);

        std::ifstream file(ini_path);
        if (!file.is_open()) {
            return false;
        }

        Phase2TuningConfig updated{};
        std::string line;
        while (std::getline(file, line)) {
            const auto comment_pos = line.find_first_of(";#");
            if (comment_pos != std::string::npos) {
                line.erase(comment_pos);
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = trim_ascii(std::string_view(line).substr(0, eq));
            const std::string val = trim_ascii(std::string_view(line).substr(eq + 1));
            if (key == "pose_centering_mode") {
                if (val == "target_b" || val == "legacy" || val == "0") {
                    updated.pose_centering = PoseCenteringMode::target_b_legacy;
                } else if (val == "extrapolated" || val == "asw" || val == "2") {
                    updated.pose_centering = PoseCenteringMode::extrapolated_asw;
                } else if (val == "meta" || val == "single_source" || val == "meta_asw" || val == "3") {
                    updated.pose_centering = PoseCenteringMode::meta_asw_single_source;
                } else {
                    updated.pose_centering = PoseCenteringMode::midpoint_slerp;
                }
            } else if (key == "vram_profile") {
                if (val == "legacy" || val == "0") {
                    updated.vram_profile = VramProfile::legacy_deep;
                } else if (val == "balanced" || val == "1") {
                    updated.vram_profile = VramProfile::balanced;
                } else {
                    updated.vram_profile = VramProfile::lean;
                }
            } else if (key == "low_latency_input" || key == "input_pacing_mode") {
                if (val == "0" || val == "false" || val == "off" || val == "legacy") {
                    updated.input_pacing = InputPacingMode::end_frame_legacy;
                } else {
                    updated.input_pacing = InputPacingMode::low_latency_clamp;
                }
            } else if (key == "keep_synthetic_depth") {
                updated.keep_synthetic_depth = (val != "0" && val != "false" && val != "off");
            } else if (key == "depth_edge_dilation") {
                updated.depth_edge_dilation = (val != "0" && val != "false" && val != "off");
            } else if (key == "static_motion_reject") {
                updated.static_motion_reject = (val != "0" && val != "false" && val != "off");
            } else if (key == "edge_feather_px" || key == "edge_feather_pixels") {
                try {
                    updated.edge_feather_pixels = std::clamp(std::stof(val), 0.0F, 64.0F);
                } catch (...) {
                }
            }
        }
        set_tuning_config(updated);
        return true;
    } catch (...) {
        return false;
    }
}

std::string ReprojectionTelemetryRegistry::format_diagnostic_summary() const {
    const VramLiveSnapshot vram = vram_snapshot();
    const KinematicFrameMetrics kin = latest_kinematic_metrics();
    const Phase2TuningConfig cfg = tuning_config();
    char buf[512]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "[ReproVR-Telemetry] VRAM=%.1f MB (PrivSwap=%.1f Staging=%.1f HistCol=%.1f HistDep=%.1f Synth=%.1f DLSS=%.1f) | "
        "ATW_Err=%.3f deg / %.2f mm | Jerk=%.3f deg | Border=%.1f px | Latency=%.1f ms | Mode=%u VRAMProf=%u",
        vram.total_megabytes(),
        static_cast<double>(vram.private_swapchains_bytes) / (1024.0 * 1024.0),
        static_cast<double>(vram.staging_textures_bytes) / (1024.0 * 1024.0),
        static_cast<double>(vram.history_color_bytes) / (1024.0 * 1024.0),
        static_cast<double>(vram.history_depth_bytes) / (1024.0 * 1024.0),
        static_cast<double>(vram.synthesizer_work_bytes) / (1024.0 * 1024.0),
        static_cast<double>(vram.dlss_guide_slots_bytes) / (1024.0 * 1024.0),
        static_cast<double>(kin.atw_pose_error_deg),
        static_cast<double>(kin.atw_position_error_mm),
        static_cast<double>(kin.inter_frame_angular_jerk_deg),
        static_cast<double>(kin.border_excursion_pixels),
        static_cast<double>(kin.pipeline_latency_ms),
        static_cast<unsigned>(cfg.pose_centering),
        static_cast<unsigned>(cfg.vram_profile));
    return std::string(buf);
}

}  // namespace xrfg
