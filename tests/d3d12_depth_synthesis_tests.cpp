#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_history.hpp"
#include "xrfg/depth_reprojection.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 32;
constexpr UINT kEyeCount = 2;
constexpr UINT kBytesPerPixel = 4;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

using StereoPattern = std::array<std::vector<std::uint8_t>, kEyeCount>;
using RgbaBytes = std::array<std::uint8_t, kBytesPerPixel>;
using ReprojectionViews = std::array<xrfg::D3D12ReprojectionView, kEyeCount>;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void require_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        require(
            false,
            std::string(operation) + " failed with HRESULT " +
                std::to_string(static_cast<std::int32_t>(result)));
    }
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

class D3D12GpuFixture final {
public:
    D3D12GpuFixture() {
        require_hresult(
            CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.GetAddressOf())),
            "CreateDXGIFactory2");
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory_->EnumAdapterByGpuPreference(
                    index,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(candidate.GetAddressOf())) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            require_hresult(candidate->GetDesc1(&desc), "GetDesc1");
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                adapter_ = candidate;
                break;
            }
        }
        if (!adapter_) {
            require_hresult(
                factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter_.GetAddressOf())),
                "EnumWarpAdapter");
        }
        require_hresult(
            D3D12CreateDevice(
                adapter_.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device_.GetAddressOf())),
            "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        require_hresult(
            device_->CreateCommandQueue(
                &queue_desc,
                IID_PPV_ARGS(queue_.GetAddressOf())),
            "CreateCommandQueue");
        require_hresult(
            device_->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence_.GetAddressOf())),
            "CreateFence");
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(fence_event_ != nullptr, "CreateEventW");
    }

    ~D3D12GpuFixture() {
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
        }
    }

    ID3D12Device* device() const noexcept { return device_.Get(); }
    ID3D12CommandQueue* queue() const noexcept { return queue_.Get(); }

    template <typename Recorder>
    void execute_and_wait(Recorder&& recorder) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> cmd;
        require_hresult(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator.GetAddressOf())),
            "CreateCommandAllocator");
        require_hresult(
            device_->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(),
                nullptr,
                IID_PPV_ARGS(cmd.GetAddressOf())),
            "CreateCommandList");
        recorder(cmd.Get());
        require_hresult(cmd->Close(), "Close");
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const std::uint64_t val = next_fence_value_++;
        require_hresult(queue_->Signal(fence_.Get(), val), "Signal");
        if (fence_->GetCompletedValue() < val) {
            require_hresult(
                fence_->SetEventOnCompletion(val, fence_event_),
                "SetEventOnCompletion");
            require(
                WaitForSingleObject(fence_event_, 10000) == WAIT_OBJECT_0,
                "WaitForSingleObject timeout");
        }
    }

private:
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_{};
    std::uint64_t next_fence_value_{1};
};

D3D12_RESOURCE_DESC make_stereo_desc(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = kEyeCount;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

ComPtr<ID3D12Resource> create_buffer(
    D3D12GpuFixture& fixture,
    D3D12_HEAP_TYPE type,
    UINT64 size,
    D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto props = heap_properties(type);
    ComPtr<ID3D12Resource> buf;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            state,
            nullptr,
            IID_PPV_ARGS(buf.GetAddressOf())),
        "CreateCommittedResource(buffer)");
    return buf;
}

ComPtr<ID3D12Resource> create_color_texture(D3D12GpuFixture& fixture) {
    const auto props = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = make_stereo_desc(kWidth, kHeight, kFormat);
    ComPtr<ID3D12Resource> tex;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,
            IID_PPV_ARGS(tex.GetAddressOf())),
        "CreateCommittedResource(color)");
    return tex;
}

void upload_color_pattern(
    D3D12GpuFixture& fixture,
    ID3D12Resource* texture,
    const StereoPattern& pattern) {
    const auto desc = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> rows{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &desc, 0, kEyeCount, 0, layouts.data(), rows.data(), row_sizes.data(), &total_size);
    auto upload = create_buffer(
        fixture, D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped), "Map(color upload)");
    auto* bytes = static_cast<std::byte*>(mapped);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (UINT y = 0; y < kHeight; ++y) {
            std::memcpy(
                bytes + layouts[eye].Offset + static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch,
                pattern[eye].data() + static_cast<std::size_t>(y) * kWidth * kBytesPerPixel,
                kWidth * kBytesPerPixel);
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* cmd) {
        const auto to_copy = transition_barrier(
            texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &to_copy);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = texture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = eye;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        const auto to_rt = transition_barrier(
            texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &to_rt);
    });
}

