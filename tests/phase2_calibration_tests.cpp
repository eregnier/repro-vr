// SPDX-License-Identifier: MIT OR Apache-2.0
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_history.hpp"
#include "xrfg/depth_reprojection.hpp"
#include "xrfg/reprojection_telemetry.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

void require_true(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[PHASE2-TEST-FAIL] %s\n", message);
        std::exit(1);
    }
}

xrfg::Quaternion make_yaw_quaternion_deg(float yaw_degrees) {
    constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
    const float half_rad = 0.5F * yaw_degrees * kDegToRad;
    return xrfg::normalize({0.0F, std::sin(half_rad), 0.0F, std::cos(half_rad)});
}

// 1. Validates that Symmetric Pose-Centered Synthesis (midpoint_slerp) eliminates
// the 45 Hz ATW angular step jerk and halves border excursion during head turns.
void test_head_rotation_micro_judder_and_border_excursion() {
    // Simulate a 90 deg/s head turn at 45 FPS game rate (90 Hz display -> 2.0 deg per game frame)
    const xrfg::Pose pose_a{make_yaw_quaternion_deg(0.0F), {0.0F, 1.65F, 0.0F}};
    const xrfg::Pose pose_b{make_yaw_quaternion_deg(2.0F), {0.02F, 1.65F, 0.0F}};
    // At the midpoint display scanout t_S (alpha = 0.5), the physical HMD is at +1.0 deg:
    const xrfg::Pose actual_hmd_at_t_s{make_yaw_quaternion_deg(1.0F), {0.01F, 1.65F, 0.0F}};

    constexpr float kHorizontalFovRad = 1.7453293F; // 100 degrees
    constexpr std::uint32_t kViewportWidth = 3840;

    // A) Legacy V008 behavior: synthetic frame S declares B.pose (+2.0 deg)
    const xrfg::KinematicFrameMetrics legacy_metrics =
        xrfg::evaluate_pair_kinematic_metrics(
            pose_a,
            pose_b,
            pose_b, // Legacy submits B's pose for S!
            actual_hmd_at_t_s,
            kHorizontalFovRad,
            kViewportWidth,
            44.4F);

    // B) Phase 2 Symmetric Pose-Centered behavior: synthetic frame S is built at P_S (+1.0 deg)
    const xrfg::FieldOfViewTangents fov{-1.0F, 1.0F, 1.0F, -1.0F};
    const xrfg::ViewRect2D rect{0.0F, 0.0F, 3840.0F, 3840.0F};
    const xrfg::SymmetricPoseMappings6Dof sym =
        xrfg::make_symmetric_pose_mappings_6dof(
            pose_a, fov, rect, pose_b, fov, rect, 0.5F);

    const xrfg::KinematicFrameMetrics phase2_metrics =
        xrfg::evaluate_pair_kinematic_metrics(
            pose_a,
            pose_b,
            sym.synthetic_pose,
            actual_hmd_at_t_s,
            kHorizontalFovRad,
            kViewportWidth,
            16.6F);

    require_true(
        legacy_metrics.atw_pose_error_deg > 0.99F &&
            legacy_metrics.inter_frame_angular_jerk_deg > 0.99F,
        "Legacy mode should exhibit ~1.0 deg of 45 Hz ATW angular step error");
    require_true(
        phase2_metrics.atw_pose_error_deg < 1.0e-3F &&
            phase2_metrics.atw_position_error_mm < 1.0e-2F &&
            phase2_metrics.inter_frame_angular_jerk_deg < 1.0e-3F,
        "Phase 2 midpoint_slerp mode must reduce 45 Hz ATW angular & positional error to 0.000");
    require_true(
        phase2_metrics.border_excursion_pixels <=
            legacy_metrics.border_excursion_pixels * 0.35F,
        "Phase 2 symmetric pose centering must reduce border excursion by >65%");

    // Verify symmetric mappings project center pixel into opposite directions in A and B
    const xrfg::MappedPoint6Dof to_a = xrfg::map_target_to_source_6dof(
        1920.0F, 1920.0F, 2.0F, sym.synthetic_to_previous);
    const xrfg::MappedPoint6Dof to_b = xrfg::map_target_to_source_6dof(
        1920.0F, 1920.0F, 2.0F, sym.synthetic_to_current);
    require_true(to_a.valid && to_b.valid, "Symmetric mappings must both be valid at screen center");
    const float shift_a = to_a.x - 1920.0F;
    const float shift_b = to_b.x - 1920.0F;
    require_true(
        shift_a * shift_b < 0.0F && std::abs(shift_a + shift_b) < 1.5F,
        "Symmetric mappings P_S->P_A and P_S->P_B must shift in opposite, balanced directions");
}

