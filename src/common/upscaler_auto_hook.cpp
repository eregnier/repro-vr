#include "xrfg/upscaler_auto_hook.hpp"

#include <windows.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kNgxCreateFlagLowResMv = 1U << 1;
constexpr std::uint32_t kNgxCreateFlagJitteredMv = 1U << 2;
constexpr std::uint32_t kNgxCreateFlagDepthInverted = 1U << 3;

struct FeatureContext {
    std::uint32_t create_flags{};
    bool has_create_flags{};
};

struct AutoHookState {
    std::mutex mutex;
    ComPtr<ID3D12Device> active_device;
    ComPtr<ID3D12CommandQueue> active_queue;
    std::unordered_map<std::uint64_t, FeatureContext> features;
    std::unordered_set<HMODULE> hooked_modules;
    UpscalerAutoHookStats stats{};
};

AutoHookState& hook_state() {
    static AutoHookState instance;
    return instance;
}

[[nodiscard]] bool ngx_ok(NgxAbiResult result) noexcept {
    return (result & 0xFFF00000U) != 0xBAD00000U;
}

[[nodiscard]] bool try_get_uint(
    const NgxParameterAbi* params,
    const char* name,
    std::uint32_t* out_value) noexcept {
    if (params == nullptr || name == nullptr || out_value == nullptr) {
        return false;
    }
    unsigned int uval = 0;
    if (ngx_ok(params->Get(name, &uval))) {
        *out_value = uval;
        return true;
    }
    int sval = 0;
    if (ngx_ok(params->Get(name, &sval))) {
        *out_value = static_cast<std::uint32_t>(sval);
        return true;
    }
    unsigned long long ullval = 0;
    if (ngx_ok(params->Get(name, &ullval))) {
        *out_value = static_cast<std::uint32_t>(ullval);
        return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t get_uint_or(
    const NgxParameterAbi* params,
    const char* name,
    std::uint32_t fallback = 0U) noexcept {
    std::uint32_t val = fallback;
    return try_get_uint(params, name, &val) ? val : fallback;
}

[[nodiscard]] float get_float_or(
    const NgxParameterAbi* params,
    const char* name,
    float fallback = 0.0F) noexcept {
    if (params == nullptr || name == nullptr) {
        return fallback;
    }
    float fval = fallback;
    if (ngx_ok(params->Get(name, &fval)) && std::isfinite(fval)) {
        return fval;
    }
    double dval = static_cast<double>(fallback);
    if (ngx_ok(params->Get(name, &dval)) && std::isfinite(dval)) {
        return static_cast<float>(dval);
    }
    return fallback;
}

[[nodiscard]] ID3D12Resource* get_d3d12_resource(
    const NgxParameterAbi* params,
    std::initializer_list<const char*> names) noexcept {
    if (params == nullptr) {
        return nullptr;
    }
    for (const char* name : names) {
        ID3D12Resource* resource = nullptr;
        if (ngx_ok(params->Get(name, &resource)) && resource != nullptr) {
            return resource;
        }
        void* raw_ptr = nullptr;
        if (ngx_ok(params->Get(name, &raw_ptr)) && raw_ptr != nullptr) {
            return static_cast<ID3D12Resource*>(raw_ptr);
        }
    }
    return nullptr;
}

[[nodiscard]] std::uint64_t handle_stream_key(const NgxHandleAbi* handle) noexcept {
    if (handle == nullptr) {
        return 1ULL;
    }
    if (handle->internal_id != 0U) {
        return static_cast<std::uint64_t>(handle->internal_id);
    }
    return reinterpret_cast<std::uint64_t>(handle);
}

using PfnNgxD3D12CreateFeature = NgxAbiResult(WINAPI*)(
    ID3D12GraphicsCommandList*,
    unsigned int,
    NgxParameterAbi*,
    NgxHandleAbi**);

using PfnNgxD3D12EvaluateFeature = NgxAbiResult(WINAPI*)(
    ID3D12GraphicsCommandList*,
    const NgxHandleAbi*,
    const NgxParameterAbi*,
    PfnNgxProgressCallback);

using PfnNgxD3D12ReleaseFeature = NgxAbiResult(WINAPI*)(NgxHandleAbi*);

PfnNgxD3D12CreateFeature g_real_create_feature = nullptr;
PfnNgxD3D12EvaluateFeature g_real_evaluate_feature = nullptr;
PfnNgxD3D12ReleaseFeature g_real_release_feature = nullptr;

NgxAbiResult WINAPI detour_ngx_d3d12_create_feature(
    ID3D12GraphicsCommandList* cmd_list,
    unsigned int feature_id,
    NgxParameterAbi* parameters,
    NgxHandleAbi** out_handle) noexcept {
    const NgxAbiResult result = g_real_create_feature != nullptr
        ? g_real_create_feature(cmd_list, feature_id, parameters, out_handle)
        : 0xBAD00000U;
    if (ngx_ok(result) && out_handle != nullptr && *out_handle != nullptr) {
        on_ngx_d3d12_create_feature(cmd_list, feature_id, parameters, *out_handle);
    }
    return result;
}

NgxAbiResult WINAPI detour_ngx_d3d12_evaluate_feature(
    ID3D12GraphicsCommandList* cmd_list,
    const NgxHandleAbi* handle,
    const NgxParameterAbi* parameters,
    PfnNgxProgressCallback callback) noexcept {
    on_ngx_d3d12_evaluate_feature(cmd_list, handle, parameters);
    return g_real_evaluate_feature != nullptr
        ? g_real_evaluate_feature(cmd_list, handle, parameters, callback)
        : 0xBAD00000U;
}

NgxAbiResult WINAPI detour_ngx_d3d12_release_feature(
    NgxHandleAbi* handle) noexcept {
    on_ngx_d3d12_release_feature(handle);
    return g_real_release_feature != nullptr
        ? g_real_release_feature(handle)
        : 0xBAD00000U;
}

bool patch_iat_entry_in_module(
    HMODULE module,
    const char* function_name,
    void* replacement,
    void** original_out) noexcept {
    if (module == nullptr || function_name == nullptr || replacement == nullptr) {
        return false;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY& import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir.VirtualAddress == 0 || import_dir.Size == 0) {
        return false;
    }

    bool patched = false;
    auto* import_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + import_dir.VirtualAddress);
    for (; import_desc->Name != 0; ++import_desc) {
        if (import_desc->OriginalFirstThunk == 0 || import_desc->FirstThunk == 0) {
            continue;
        }
        auto* orig_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + import_desc->OriginalFirstThunk);
        auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + import_desc->FirstThunk);
        for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import_by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + orig_thunk->u1.AddressOfData);
            if (std::strcmp(import_by_name->Name, function_name) != 0) {
                continue;
            }
            DWORD old_protect = 0;
            if (VirtualProtect(
                    &iat_thunk->u1.Function,
                    sizeof(ULONG_PTR),
                    PAGE_READWRITE,
                    &old_protect)) {
                void* current_fn =
                    reinterpret_cast<void*>(iat_thunk->u1.Function);
                if (current_fn != replacement && original_out != nullptr &&
                    *original_out == nullptr) {
                    *original_out = current_fn;
                }
                iat_thunk->u1.Function =
                    reinterpret_cast<ULONG_PTR>(replacement);
                VirtualProtect(
                    &iat_thunk->u1.Function,
                    sizeof(ULONG_PTR),
                    old_protect,
                    &old_protect);
                patched = true;
            }
        }
    }
    return patched;
}

}  // namespace