StereoPattern readback_color_pattern(
    D3D12GpuFixture& fixture,
    ID3D12Resource* texture) {
    const auto desc = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> rows{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &desc, 0, kEyeCount, 0, layouts.data(), rows.data(), row_sizes.data(), &total_size);
    auto readback = create_buffer(
        fixture, D3D12_HEAP_TYPE_READBACK, total_size, D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* cmd) {
        const auto to_src = transition_barrier(
            texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &to_src);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = texture;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = eye;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        const auto to_rt = transition_barrier(
            texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &to_rt);
    });
    void* mapped = nullptr;
    require_hresult(readback->Map(0, nullptr, &mapped), "Map(readback)");
    const auto* bytes = static_cast<const std::byte*>(mapped);
    StereoPattern out;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        out[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            std::memcpy(
                out[eye].data() + static_cast<std::size_t>(y) * kWidth * kBytesPerPixel,
                bytes + layouts[eye].Offset + static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch,
                kWidth * kBytesPerPixel);
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return out;
}

template <typename Fn>
ComPtr<ID3D12Resource> create_motion_texture(D3D12GpuFixture& fixture, Fn&& fn) {
    const auto props = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = make_stereo_desc(
        kWidth, kHeight, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> tex;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(tex.GetAddressOf())),
        "CreateCommittedResource(motion)");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> rows{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &desc, 0, kEyeCount, 0, layouts.data(), rows.data(), row_sizes.data(), &total_size);
    auto upload = create_buffer(
        fixture, D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped), "Map(motion)");
    auto* bytes = static_cast<std::byte*>(mapped);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (UINT y = 0; y < kHeight; ++y) {
            auto* row = reinterpret_cast<float*>(
                bytes + layouts[eye].Offset + static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch);
            for (UINT x = 0; x < kWidth; ++x) {
                const auto mv = fn(eye, x, y);
                row[x * 2U] = mv[0];
                row[x * 2U + 1U] = mv[1];
            }
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* cmd) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = eye;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        const auto ready = transition_barrier(
            tex.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &ready);
    });
    return tex;
}

template <typename Fn>
ComPtr<ID3D12Resource> create_depth_texture(D3D12GpuFixture& fixture, Fn&& fn) {
    const auto props = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = make_stereo_desc(
        kWidth, kHeight, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> tex;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(tex.GetAddressOf())),
        "CreateCommittedResource(depth)");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> rows{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &desc, 0, kEyeCount, 0, layouts.data(), rows.data(), row_sizes.data(), &total_size);
    auto upload = create_buffer(
        fixture, D3D12_HEAP_TYPE_UPLOAD, total_size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped), "Map(depth)");
    auto* bytes = static_cast<std::byte*>(mapped);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (UINT y = 0; y < kHeight; ++y) {
            auto* row = reinterpret_cast<float*>(
                bytes + layouts[eye].Offset + static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch);
            for (UINT x = 0; x < kWidth; ++x) {
                row[x] = fn(eye, x, y);
            }
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* cmd) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = eye;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        const auto ready = transition_barrier(
            tex.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &ready);
    });
    return tex;
}

std::shared_ptr<xrfg::DlssMotionVectorSet> make_guide_set(
    D3D12GpuFixture& fixture,
    ID3D12Resource* mv_tex,
    ID3D12Resource* depth_tex,
    std::uint64_t serial,
    float camera_near = 0.1F,
    float camera_far = 1000.0F,
    bool depth_inverted = true,
    bool depth_infinite = true) {
    auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
    set->eye_count = kEyeCount;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
        frame->stream = static_cast<std::uint64_t>(eye) + 1ULL;
        frame->epoch = 1ULL;
        frame->serial = serial;
        frame->previous_serial = serial > 1ULL ? serial - 1ULL : 0ULL;
        frame->publication = serial * 2ULL + eye;
        frame->motion_vectors = mv_tex;
        frame->producer_queue = fixture.queue();
        frame->output_width = kWidth;
        frame->output_height = kHeight;
        frame->output_slice = eye;
        frame->motion_width = kWidth;
        frame->motion_height = kHeight;
        frame->motion_slice = eye;
        frame->scale_x = 1.0F;
        frame->scale_y = 1.0F;
        frame->resource_state =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        frame->depth = depth_tex;
        frame->depth_width = kWidth;
        frame->depth_height = kHeight;
        frame->depth_resource_state =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        frame->depth_inverted = depth_inverted;
        frame->depth_infinite = depth_infinite;
        frame->camera_near = camera_near;
        frame->camera_far = camera_far;
        set->eyes[eye] = std::move(frame);
    }
    return set;
}