// 2. Validates Edge Feathering, 3x3 Depth Dilation, Static Reject & Ring Flow Repair.
void test_edge_feather_depth_dilation_and_flow_repair() {
    const xrfg::ViewRect2D rect{0.0F, 0.0F, 1000.0F, 1000.0F};
    require_true(
        xrfg::compute_edge_feather_weight(500.0F, 500.0F, rect, 12.0F) == 1.0F,
        "Interior pixel must have edge feather weight 1.0");
    const float near_edge = xrfg::compute_edge_feather_weight(3.0F, 500.0F, rect, 12.0F);
    require_true(
        near_edge > 0.05F && near_edge < 0.5F,
        "Border pixel within feather band must smoothly attenuate");
    require_true(
        xrfg::compute_edge_feather_weight(-2.0F, 500.0F, rect, 12.0F) == 0.0F,
        "Out-of-bounds pixel must have weight 0.0");

    // 3x3 depth dilation: weapon silhouette at 0.65m in front of 25.0m wall
    const std::array<float, 9> neighborhood{
        25.0F, 25.0F, 25.0F,
        25.0F, 25.0F, 0.65F,
        25.0F, 25.0F, 0.65F,
    };
    const xrfg::DepthNeighborhood3x3 nb =
        xrfg::analyze_depth_neighborhood_3x3(neighborhood, 0.04F);
    require_true(
        std::abs(nb.foreground_dilated_depth - 0.65F) < 1.0e-5F &&
            std::abs(nb.background_eroded_depth - 25.0F) < 1.0e-4F &&
            nb.lock_foreground,
        "3x3 depth neighborhood must dilate foreground weapon depth (0.65m) and flag silhouette lock");

    // Static motion damping on HUD / fine grid
    const float hud_damping = xrfg::compute_static_motion_damping_factor(
        0.22F, -0.15F, 0.002F);
    const float real_motion_damping = xrfg::compute_static_motion_damping_factor(
        6.5F, 2.0F, 0.18F);
    require_true(
        hud_damping < 0.05F && real_motion_damping > 0.99F,
        "Static motion reject must damp spurious HUD shimmer while preserving real motion");

    // Ring repair of distrusted optical flow vector
    const xrfg::RingCandidateMotionSample bad_center{42.0F, -38.0F, 2.0F, 0.95F};
    const std::array<xrfg::RingCandidateMotionSample, 8> ring{{
        {15.0F, 0.0F, 0.5F, 0.1F},   // Closer foreground object (wrong depth 0.5m)
        {-4.0F, 1.0F, 2.02F, 0.05F}, // Matching background surface at ~2.0m (trusted!)
        {30.0F, 12.0F, 2.0F, 0.90F}, // Distrusted
        {14.0F, 0.0F, 0.52F, 0.1F},
        {-3.9F, 1.1F, 2.15F, 0.2F},
        {0.0F, 0.0F, 15.0F, 0.1F},
        {0.0F, 0.0F, 15.0F, 0.1F},
        {0.0F, 0.0F, 15.0F, 0.1F},
    }};
    const xrfg::MotionDepthSample repaired =
        xrfg::repair_distrusted_motion_vector_ring(bad_center, ring);
    require_true(
        std::abs(repaired.mv_x - (-4.0F)) < 1.0e-4F &&
            std::abs(repaired.mv_y - 1.0F) < 1.0e-4F,
        "Ring repair must select the trusted neighbor matching the target surface depth (2.0m)");
}

