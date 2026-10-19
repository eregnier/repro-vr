#include "xrfg/upscaler_auto_hook.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

using Microsoft::WRL::ComPtr;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class MockNgxParameter final : public xrfg::NgxParameterAbi {
public:
    void Set(const char* name, unsigned long long value) override {
        ull_values_[name] = value;
    }
    void Set(const char* name, float value) override {
        float_values_[name] = value;
    }
    void Set(const char* name, double value) override {
        double_values_[name] = value;
    }
    void Set(const char* name, unsigned int value) override {
        uint_values_[name] = value;
    }
    void Set(const char* name, int value) override {
        int_values_[name] = value;
    }
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char* name, ID3D12Resource* value) override {
        d3d12_values_[name] = value;
    }
    void Set(const char* name, void* value) override {
        ptr_values_[name] = value;
    }

    xrfg::NgxAbiResult Get(const char* name, unsigned long long* out) const override {
        if (auto it = ull_values_.find(name); it != ull_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char* name, float* out) const override {
        if (auto it = float_values_.find(name); it != float_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char* name, double* out) const override {
        if (auto it = double_values_.find(name); it != double_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char* name, unsigned int* out) const override {
        if (auto it = uint_values_.find(name); it != uint_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char* name, int* out) const override {
        if (auto it = int_values_.find(name); it != int_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char*, ID3D11Resource**) const override {
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char* name, ID3D12Resource** out) const override {
        if (auto it = d3d12_values_.find(name); it != d3d12_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    xrfg::NgxAbiResult Get(const char* name, void** out) const override {
        if (auto it = ptr_values_.find(name); it != ptr_values_.end() && out != nullptr) {
            *out = it->second;
            return 1U;
        }
        return 0xBAD00001U;
    }
    void Reset() override {
        ull_values_.clear();
        float_values_.clear();
        double_values_.clear();
        uint_values_.clear();
        int_values_.clear();
        d3d12_values_.clear();
        ptr_values_.clear();
    }

private:
    std::unordered_map<std::string, unsigned long long> ull_values_;
    std::unordered_map<std::string, float> float_values_;
    std::unordered_map<std::string, double> double_values_;
    std::unordered_map<std::string, unsigned int> uint_values_;
    std::unordered_map<std::string, int> int_values_;
    std::unordered_map<std::string, ID3D12Resource*> d3d12_values_;
    std::unordered_map<std::string, void*> ptr_values_;
};

ComPtr<ID3D12Resource> create_texture(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT16 array_size = 1U) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = array_size;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> resource;
    require(
        SUCCEEDED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&resource))),
        "CreateCommittedResource must succeed");
    return resource;
}

}  // namespace

int main() {
    xrfg::reset_upscaler_auto_hook_state_for_tests();
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();

    ComPtr<IDXGIFactory4> factory;
    require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1 failed");

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        ComPtr<IDXGIAdapter> warp;
        require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "EnumWarpAdapter failed");
        require(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP D3D12CreateDevice failed");
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    require(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))), "CreateCommandQueue failed");

    ComPtr<ID3D12CommandAllocator> allocator;
    require(
        SUCCEEDED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))),
        "CreateCommandAllocator failed");
    ComPtr<ID3D12GraphicsCommandList> command_list;
    require(
        SUCCEEDED(device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&command_list))),
        "CreateCommandList failed");

    xrfg::set_upscaler_auto_hook_d3d12_context(device.Get(), queue.Get());

    // Create feature with LowRes MVs (1<<1) + Jittered MVs (1<<2) + DepthInverted (1<<3)
    MockNgxParameter create_params;
    create_params.Set("DLSS.Feature.Create.Flags", static_cast<unsigned int>((1U << 1) | (1U << 2) | (1U << 3)));
    const xrfg::NgxHandleAbi left_handle{101U};
    const xrfg::NgxHandleAbi right_handle{102U};

    xrfg::on_ngx_d3d12_create_feature(command_list.Get(), 1U, &create_params, &left_handle);
    xrfg::on_ngx_d3d12_create_feature(command_list.Get(), 1U, &create_params, &right_handle);

    auto left_mv = create_texture(device.Get(), 960U, 1080U, DXGI_FORMAT_R16G16_FLOAT);
    auto left_depth = create_texture(device.Get(), 960U, 1080U, DXGI_FORMAT_R32_FLOAT);
    auto left_out = create_texture(device.Get(), 1920U, 2160U, DXGI_FORMAT_R8G8B8A8_UNORM);

    auto right_mv = create_texture(device.Get(), 960U, 1080U, DXGI_FORMAT_R16G16_FLOAT);
    auto right_depth = create_texture(device.Get(), 960U, 1080U, DXGI_FORMAT_R32_FLOAT);
    auto right_out = create_texture(device.Get(), 1920U, 2160U, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto stereo_swapchain = create_texture(device.Get(), 1920U, 2160U, DXGI_FORMAT_R8G8B8A8_UNORM, 2U);

    MockNgxParameter left_eval;
    left_eval.Set("MotionVectors", left_mv.Get());
    left_eval.Set("Depth", left_depth.Get());
    left_eval.Set("Output", left_out.Get());
    left_eval.Set("DLSS.Render.Subrect.Dimensions.Width", 960U);
    left_eval.Set("DLSS.Render.Subrect.Dimensions.Height", 1080U);
    left_eval.Set("MV.Scale.X", 1.0F);
    left_eval.Set("MV.Scale.Y", 1.0F);
    left_eval.Set("Jitter.Offset.X", 0.25F);
    left_eval.Set("Jitter.Offset.Y", -0.25F);

    MockNgxParameter right_eval;
    right_eval.Set("MotionVectors", right_mv.Get());
    right_eval.Set("Depth", right_depth.Get());
    right_eval.Set("Output", right_out.Get());
    right_eval.Set("DLSS.Render.Subrect.Dimensions.Width", 960U);
    right_eval.Set("DLSS.Render.Subrect.Dimensions.Height", 1080U);
    right_eval.Set("MV.Scale.X", 1.0F);
    right_eval.Set("MV.Scale.Y", 1.0F);
    right_eval.Set("Jitter.Offset.X", 0.25F);
    right_eval.Set("Jitter.Offset.Y", -0.25F);

    xrfg::on_ngx_d3d12_evaluate_feature(command_list.Get(), &left_handle, &left_eval);
    xrfg::on_ngx_d3d12_evaluate_feature(command_list.Get(), &right_handle, &right_eval);

    require(SUCCEEDED(command_list->Close()), "CommandList close failed");
    ID3D12CommandList* lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    require(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence failed");
    require(SUCCEEDED(queue->Signal(fence.Get(), 1)), "Signal failed");
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(SUCCEEDED(fence->SetEventOnCompletion(1, evt)), "SetEventOnCompletion failed");
    WaitForSingleObject(evt, 5000);
    CloseHandle(evt);

    const auto stats = xrfg::upscaler_auto_hook_statistics();
    require(stats.feature_creates_observed == 2U, "Expected 2 feature creates observed");
    require(stats.evaluations_observed == 2U, "Expected 2 evaluations observed");
    require(stats.publications_emitted == 2U, "Expected 2 publications emitted into DlssMotionVectorRegistry");
    require(stats.depth_resources_captured == 2U, "Expected 2 hardware depth buffers captured");

    const auto guides = xrfg::resolve_dlss_motion_vectors(stereo_swapchain.Get(), queue.Get());
    require(guides != nullptr, "resolve_dlss_motion_vectors must return stereo guide set from auto-hook");
    require(guides->eye_count == 2U, "Expected 2 stereo eyes in resolved guide set");
    require(guides->eyes[0] != nullptr && guides->eyes[1] != nullptr, "Both stereo eyes must be populated");
    require(guides->eyes[0]->motion_vectors != nullptr, "Left eye MV snapshot must be populated");
    require(guides->eyes[1]->motion_vectors != nullptr, "Right eye MV snapshot must be populated");
    require(guides->eyes[0]->depth != nullptr, "Left eye depth snapshot must be populated");
    require(guides->eyes[1]->depth != nullptr, "Right eye depth snapshot must be populated");
    require(guides->eyes[0]->depth_inverted && guides->eyes[1]->depth_inverted, "DepthInverted flag must propagate from NGX CreateFeature");
    require(guides->eyes[0]->jittered, "Jittered flag must propagate");
    require(guides->eyes[0]->motion_width == 960U && guides->eyes[0]->motion_height == 1080U, "Low-res MV subrect dimensions must propagate");

    // Verify in-process module scan runs safely when no NGX DLLs are loaded:
    (void)xrfg::install_upscaler_auto_hooks_in_process();

    xrfg::on_ngx_d3d12_release_feature(&left_handle);
    xrfg::on_ngx_d3d12_release_feature(&right_handle);
    xrfg::clear_upscaler_auto_hook_d3d12_context();
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();

    std::cout << "PASS: xrfg_upscaler_auto_hook_tests\n";
    return EXIT_SUCCESS;
}