ReprojectionViews make_stereo_views(float head_x = 0.0F) {
    ReprojectionViews views{};
    constexpr float quarter_pi = std::numbers::pi_v<float> * 0.25F;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        views[eye].pose.orientation = {0.0F, 0.0F, 0.0F, 1.0F};
        views[eye].pose.position = {
            head_x + (eye == 0U ? -0.032F : 0.032F),
            0.0F,
            0.0F,
        };
        views[eye].fov = {-quarter_pi, quarter_pi, quarter_pi, -quarter_pi};
        views[eye].image_rect = {0, 0, kWidth, kHeight};
        views[eye].array_slice = eye;
    }
    return views;
}

RgbaBytes get_pixel(const StereoPattern& pattern, UINT eye, UINT x, UINT y) {
    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * kBytesPerPixel;
    return {
        pattern[eye][idx + 0],
        pattern[eye][idx + 1],
        pattern[eye][idx + 2],
        pattern[eye][idx + 3],
    };
}

void set_pixel(StereoPattern& pattern, UINT eye, UINT x, UINT y, RgbaBytes color) {
    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * kBytesPerPixel;
    pattern[eye][idx + 0] = color[0];
    pattern[eye][idx + 1] = color[1];
    pattern[eye][idx + 2] = color[2];
    pattern[eye][idx + 3] = color[3];
}