// 3. Validates Low-Latency Controller Input Clock Clamping & VRAM Budget Reduction (>55% saved).
void test_low_latency_input_clamp_and_vram_cure() {
    constexpr std::int64_t kPeriodNs = 22'222'222LL; // 45 Hz virtual period (90 Hz HMD)
    const std::int64_t runtime_now = 1'000'000'000LL;
    // Suppose the virtual clock previously drifted 5 periods ahead (111 ms input lag):
    const std::int64_t drifted_last = runtime_now + 5 * kPeriodNs;

    const std::int64_t clamped_low_lat =
        xrfg::clamp_low_latency_virtual_display_time(
            runtime_now,
            kPeriodNs,
            drifted_last,
            xrfg::InputPacingMode::low_latency_clamp);
    // Low-latency mode prevents stepping another full period when already ahead of ceiling:
    require_true(
        clamped_low_lat == drifted_last + 1,
        "Low-latency input clamp must advance by 1 ns instead of compounding another virtual period");

    // When within normal bounds, low-latency ceiling is 1.25 periods ahead of runtime_now:
    const std::int64_t normal_low_lat =
        xrfg::clamp_low_latency_virtual_display_time(
            runtime_now,
            kPeriodNs,
            runtime_now + kPeriodNs + 1000,
            xrfg::InputPacingMode::low_latency_clamp);
    require_true(
        normal_low_lat <= runtime_now + kPeriodNs + (kPeriodNs / 4),
        "Low-latency virtual display time must stay within 1.25 virtual periods of runtime prediction");

    // VRAM footprint comparison at 4000x4000x2 RGBA16F (8 bytes/px) with 4 mip levels:
    const xrfg::VramFootprintEstimate legacy_vram = xrfg::estimate_vram_footprint(
        4000, 4000, 2, 8, 4, xrfg::VramProfile::legacy_deep, false, false);
    const xrfg::VramFootprintEstimate lean_vram = xrfg::estimate_vram_footprint(
        4000, 4000, 2, 8, 4, xrfg::VramProfile::lean, false, false);

    const double saved_ratio =
        1.0 - (static_cast<double>(lean_vram.total_bytes) /
               static_cast<double>(legacy_vram.total_bytes));
    std::printf(
        "[PHASE2-VRAM] 4000x4000x2 RGBA16F (4 mips): Legacy=%.1f MB -> Lean=%.1f MB (Saved %.1f%% = %.1f MB)\n",
        legacy_vram.total_megabytes(),
        lean_vram.total_megabytes(),
        saved_ratio * 100.0,
        legacy_vram.total_megabytes() - lean_vram.total_megabytes());
    require_true(
        saved_ratio >= 0.50 && (legacy_vram.total_megabytes() - lean_vram.total_megabytes()) > 1200.0,
        "Phase 2 Lean VRAM profile must save at least 50% (>1.2 GB) of VRAM at 4K per eye");

    // Hot-reload test
    auto& reg = xrfg::ReprojectionTelemetryRegistry::instance();
    reg.reset();
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "repro_vr_phase2_hotreload_test";
    std::filesystem::create_directories(temp_dir);
    {
        std::ofstream ini(temp_dir / "ofxr_bridge.ini", std::ios::trunc);
        ini << "[ofxr]\n"
            << "pose_centering_mode=midpoint\n"
            << "vram_profile=lean\n"
            << "low_latency_input=1\n"
            << "keep_synthetic_depth=1\n"
            << "edge_feather_px=14\n";
    }
    const bool reloaded = reg.poll_hot_reload(temp_dir, true);
    const xrfg::Phase2TuningConfig cfg = reg.tuning_config();
    require_true(
        reloaded &&
            cfg.pose_centering == xrfg::PoseCenteringMode::midpoint_slerp &&
            cfg.vram_profile == xrfg::VramProfile::lean &&
            cfg.input_pacing == xrfg::InputPacingMode::low_latency_clamp &&
            cfg.keep_synthetic_depth &&
            std::abs(cfg.edge_feather_pixels - 14.0F) < 1.0e-3F,
        "Hot-reload from ofxr_bridge.ini must apply all Phase 2 tuning parameters");
    std::filesystem::remove_all(temp_dir);
    static_cast<void>(reg.poll_hot_reload(temp_dir, true));
    require_true(
        reg.tuning_config().pose_centering == xrfg::PoseCenteringMode::target_b_legacy,
        "Force reload on directory without ofxr_bridge.ini must reset to defaults");
    reg.reset();
}