void set_upscaler_auto_hook_d3d12_context(
    ID3D12Device* device,
    ID3D12CommandQueue* queue) noexcept {
    try {
        configure_dlss_motion_vector_tracking(device != nullptr && queue != nullptr);
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);
        state.active_device = device;
        state.active_queue = queue;
    } catch (...) {
    }
}

void clear_upscaler_auto_hook_d3d12_context() noexcept {
    try {
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);
        state.active_device.Reset();
        state.active_queue.Reset();
    } catch (...) {
    }
}

void on_ngx_d3d12_create_feature(
    ID3D12GraphicsCommandList* command_list,
    unsigned int feature_id,
    const NgxParameterAbi* parameters,
    const NgxHandleAbi* handle) noexcept {
    static_cast<void>(command_list);
    static_cast<void>(feature_id);
    if (parameters == nullptr || handle == nullptr) {
        return;
    }
    try {
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);
        ++state.stats.feature_creates_observed;
        std::uint32_t create_flags = 0;
        const bool has_flags = try_get_uint(
            parameters,
            "DLSS.Feature.Create.Flags",
            &create_flags);
        const std::uint64_t key = handle_stream_key(handle);
        state.features[key] = FeatureContext{create_flags, has_flags};
    } catch (...) {
    }
}

void on_ngx_d3d12_evaluate_feature(
    ID3D12GraphicsCommandList* command_list,
    const NgxHandleAbi* handle,
    const NgxParameterAbi* parameters) noexcept {
    if (command_list == nullptr || parameters == nullptr) {
        return;
    }
    try {
        ComPtr<ID3D12Device> verified_device;
        ComPtr<ID3D12CommandQueue> producer_queue;
        FeatureContext feature_ctx{};
        const std::uint64_t stream_key = handle_stream_key(handle);

        {
            auto& state = hook_state();
            std::scoped_lock lock(state.mutex);
            ++state.stats.evaluations_observed;
            verified_device = state.active_device;
            producer_queue = state.active_queue;
            const auto it = state.features.find(stream_key);
            if (it != state.features.end()) {
                feature_ctx = it->second;
            }
        }

        if (!producer_queue) {
            return;
        }

        ID3D12Resource* const output = get_d3d12_resource(
            parameters,
            {"Output", "DLSS.Output", "Color.Output"});
        ID3D12Resource* const motion_vectors = get_d3d12_resource(
            parameters,
            {"MotionVectors", "DLSS.Input.MotionVectors", "MV"});
        if (output == nullptr || motion_vectors == nullptr) {
            return;
        }
        ID3D12Resource* const depth = get_d3d12_resource(
            parameters,
            {"Depth", "DLSS.Input.Depth"});

        const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
        const D3D12_RESOURCE_DESC mv_desc = motion_vectors->GetDesc();

        std::uint32_t render_w = get_uint_or(
            parameters,
            "DLSS.Render.Subrect.Dimensions.Width",
            get_uint_or(parameters, "Width", static_cast<std::uint32_t>(mv_desc.Width)));
        std::uint32_t render_h = get_uint_or(
            parameters,
            "DLSS.Render.Subrect.Dimensions.Height",
            get_uint_or(parameters, "Height", mv_desc.Height));
        std::uint32_t out_w = get_uint_or(
            parameters,
            "OutWidth",
            static_cast<std::uint32_t>(output_desc.Width));
        std::uint32_t out_h = get_uint_or(
            parameters,
            "OutHeight",
            output_desc.Height);

        std::uint32_t create_flags = feature_ctx.create_flags;
        if (!feature_ctx.has_create_flags) {
            create_flags = get_uint_or(
                parameters,
                "DLSS.Feature.Create.Flags",
                kNgxCreateFlagLowResMv | kNgxCreateFlagDepthInverted);
        }

        const bool low_res_mv = (create_flags & kNgxCreateFlagLowResMv) != 0U;
        const std::uint32_t mv_x =
            get_uint_or(parameters, "DLSS.Input.MV.Subrect.Base.X", 0U);
        const std::uint32_t mv_y =
            get_uint_or(parameters, "DLSS.Input.MV.Subrect.Base.Y", 0U);
        const std::uint32_t mv_w = std::min<std::uint32_t>(
            low_res_mv ? render_w : out_w,
            static_cast<std::uint32_t>(mv_desc.Width) -
                std::min<std::uint32_t>(mv_x, static_cast<std::uint32_t>(mv_desc.Width)));
        const std::uint32_t mv_h = std::min<std::uint32_t>(
            low_res_mv ? render_h : out_h,
            mv_desc.Height - std::min<std::uint32_t>(mv_y, mv_desc.Height));

        const std::uint32_t out_x =
            get_uint_or(parameters, "DLSS.Output.Subrect.Base.X", 0U);
        const std::uint32_t out_y =
            get_uint_or(parameters, "DLSS.Output.Subrect.Base.Y", 0U);

        DlssMotionVectorPublication pub{};
        pub.stream = stream_key;
        pub.output = output;
        pub.motion_vectors = motion_vectors;
        pub.producer_queue = producer_queue.Get();
        pub.producer_command_list = command_list;
        pub.verified_producer_device = verified_device.Get();
        pub.output_x = out_x;
        pub.output_y = out_y;
        pub.output_width = out_w;
        pub.output_height = out_h;
        pub.motion_x = mv_x;
        pub.motion_y = mv_y;
        pub.motion_width = mv_w;
        pub.motion_height = mv_h;
        pub.scale_x = get_float_or(parameters, "MV.Scale.X", 1.0F);
        pub.scale_y = get_float_or(parameters, "MV.Scale.Y", 1.0F);
        pub.jitter_x = get_float_or(parameters, "Jitter.Offset.X", 0.0F);
        pub.jitter_y = get_float_or(parameters, "Jitter.Offset.Y", 0.0F);
        pub.jittered = (create_flags & kNgxCreateFlagJitteredMv) != 0U ||
            (std::abs(pub.jitter_x) > 1.0e-5F || std::abs(pub.jitter_y) > 1.0e-5F);
        pub.reset = get_uint_or(parameters, "Reset", 0U) != 0U;
        pub.resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (depth != nullptr) {
            const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
            const std::uint32_t depth_x =
                get_uint_or(parameters, "DLSS.Input.Depth.Subrect.Base.X", 0U);
            const std::uint32_t depth_y =
                get_uint_or(parameters, "DLSS.Input.Depth.Subrect.Base.Y", 0U);
            pub.depth = depth;
            pub.depth_x = depth_x;
            pub.depth_y = depth_y;
            pub.depth_width = std::min<std::uint32_t>(
                render_w,
                static_cast<std::uint32_t>(depth_desc.Width) -
                    std::min<std::uint32_t>(depth_x, static_cast<std::uint32_t>(depth_desc.Width)));
            pub.depth_height = std::min<std::uint32_t>(
                render_h,
                depth_desc.Height - std::min<std::uint32_t>(depth_y, depth_desc.Height));
            pub.depth_resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            pub.depth_inverted = (create_flags & kNgxCreateFlagDepthInverted) != 0U ||
                get_uint_or(parameters, "DepthInverted", 1U) != 0U;
            pub.depth_infinite = get_uint_or(parameters, "CameraFarInfinite", 0U) != 0U;
            pub.camera_near = std::max(get_float_or(parameters, "CameraNear", 0.1F), 1.0e-4F);
            pub.camera_far = std::max(
                get_float_or(parameters, "CameraFar", 1000.0F),
                pub.camera_near + 1.0F);
            pub.frame_time_delta_ms = std::max(
                get_float_or(parameters, "FrameTimeDeltaInMsec", 11.1111F),
                0.1F);
        }

        publish_dlss_motion_vectors(pub);

        {
            auto& state = hook_state();
            std::scoped_lock lock(state.mutex);
            ++state.stats.publications_emitted;
            if (depth != nullptr) {
                ++state.stats.depth_resources_captured;
            }
        }
    } catch (...) {
    }
}