void test_depth_ordered_disocclusion_and_horizon_search(D3D12GpuFixture& fixture) {
    auto src0 = create_color_texture(fixture);
    auto src1 = create_color_texture(fixture);
    auto curr_dst = create_color_texture(fixture);
    auto synth_dst = create_color_texture(fixture);

    std::array<ID3D12Resource*, 2> sources{src0.Get(), src1.Get()};
    std::array<ID3D12Resource*, 1> curr_dsts{curr_dst.Get()};
    std::array<ID3D12Resource*, 1> synth_dsts{synth_dst.Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(
        history->initialize(
            fixture.device(),
            fixture.queue(),
            sources,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "history.initialize");

    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(
        synth.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            curr_dsts,
            synth_dsts,
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "synth.initialize");

    // Background is blue (0, 0, 220) at Z = 10.0m (reversed-Z hw = 0.1 / 10.0 = 0.01).
    // A foreground patch (red = 240, 20, 20) at Z = 0.5m (reversed-Z hw = 0.1 / 0.5 = 0.20)
    // moves right by +8 pixels from x in [16..24) in PreviousFrame to x in [24..32) in CurrentFrame.
    // At midpoint t = 0.5, the foreground patch should appear at x in [20..28) via the
    // depth-ordered foreground horizon search!
    StereoPattern prev_color{};
    StereoPattern curr_color{};
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        prev_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        curr_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                set_pixel(prev_color, eye, x, y, {0, 0, 220, 255});
                set_pixel(curr_color, eye, x, y, {0, 0, 220, 255});
                if (y >= 8U && y < 24U) {
                    if (x >= 16U && x < 24U) {
                        set_pixel(prev_color, eye, x, y, {240, 20, 20, 255});
                    }
                    if (x >= 24U && x < 32U) {
                        set_pixel(curr_color, eye, x, y, {240, 20, 20, 255});
                    }
                }
            }
        }
    }

    upload_color_pattern(fixture, src0.Get(), prev_color);
    upload_color_pattern(fixture, src1.Get(), curr_color);

    auto prev_mv = create_motion_texture(fixture, [](UINT, UINT, UINT) {
        return std::array<float, 2>{0.0F, 0.0F};
    });
    auto curr_mv = create_motion_texture(fixture, [](UINT, UINT x, UINT y) {
        if (y >= 8U && y < 24U && x >= 24U && x < 32U) {
            return std::array<float, 2>{-8.0F, 0.0F};
        }
        return std::array<float, 2>{0.0F, 0.0F};
    });

    auto prev_depth = create_depth_texture(fixture, [](UINT, UINT x, UINT y) {
        return (y >= 8U && y < 24U && x >= 16U && x < 24U) ? 0.20F : 0.01F;
    });
    auto curr_depth = create_depth_texture(fixture, [](UINT, UINT x, UINT y) {
        return (y >= 8U && y < 24U && x >= 24U && x < 32U) ? 0.20F : 0.01F;
    });

    const auto views = make_stereo_views(0.0F);
    xrfg::D3D12HistoryCaptureTicket t0{};
    require_hresult(history->capture(0, &t0), "capture t0");
    require_hresult(history->commit(t0), "commit t0");
    xrfg::D3D12FrameSynthesisTicket prime_ticket{};
    require_hresult(
        synth.submit_prime(
            t0,
            views,
            0,
            &prime_ticket,
            make_guide_set(fixture, prev_mv.Get(), prev_depth.Get(), 1ULL)),
        "submit_prime");
    require_hresult(synth.wait_for_previous_submission(5000), "wait_for_previous_submission(prime)");

    xrfg::D3D12HistoryCaptureTicket t1{};
    require_hresult(history->capture(1, &t1), "capture t1");
    require_hresult(history->commit(t1), "commit t1");
    xrfg::D3D12FrameSynthesisTicket pair_ticket{};
    require_hresult(
        synth.submit_pair(
            t1,
            views,
            views,
            0,
            0,
            &pair_ticket,
            std::nullopt,
            make_guide_set(fixture, curr_mv.Get(), curr_depth.Get(), 2ULL)),
        "submit_pair(depth + horizon search)");
    require_hresult(synth.wait_for_idle(), "wait_for_idle(pair)");

    const auto out_pattern = readback_color_pattern(fixture, synth_dst.Get());
    // At x = 22, y = 16 (covered by background in CurrentFrame, but crossed by foreground at t=0.5),
    // the depth-ordered horizon search must find the red foreground object at x + 4 = 26!
    const auto mid_fg = get_pixel(out_pattern, 0U, 22U, 16U);
    require(
        mid_fg[0] > 180U && mid_fg[2] < 80U,
        "Depth-ordered horizon search must reconstruct the arriving foreground red pixel at x=22");

    // At x = 18, y = 16 (disoccluded background behind the departing foreground object),
    // the output must be clean blue background, not a red/blue ghost blend!
    const auto disoccluded_bg = get_pixel(out_pattern, 0U, 18U, 16U);
    require(
        disoccluded_bg[2] > 180U && disoccluded_bg[0] < 60U,
        "Disoccluded background pixel at x=18 must resolve to unoccluded blue background");
}