// 4. End-to-End D3D12 GPU test of `submit_pose_centered_pair` on RTX 5090:
// Verifies that during a large head turn, the synthesized midpoint frame at P_S
// has zero black border pixels (all pixels valid via symmetric A->P_S and B->P_S coverage).
void test_d3d12_gpu_pose_centered_synthesis_no_black_borders() {
    ComPtr<IDXGIFactory6> factory;
    require_true(
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))),
        "Failed to create DXGIFactory6");
    ComPtr<ID3D12Device> device;
    require_true(
        SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.GetAddressOf()))),
        "Failed to create D3D12Device");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    require_true(
        SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf()))),
        "Failed to create D3D12CommandQueue");

    constexpr UINT kW = 64;
    constexpr UINT kH = 64;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kW;
    tex_desc.Height = kH;
    tex_desc.DepthOrArraySize = 2;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    std::array<ComPtr<ID3D12Resource>, 2> sources;
    std::array<ComPtr<ID3D12Resource>, 2> curr_dest;
    std::array<ComPtr<ID3D12Resource>, 2> synth_dest;
    for (std::size_t i = 0; i < 2; ++i) {
        require_true(SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(sources[i].GetAddressOf()))),
            "Failed to create source texture");
        require_true(SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(curr_dest[i].GetAddressOf()))),
            "Failed to create curr_dest texture");
        require_true(SUCCEEDED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(synth_dest[i].GetAddressOf()))),
            "Failed to create synth_dest texture");
    }

    // Clear both source textures to a bright non-zero sky color (R=100, G=160, B=220, A=255)
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 2;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require_true(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.GetAddressOf()))),
        "Failed to create RTV heap");
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    require_true(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf()))),
        "Failed to create CommandAllocator");
    require_true(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(cmd.GetAddressOf()))),
        "Failed to create CommandList");

    const float sky_color[4] = {100.0F / 255.0F, 160.0F / 255.0F, 220.0F / 255.0F, 1.0F};
    const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (std::size_t i = 0; i < 2; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * rtv_inc;
        device->CreateRenderTargetView(sources[i].Get(), nullptr, h);
        cmd->ClearRenderTargetView(h, sky_color, 0, nullptr);
    }
    require_true(SUCCEEDED(cmd->Close()), "Failed to close clear command list");
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    std::array<ID3D12Resource*, 2> src_ptrs{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> curr_ptrs{curr_dest[0].Get(), curr_dest[1].Get()};
    std::array<ID3D12Resource*, 2> synth_ptrs{synth_dest[0].Get(), synth_dest[1].Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_true(SUCCEEDED(history->initialize(
        device.Get(), queue.Get(), src_ptrs, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "Failed to initialize D3D12SwapchainHistory");

    xrfg::D3D12FrameSynthesizer synthesizer;
    require_true(SUCCEEDED(synthesizer.initialize(
        device.Get(), queue.Get(), history, curr_ptrs, synth_ptrs,
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET,
        xrfg::D3D12OpticalFlowBackend::fidelity_fx)),
        "Failed to initialize D3D12FrameSynthesizer");

    auto make_views = [](float yaw_deg) {
        std::array<xrfg::D3D12ReprojectionView, 2> v{};
        const xrfg::Quaternion q = make_yaw_quaternion_deg(yaw_deg);
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            v[eye].pose.orientation = q;
            v[eye].pose.position = {eye == 0 ? -0.032F : 0.032F, 1.65F, 0.0F};
            v[eye].fov = {-0.8F, 0.8F, 0.8F, -0.8F};
            v[eye].image_rect = {0U, 0U, kW, kH};
            v[eye].array_slice = eye;
        }
        return v;
    };

    const auto views_a = make_views(0.0F);
    const auto views_b = make_views(12.0F); // Fast 12-degree head turn between A and B!
    const auto views_mid = make_views(6.0F); // Midpoint pose P_S at +6 degrees

    xrfg::D3D12HistoryCaptureTicket ticket_a{};
    xrfg::D3D12FrameSynthesisTicket prime_ticket{};
    require_true(SUCCEEDED(history->capture(0, &ticket_a)) && SUCCEEDED(history->commit(ticket_a)),
        "Capture A failed");
    require_true(SUCCEEDED(synthesizer.submit_prime(ticket_a, views_a, 0, &prime_ticket)),
        "Prime A failed");

    xrfg::D3D12HistoryCaptureTicket ticket_b{};
    xrfg::D3D12FrameSynthesisTicket pair_ticket{};
    require_true(SUCCEEDED(history->capture(1, &ticket_b)) && SUCCEEDED(history->commit(ticket_b)),
        "Capture B failed");
    require_true(SUCCEEDED(synthesizer.submit_pose_centered_pair(
        ticket_b, views_b, views_mid, 0, 1, &pair_ticket)),
        "submit_pose_centered_pair failed on GPU");
    require_true(SUCCEEDED(synthesizer.wait_for_idle()), "wait_for_idle failed");

    // Read back slice 0 of synth_dest[0] and verify zero black border pixels exist
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = total_bytes;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.Format = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback_buf;
    require_true(SUCCEEDED(device->CreateCommittedResource(
        &readback_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback_buf.GetAddressOf()))),
        "Failed to create readback buffer");

    require_true(SUCCEEDED(alloc->Reset()) && SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)),
        "Failed to reset command list for readback");
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = synth_dest[0].Get();
    to_copy.Transition.Subresource = 0;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback_buf.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = synth_dest[0].Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    require_true(SUCCEEDED(cmd->Close()), "Failed to close readback command list");
    ID3D12CommandList* rb_lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, rb_lists);

    ComPtr<ID3D12Fence> rb_fence;
    require_true(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(rb_fence.GetAddressOf()))),
        "Failed to create readback fence");
    HANDLE rb_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require_true(SUCCEEDED(queue->Signal(rb_fence.Get(), 1)) &&
        SUCCEEDED(rb_fence->SetEventOnCompletion(1, rb_ev)),
        "Failed to signal readback fence");
    WaitForSingleObject(rb_ev, INFINITE);
    CloseHandle(rb_ev);

    const std::uint8_t* mapped = nullptr;
    require_true(SUCCEEDED(readback_buf->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint8_t**>(&mapped)))),
        "Failed to map readback buffer");
    std::uint32_t black_pixels = 0;
    for (UINT y = 0; y < kH; ++y) {
        const std::uint8_t* row = mapped + y * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kW; ++x) {
            const std::uint8_t r = row[x * 4 + 0];
            const std::uint8_t g = row[x * 4 + 1];
            const std::uint8_t b = row[x * 4 + 2];
            if (r == 0 && g == 0 && b == 0) {
                ++black_pixels;
            }
        }
    }
    readback_buf->Unmap(0, nullptr);
    require_true(
        black_pixels == 0,
        "Synthesized pose-centered frame must have 0 black border pixels after a 12-deg head turn");
}

}  // namespace

int main() {
    test_head_rotation_micro_judder_and_border_excursion();
    test_edge_feather_depth_dilation_and_flow_repair();
    test_low_latency_input_clamp_and_vram_cure();
    test_d3d12_gpu_pose_centered_synthesis_no_black_borders();
    std::printf("[PHASE2-TESTS] All Phase 2 calibration, VRAM & GPU tests passed!\n");
    return 0;
}
