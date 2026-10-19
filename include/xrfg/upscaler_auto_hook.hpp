#pragma once

#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>

namespace xrfg {

using NgxAbiResult = std::uint32_t;

struct NgxHandleAbi {
    unsigned int internal_id{};
};

// ABI-compatible public subset of NVSDK_NGX_Parameter (NVIDIA DLSS SDK).
// Allows zero-dependency extraction of MotionVectors, Depth, Output, Jitter,
// and MV.Scale from any in-process DLSS / Streamline evaluation.
struct NgxParameterAbi {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NgxAbiResult Get(const char*, unsigned long long*) const = 0;
    virtual NgxAbiResult Get(const char*, float*) const = 0;
    virtual NgxAbiResult Get(const char*, double*) const = 0;
    virtual NgxAbiResult Get(const char*, unsigned int*) const = 0;
    virtual NgxAbiResult Get(const char*, int*) const = 0;
    virtual NgxAbiResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NgxAbiResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NgxAbiResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

using PfnNgxProgressCallback = void (*)(float, bool&);

struct UpscalerAutoHookStats {
    std::uint64_t feature_creates_observed{};
    std::uint64_t evaluations_observed{};
    std::uint64_t publications_emitted{};
    std::uint64_t depth_resources_captured{};
    std::uint32_t hooked_module_count{};
};

// Registers the active D3D12 queue and device used by the OpenXR session so
// that intercepted NGX / DLSS evaluations on command lists can be published
// directly into the DlssMotionVectorRegistry without OptiScaler.
void set_upscaler_auto_hook_d3d12_context(
    ID3D12Device* device,
    ID3D12CommandQueue* queue) noexcept;

void clear_upscaler_auto_hook_d3d12_context() noexcept;

// Direct entry points invoked by the NGX D3D12 detours (and callable by unit tests).
void on_ngx_d3d12_create_feature(
    ID3D12GraphicsCommandList* command_list,
    unsigned int feature_id,
    const NgxParameterAbi* parameters,
    const NgxHandleAbi* handle) noexcept;

void on_ngx_d3d12_evaluate_feature(
    ID3D12GraphicsCommandList* command_list,
    const NgxHandleAbi* handle,
    const NgxParameterAbi* parameters) noexcept;

void on_ngx_d3d12_release_feature(const NgxHandleAbi* handle) noexcept;

// Scans loaded modules in the current process (_nvngx.dll, nvngx_dlss.dll,
// sl.interposer.dll, nvngx.dll) and installs IAT hooks for
// NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature / ReleaseFeature.
std::uint32_t install_upscaler_auto_hooks_in_process() noexcept;

[[nodiscard]] UpscalerAutoHookStats upscaler_auto_hook_statistics() noexcept;

void reset_upscaler_auto_hook_state_for_tests() noexcept;

}  // namespace xrfg