void test_asw_forward_extrapolation_on_gpu(D3D12GpuFixture& fixture) {
    auto src0 = create_color_texture(fixture);
    auto src1 = create_color_texture(fixture);
    auto curr_dst = create_color_texture(fixture);
    auto synth_dst = create_color_texture(fixture);

    std::array<ID3D12Resource*, 2> sources{src0.Get(), src1.Get()};
    std::array<ID3D12Resource*, 1> curr_dsts{curr_dst.Get()};
    std::array<ID3D12Resource*, 1> synth_dsts{synth_dst.Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(
        history->initialize(
            fixture.device(),
            fixture.queue(),
            sources,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "history.initialize(extrap)");

    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(
        synth.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            curr_dsts,
            synth_dsts,
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "synth.initialize(extrap)");

    // Foreground green block (20, 230, 20) moves right by +8 pixels from [16..24) in t-1
    // to [24..32) in t. With extrapolation_fraction = 1.5 (alpha = 0.5 ahead of t),
    // the extrapolated block should shift forward to [28..36), so pixel x = 34, y = 16
    // (which is background in both t-1 and t!) becomes green!
    StereoPattern prev_color{};
    StereoPattern curr_color{};
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        prev_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        curr_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                set_pixel(prev_color, eye, x, y, {20, 20, 40, 255});
                set_pixel(curr_color, eye, x, y, {20, 20, 40, 255});
                if (y >= 8U && y < 24U) {
                    if (x >= 16U && x < 24U) {
                        set_pixel(prev_color, eye, x, y, {20, 230, 20, 255});
                    }
                    if (x >= 24U && x < 32U) {
                        set_pixel(curr_color, eye, x, y, {20, 230, 20, 255});
                    }
                }
            }
        }
    }

    upload_color_pattern(fixture, src0.Get(), prev_color);
    upload_color_pattern(fixture, src1.Get(), curr_color);

    auto prev_mv = create_motion_texture(fixture, [](UINT, UINT, UINT) {
        return std::array<float, 2>{0.0F, 0.0F};
    });
    auto curr_mv = create_motion_texture(fixture, [](UINT, UINT x, UINT y) {
        if (y >= 8U && y < 24U && x >= 24U && x < 32U) {
            return std::array<float, 2>{-8.0F, 0.0F};
        }
        return std::array<float, 2>{0.0F, 0.0F};
    });

    auto prev_depth = create_depth_texture(fixture, [](UINT, UINT x, UINT y) {
        return (y >= 8U && y < 24U && x >= 16U && x < 24U) ? 0.20F : 0.01F;
    });
    auto curr_depth = create_depth_texture(fixture, [](UINT, UINT x, UINT y) {
        return (y >= 8U && y < 24U && x >= 24U && x < 32U) ? 0.20F : 0.01F;
    });

    const auto views = make_stereo_views(0.0F);
    xrfg::D3D12HistoryCaptureTicket t0{};
    require_hresult(history->capture(0, &t0), "capture t0(extrap)");
    require_hresult(history->commit(t0), "commit t0(extrap)");
    xrfg::D3D12FrameSynthesisTicket prime_ticket{};
    require_hresult(
        synth.submit_prime(
            t0,
            views,
            0,
            &prime_ticket,
            make_guide_set(fixture, prev_mv.Get(), prev_depth.Get(), 1ULL)),
        "submit_prime(extrap)");
    require_hresult(synth.wait_for_previous_submission(5000), "wait_for_previous_submission(prime extrap)");

    xrfg::D3D12HistoryCaptureTicket t1{};
    require_hresult(history->capture(1, &t1), "capture t1(extrap)");
    require_hresult(history->commit(t1), "commit t1(extrap)");
    xrfg::D3D12FrameSynthesisTicket extrap_ticket{};
    require_hresult(
        synth.submit_extrapolated_pair(
            t1,
            views,
            views,
            0,
            0,
            &extrap_ticket,
            1.5F,
            make_guide_set(fixture, curr_mv.Get(), curr_depth.Get(), 2ULL)),
        "submit_extrapolated_pair");
    require_hresult(synth.wait_for_idle(), "wait_for_idle(extrap)");

    const auto out_pattern = readback_color_pattern(fixture, synth_dst.Get());
    // At x = 34, y = 16 (4 pixels ahead of the CurrentFrame edge at 32!),
    // ASW forward extrapolation with horizon search must place the moving green block here!
    const auto extrap_fg = get_pixel(out_pattern, 0U, 34U, 16U);
    require(
        extrap_fg[1] > 180U && extrap_fg[0] < 60U,
        "ASW forward extrapolation must project moving green object ahead of CurrentFrame to x=34");
}