void on_ngx_d3d12_release_feature(const NgxHandleAbi* handle) noexcept {
    if (handle == nullptr) {
        return;
    }
    try {
        const std::uint64_t key = handle_stream_key(handle);
        retire_dlss_motion_vector_stream(key);
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);
        state.features.erase(key);
    } catch (...) {
    }
}

std::uint32_t install_upscaler_auto_hooks_in_process() noexcept {
    try {
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);

        std::array<HMODULE, 256> modules{};
        DWORD bytes_needed = 0;
        if (!EnumProcessModules(
                GetCurrentProcess(),
                modules.data(),
                static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                &bytes_needed)) {
            return state.stats.hooked_module_count;
        }
        const std::size_t count = std::min<std::size_t>(
            modules.size(),
            bytes_needed / sizeof(HMODULE));

        for (std::size_t i = 0; i < count; ++i) {
            HMODULE mod = modules[i];
            if (mod == nullptr || state.hooked_modules.contains(mod)) {
                continue;
            }
            bool any_patched = false;
            any_patched |= patch_iat_entry_in_module(
                mod,
                "NVSDK_NGX_D3D12_CreateFeature",
                reinterpret_cast<void*>(&detour_ngx_d3d12_create_feature),
                reinterpret_cast<void**>(&g_real_create_feature));
            any_patched |= patch_iat_entry_in_module(
                mod,
                "NVSDK_NGX_D3D12_EvaluateFeature",
                reinterpret_cast<void*>(&detour_ngx_d3d12_evaluate_feature),
                reinterpret_cast<void**>(&g_real_evaluate_feature));
            any_patched |= patch_iat_entry_in_module(
                mod,
                "NVSDK_NGX_D3D12_ReleaseFeature",
                reinterpret_cast<void*>(&detour_ngx_d3d12_release_feature),
                reinterpret_cast<void**>(&g_real_release_feature));
            if (any_patched) {
                state.hooked_modules.insert(mod);
                ++state.stats.hooked_module_count;
            }
        }
        return state.stats.hooked_module_count;
    } catch (...) {
        return 0U;
    }
}

UpscalerAutoHookStats upscaler_auto_hook_statistics() noexcept {
    try {
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);
        return state.stats;
    } catch (...) {
        return {};
    }
}

void reset_upscaler_auto_hook_state_for_tests() noexcept {
    try {
        auto& state = hook_state();
        std::scoped_lock lock(state.mutex);
        state.active_device.Reset();
        state.active_queue.Reset();
        state.features.clear();
        state.hooked_modules.clear();
        state.stats = {};
    } catch (...) {
    }
}

}  // namespace xrfg