void test_cross_eye_stereo_synthesis_on_gpu(D3D12GpuFixture& fixture) {
    auto src0 = create_color_texture(fixture);
    auto src1 = create_color_texture(fixture);
    auto curr_dst = create_color_texture(fixture);
    auto synth_dst = create_color_texture(fixture);

    std::array<ID3D12Resource*, 2> sources{src0.Get(), src1.Get()};
    std::array<ID3D12Resource*, 1> curr_dsts{curr_dst.Get()};
    std::array<ID3D12Resource*, 1> synth_dsts{synth_dst.Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(
        history->initialize(
            fixture.device(),
            fixture.queue(),
            sources,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "history.initialize(cross-eye)");

    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(
        synth.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            curr_dsts,
            synth_dsts,
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "synth.initialize(cross-eye)");

    // Surface at linear depth Z = 0.512m (reversed-Z hw = 0.1 / 0.512 = 0.1953125).
    // With IPD = 0.064m (Left Eye x = -0.032m, Right Eye x = +0.032m) and 90deg FOV on 64px width
    // (focal length f = 32px):
    // Stereo disparity from Right Eye to Left Eye is:
    // delta_u = f * (+0.064) / 0.512 = 32 * 0.125 = +4.0 pixels!
    // So Right Eye pixel x = 30 maps to Left Eye pixel x = 34!
    // Put a bright yellow marker (250, 240, 10) in CurrentFrame Left Eye (slice 0) at x in [32..37),
    // while CurrentFrame Right Eye (slice 1) is left blank black (0, 0, 0).
    StereoPattern prev_color{};
    StereoPattern curr_color{};
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        prev_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        curr_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                set_pixel(prev_color, eye, x, y, {30, 30, 30, 255});
                set_pixel(curr_color, eye, x, y, {0, 0, 0, 255});
                if (eye == 0U && x >= 32U && x < 37U && y >= 10U && y < 22U) {
                    set_pixel(curr_color, 0U, x, y, {250, 240, 10, 255});
                }
            }
        }
    }

    upload_color_pattern(fixture, src0.Get(), prev_color);
    upload_color_pattern(fixture, src1.Get(), curr_color);

    auto zero_mv = create_motion_texture(fixture, [](UINT, UINT, UINT) {
        return std::array<float, 2>{0.0F, 0.0F};
    });
    constexpr float kHwDepth = 0.1F / 0.512F;
    auto uniform_depth = create_depth_texture(fixture, [](UINT, UINT, UINT) {
        return kHwDepth;
    });

    const auto views = make_stereo_views(0.0F);
    xrfg::D3D12HistoryCaptureTicket t0{};
    require_hresult(history->capture(0, &t0), "capture t0(cross-eye)");
    require_hresult(history->commit(t0), "commit t0(cross-eye)");
    xrfg::D3D12FrameSynthesisTicket prime_ticket{};
    require_hresult(
        synth.submit_prime(
            t0,
            views,
            0,
            &prime_ticket,
            make_guide_set(fixture, zero_mv.Get(), uniform_depth.Get(), 1ULL)),
        "submit_prime(cross-eye)");
    require_hresult(synth.wait_for_previous_submission(5000), "wait_for_previous_submission(prime cross-eye)");

    xrfg::D3D12HistoryCaptureTicket t1{};
    require_hresult(history->capture(1, &t1), "capture t1(cross-eye)");
    require_hresult(history->commit(t1), "commit t1(cross-eye)");
    xrfg::D3D12FrameSynthesisTicket stereo_ticket{};
    require_hresult(
        synth.submit_cross_eye_stereo(
            t1,
            views,
            views,
            0,
            0,
            &stereo_ticket,
            make_guide_set(fixture, zero_mv.Get(), uniform_depth.Get(), 2ULL)),
        "submit_cross_eye_stereo");
    require_hresult(synth.wait_for_idle(), "wait_for_idle(cross-eye)");

    const auto out_pattern = readback_color_pattern(fixture, synth_dst.Get());
    // In Right Eye (slice 1), pixel x = 30, y = 16 must have been synthesized from
    // Left Eye (slice 0) at x = 30 + 4 = 34 (bright yellow 250, 240, 10)!
    const auto right_eye_reconstructed = get_pixel(out_pattern, 1U, 30U, 16U);
    require(
        right_eye_reconstructed[0] > 200U && right_eye_reconstructed[1] > 190U &&
            right_eye_reconstructed[2] < 50U,
        "Cross-eye stereo synthesis must reconstruct Right Eye (slice 1) at x=30 from Left Eye (slice 0) at x=34 using 6-DOF IPD + depth disparity");
}

void test_6dof_depth_parallax_reprojection_on_gpu(D3D12GpuFixture& fixture) {
    auto src0 = create_color_texture(fixture);
    auto src1 = create_color_texture(fixture);
    auto curr_dst = create_color_texture(fixture);
    auto synth_dst = create_color_texture(fixture);

    std::array<ID3D12Resource*, 2> sources{src0.Get(), src1.Get()};
    std::array<ID3D12Resource*, 1> curr_dsts{curr_dst.Get()};
    std::array<ID3D12Resource*, 1> synth_dsts{synth_dst.Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(
        history->initialize(
            fixture.device(),
            fixture.queue(),
            sources,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "history.initialize(6dof)");

    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(
        synth.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            curr_dsts,
            synth_dsts,
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        "synth.initialize(6dof)");

    // Pure 6-DOF camera translation along +X by +0.064m between t-1 (head_x = 0.0m)
    // and t (head_x = +0.064m). Surface at Z = 0.512m shifts by -4 pixels on screen
    // from x in [32..37) in PreviousFrame to x in [28..33) in CurrentFrame.
    // With 6-DOF depth parallax enabled in map_target_to_source_with_depth,
    // mapping target pixel x = 30 (at head_x = +0.064m) back to PreviousFrame (at head_x = 0.0m)
    // shifts by +0.064 / 0.512 * 32 = +4.0 pixels to x = 34 in PreviousFrame, where the
    // cyan marker (10, 240, 240) was located at t-1!
    StereoPattern prev_color{};
    StereoPattern curr_color{};
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        prev_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        curr_color[eye].resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                set_pixel(prev_color, eye, x, y, {20, 20, 20, 255});
                set_pixel(curr_color, eye, x, y, {20, 20, 20, 255});
                if (y >= 10U && y < 22U) {
                    if (x >= 32U && x < 37U) {
                        set_pixel(prev_color, eye, x, y, {10, 240, 240, 255});
                    }
                    if (x >= 28U && x < 33U) {
                        set_pixel(curr_color, eye, x, y, {10, 240, 240, 255});
                    }
                }
            }
        }
    }

    upload_color_pattern(fixture, src0.Get(), prev_color);
    upload_color_pattern(fixture, src1.Get(), curr_color);

    auto zero_mv = create_motion_texture(fixture, [](UINT, UINT, UINT) {
        return std::array<float, 2>{0.0F, 0.0F};
    });
    constexpr float kHwDepth = 0.1F / 0.512F;
    auto uniform_depth = create_depth_texture(fixture, [](UINT, UINT, UINT) {
        return kHwDepth;
    });

    const auto prev_views = make_stereo_views(0.0F);
    const auto curr_views = make_stereo_views(0.064F);
    xrfg::D3D12HistoryCaptureTicket t0{};
    require_hresult(history->capture(0, &t0), "capture t0(6dof)");
    require_hresult(history->commit(t0), "commit t0(6dof)");
    xrfg::D3D12FrameSynthesisTicket prime_ticket{};
    require_hresult(
        synth.submit_prime(
            t0,
            prev_views,
            0,
            &prime_ticket,
            make_guide_set(fixture, zero_mv.Get(), uniform_depth.Get(), 1ULL)),
        "submit_prime(6dof)");
    require_hresult(synth.wait_for_previous_submission(5000), "wait_for_previous_submission(prime 6dof)");

    xrfg::D3D12HistoryCaptureTicket t1{};
    require_hresult(history->capture(1, &t1), "capture t1(6dof)");
    require_hresult(history->commit(t1), "commit t1(6dof)");
    xrfg::D3D12FrameSynthesisTicket pair_ticket{};
    require_hresult(
        synth.submit_pair(
            t1,
            curr_views,
            curr_views,
            0,
            0,
            &pair_ticket,
            std::nullopt,
            make_guide_set(fixture, zero_mv.Get(), uniform_depth.Get(), 2ULL)),
        "submit_pair(6dof)");
    require_hresult(synth.wait_for_idle(), "wait_for_idle(6dof)");

    const auto out_pattern = readback_color_pattern(fixture, synth_dst.Get());
    // At x = 30, y = 16: with 6-DOF depth parallax, both PreviousFrame (sampled at x=34)
    // and CurrentFrame (sampled at x=30) agree on cyan (10, 240, 240), whereas 3-DOF
    // rotation-only would have sampled PreviousFrame at x=30 (dark 20,20,20) and dimmed it!
    const auto parallax_px = get_pixel(out_pattern, 0U, 30U, 16U);
    require(
        parallax_px[1] > 210U && parallax_px[2] > 210U && parallax_px[0] < 40U,
        "6-DOF depth parallax reprojection must align PreviousFrame(x=34) with CurrentFrame(x=30) at full cyan intensity");
}

}  // namespace

int main() {
    D3D12GpuFixture fixture;
    test_6dof_depth_parallax_reprojection_on_gpu(fixture);
    test_depth_ordered_disocclusion_and_horizon_search(fixture);
    test_asw_forward_extrapolation_on_gpu(fixture);
    test_cross_eye_stereo_synthesis_on_gpu(fixture);
    std::cout << "PASS: xrfg_d3d12_depth_synthesis_tests\n";
    return EXIT_SUCCESS;
}
