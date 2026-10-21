Texture2DArray<float4> PreviousFrame : register(t0);
Texture2DArray<float4> CurrentFrame : register(t1);
Texture2D<int2> BackwardFlow : register(t2);
Texture2D<uint> FlowAuxiliary : register(t3);
Texture2D<int2> ForwardFlow : register(t4);
Texture2D<uint> ForwardAuxiliary : register(t5);
Texture2DArray<float2> GameMotionVectors : register(t6);
Texture2DArray<float> CurrentDepth : register(t7);
Texture2DArray<float> PreviousDepth : register(t8);
RWTexture2D<float4> PackedColor : register(u0);
// The NVIDIA pack is compiled twice: once for BGRA8 inputs, and once with
// XRFG_NVIDIA_LUMA_INPUT for R8 inputs, which the engine takes as grayscale
// and which is a quarter of the bytes for the same flow - it matches on
// luma either way. The C++ side picks the pipeline by the format the driver
// accepted at creation.
#ifdef XRFG_NVIDIA_LUMA_INPUT
RWTexture2D<float> NvidiaPreviousColor : register(u1);
RWTexture2D<float> NvidiaCurrentColor : register(u2);
#else
RWTexture2D<float4> NvidiaPreviousColor : register(u1);
RWTexture2D<float4> NvidiaCurrentColor : register(u2);
#endif

struct CameraMapping {
    float4 TargetToSourceRotation;
    float4 SourceTangents;
    float4 TargetTangents;
    float4 SourceRect;
    float4 TargetRect;
};

cbuffer SynthesisParameters : register(b0) {
    uint Width;
    uint Height;
    uint ArraySize;
    uint PackedWidth;
    uint PackedHeight;
    uint PackedEyeStride;
    uint FlowWidth;
    uint FlowHeight;
    uint FlowBlockSize;
    uint ViewIndex;
    uint UseGameMotion;
    uint GameMotionPadding;
    float4 GameMotionRect;
    float2 GameMotionScale;
    float2 GameMotionJitterDelta;
    CameraMapping PreviousMappings[2];
    uint Slice;
    // Bit 0 is the repeated-capture flag. Bit 1 asks the pack shaders to
    // sRGB-encode the flow input.
    // Bit 2: CurrentDepth SRV (t7) valid.
    // Bit 3: PreviousDepth SRV (t8) valid.
    // Bit 4: Reversed-Z depth (1.0 at near, 0.0 at far).
    // Bit 5: Infinite far plane depth.
    // Bit 6: Enable 6-DOF positional parallax & depth occlusion sorting.
    // Bit 7: Enable Meta ASW 2.0 3x3 adaptive median & pseudo-depth sorting.
    // Bits 8-23 carry the synthetic's position between the two captures in
    // 1/65535ths - 0 at the previous capture, 1 at the current.
    // Bit 24: ASW zero-latency extrapolation mode (fraction = 1.0 + packed).
    // Bit 25: Cross-eye stereo-temporal synthesis mode (AFW / AER).
    uint SynthesisFlags;
};

uint active_view_index() {
    return min(ViewIndex & 0xffffu, 1u);
}

uint repeated_capture_flag() {
    return SynthesisFlags & 1u;
}

bool encode_srgb_flow_input() {
    return (SynthesisFlags & 2u) != 0u;
}

bool has_current_depth_flag() {
    return (SynthesisFlags & 4u) != 0u;
}

bool has_previous_depth_flag() {
    return (SynthesisFlags & 8u) != 0u;
}

bool depth_inverted_flag() {
    return (SynthesisFlags & 16u) != 0u;
}

bool depth_infinite_flag() {
    return (SynthesisFlags & 32u) != 0u;
}

bool enable_6dof_parallax_flag() {
    return (SynthesisFlags & 64u) != 0u;
}

bool enable_asw_median_flag() {
    return (SynthesisFlags & 128u) != 0u;
}

bool asw_single_source_flag() {
    return (SynthesisFlags & (1u << 29)) != 0u;
}

bool asw_extrapolation_mode_flag() {
    return (SynthesisFlags & (1u << 24)) != 0u;
}

bool cross_eye_stereo_mode_flag() {
    return (SynthesisFlags & (1u << 25)) != 0u;
}

bool use_game_motion_flag() {
    return (UseGameMotion & 1u) != 0u;
}

float3 target_to_source_translation_vec() {
    float3 result = GameMotionRect.xyz;
    if (use_game_motion_flag()) {
        result = float3(
            asfloat(PackedWidth),
            asfloat(PackedHeight),
            asfloat(PackedEyeStride));
    }
    return result;
}

float2 depth_near_far_planes() {
    float near_opt = GameMotionRect.w > 1.0e-4 ? GameMotionRect.w : 0.1;
    float far_opt = GameMotionScale.x > near_opt ? GameMotionScale.x : 1000.0;
    float2 result = float2(near_opt, far_opt);
    if (use_game_motion_flag()) {
        uint near_half = (UseGameMotion >> 16) & 0xffffu;
        uint far_half = (ViewIndex >> 16) & 0xffffu;
        float near_val = near_half != 0u ? f16tof32(near_half) : 0.1;
        float far_val = far_half != 0u ? f16tof32(far_half) : 1000.0;
        result = float2(max(near_val, 1.0e-4), max(far_val, near_val + 1.0));
    }
    return result;
}

float linearize_depth_value(float z_hw) {
    float2 near_far = depth_near_far_planes();
    float z_near = near_far.x;
    float z_far = near_far.y;
    float clamped = saturate(z_hw);
    float denom = depth_inverted_flag()
        ? (z_near + clamped * (z_far - z_near))
        : (z_far - clamped * (z_far - z_near));
    float result = (z_near * z_far) / max(denom, 1.0e-6);
    if (depth_infinite_flag()) {
        float denom_inf = depth_inverted_flag()
            ? max(clamped, 1.0e-6)
            : max(1.0 - clamped, 1.0e-6);
        result = z_near / denom_inf;
    }
    return result;
}

int2 map_pixel_to_depth_coord(float2 pixel) {
    float2 clamped_pixel = clamp(
        pixel,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    int2 result = int2(round(clamped_pixel));
    if (use_game_motion_flag()) {
        float4 output_rect = float4(
            float(FlowWidth), float(FlowHeight),
            float(FlowBlockSize), float(GameMotionPadding));
        float2 uv = (pixel - output_rect.xy + 0.5) /
            max(output_rect.zw, float2(1.0, 1.0));
        float2 coord = GameMotionRect.xy + uv * GameMotionRect.zw - 0.5;
        float2 minimum = GameMotionRect.xy;
        float2 maximum = GameMotionRect.xy + GameMotionRect.zw - 1.0;
        result = int2(round(clamp(coord, minimum, maximum)));
    }
    return result;
}

float sample_current_linear_depth(float2 pixel) {
    float result = 0.0;
    if (has_current_depth_flag()) {
        int2 coord = map_pixel_to_depth_coord(pixel);
        float z_hw = CurrentDepth.Load(int4(coord, 0, 0));
        result = linearize_depth_value(z_hw);
    }
    return result;
}

float sample_previous_linear_depth(float2 pixel) {
    float result = 0.0;
    if (has_previous_depth_flag()) {
        int2 coord = map_pixel_to_depth_coord(pixel);
        float z_hw = PreviousDepth.Load(int4(coord, 0, 0));
        result = linearize_depth_value(z_hw);
    }
    return result;
}

float2 map_pixel_to_depth_coord_clamped(float2 pixel) {
    return float2(map_pixel_to_depth_coord(pixel));
}

// 9-tap morphological depth neighborhood (foreground dilation + discontinuity detection).
struct DepthNeighborhoodSample {
    float foreground_depth;
    float background_depth;
    float center_depth;
    bool is_silhouette;
};

DepthNeighborhoodSample sample_current_depth_neighborhood(float2 pixel) {
    DepthNeighborhoodSample result;
    result.foreground_depth = 0.0;
    result.background_depth = 0.0;
    result.center_depth = 0.0;
    result.is_silhouette = false;
    if (has_current_depth_flag()) {
        float center_z = sample_current_linear_depth(pixel);
        result.center_depth = center_z;
        result.foreground_depth = center_z;
        result.background_depth = center_z;
        static const float2 kOffsets[4] = {
            float2(-1.0, 0.0), float2(1.0, 0.0),
            float2(0.0, -1.0), float2(0.0, 1.0)
        };
        [unroll] for (uint i = 0u; i < 4u; ++i) {
            float z_n = sample_current_linear_depth(pixel + kOffsets[i]);
            if (z_n > 1.0e-4) {
                result.foreground_depth = result.foreground_depth > 1.0e-4
                    ? min(result.foreground_depth, z_n)
                    : z_n;
                result.background_depth = max(result.background_depth, z_n);
            }
        }
        float gap = result.background_depth - result.foreground_depth;
        result.is_silhouette =
            result.foreground_depth > 1.0e-4 &&
            (gap / max(result.foreground_depth, 0.02)) > 0.04;
    }
    return result;
}

float edge_feather_factor(float2 coordinate, float4 rect, float feather_px) {
    float2 minimum = rect.xy - 0.5;
    float2 maximum = rect.xy + rect.zw - 0.5;
    bool inside =
        coordinate.x >= minimum.x && coordinate.y >= minimum.y &&
        coordinate.x <= maximum.x && coordinate.y <= maximum.y;
    float dist = min(
        min(coordinate.x - minimum.x, maximum.x - coordinate.x),
        min(coordinate.y - minimum.y, maximum.y - coordinate.y));
    return inside ? smoothstep(0.0, max(feather_px, 1.0), dist) : 0.0;
}

// Damps small spurious optical-flow vectors on static/low-contrast surfaces (HUD, fine grids).
float static_motion_damping(float2 mv, float4 color_a, float4 color_b) {
    float mag = length(mv);
    float luma_a = dot(color_a.rgb, float3(0.2126, 0.7152, 0.0722));
    float luma_b = dot(color_b.rgb, float3(0.2126, 0.7152, 0.0722));
    float luma_diff = abs(luma_a - luma_b);
    float mag_factor = smoothstep(0.35, 1.75, mag);
    float luma_factor = smoothstep(0.004, 0.025, luma_diff);
    return max(mag_factor, luma_factor);
}
float3 linear_to_srgb(float3 linear_color) {
    float3 low = linear_color * 12.92;
    float3 high = 1.055 * pow(max(linear_color, 0.0), 1.0 / 2.4) - 0.055;
    return linear_color <= 0.0031308 ? low : high;
}

float4 pack_flow_input_color(float4 color) {
    float4 packed = saturate(color);
    if (encode_srgb_flow_input()) {
        packed.rgb = linear_to_srgb(packed.rgb);
    }
    return packed;
}

// Rec. 709 weights give relative luminance from linear values; the encode
// then spreads it over the codes the way the swapchain spreads colour.
float pack_flow_input_luma(float4 color) {
    const float3 luma_weights = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(saturate(color).rgb, luma_weights);
    if (encode_srgb_flow_input()) {
        luma = linear_to_srgb(luma.xxx).x;
    }
    return luma;
}

// An unset field decodes to 0.5, which is the fixed midpoint this shader
// used before the layer began deriving it. In ASW extrapolation mode (bit 24),
// the fraction represents 1.0 + base in (1.0, 2.0].
float synthesis_fraction() {
    uint packed = (SynthesisFlags >> 8) & 0xffffu;
    float base = packed == 0u ? 0.5 : float(packed) / 65535.0;
    return asw_extrapolation_mode_flag() ? (1.0 + base) : base;
}

float3 rotate_by_quaternion(float4 quaternion, float3 input_vector) {
    return input_vector + 2.0 * cross(
        quaternion.xyz,
        cross(quaternion.xyz, input_vector) + quaternion.w * input_vector);
}

struct MappedCoordinate {
    float2 coordinate;
    float valid;
    float reprojected_depth;
};

MappedCoordinate map_target_to_source_with_depth_scaled(
    float2 target_coordinate,
    CameraMapping mapping,
    float linear_depth,
    float translation_scale) {
    MappedCoordinate output;
    output.coordinate = float2(0.0, 0.0);
    output.valid = 0.0;
    output.reprojected_depth = 0.0;
    float2 target_minimum = mapping.TargetRect.xy;
    float2 target_extent = mapping.TargetRect.zw;
    float2 target_maximum = target_minimum + target_extent - 1.0;
    bool target_inside =
        target_coordinate.x >= target_minimum.x &&
        target_coordinate.y >= target_minimum.y &&
        target_coordinate.x <= target_maximum.x &&
        target_coordinate.y <= target_maximum.y;

    float2 target_uv =
        (target_coordinate - target_minimum + 0.5) / target_extent;
    float2 target_tangent = float2(
        lerp(mapping.TargetTangents.x, mapping.TargetTangents.y, target_uv.x),
        lerp(mapping.TargetTangents.z, mapping.TargetTangents.w, target_uv.y));
    float3 target_ray = float3(target_tangent, -1.0);
    float3 source_ray = rotate_by_quaternion(
        mapping.TargetToSourceRotation,
        target_ray);
    if (enable_6dof_parallax_flag() && linear_depth > 1.0e-4) {
        float3 translation = target_to_source_translation_vec() * translation_scale;
        source_ray += translation / linear_depth;
        output.reprojected_depth = -source_ray.z * linear_depth;
    }
    bool source_in_front = source_ray.z < -1.0e-5;
    float inverse_depth = 1.0 / max(-source_ray.z, 1.0e-5);
    float2 source_tangent = source_ray.xy * inverse_depth;
    float source_width = mapping.SourceTangents.y - mapping.SourceTangents.x;
    float source_height = mapping.SourceTangents.w - mapping.SourceTangents.z;
    bool valid_fov = source_width > 1.0e-7 && abs(source_height) > 1.0e-7;
    source_width = max(source_width, 1.0e-7);
    source_height = source_height < 0.0
                        ? min(source_height, -1.0e-7)
                        : max(source_height, 1.0e-7);
    float2 source_uv = float2(
        (source_tangent.x - mapping.SourceTangents.x) / source_width,
        (source_tangent.y - mapping.SourceTangents.z) / source_height);
    float2 source_minimum = mapping.SourceRect.xy;
    float2 source_extent = mapping.SourceRect.zw;
    float2 source_maximum = source_minimum + source_extent;
    output.coordinate = source_minimum + source_uv * source_extent - 0.5;
    output.valid = target_inside && source_in_front && valid_fov &&
        output.coordinate.x >= source_minimum.x - 0.5 &&
        output.coordinate.y >= source_minimum.y - 0.5 &&
        output.coordinate.x <= source_maximum.x - 0.5 &&
        output.coordinate.y <= source_maximum.y - 0.5
            ? 1.0
            : 0.0;
    return output;
}

MappedCoordinate map_target_to_source_with_depth(
    float2 target_coordinate,
    CameraMapping mapping,
    float linear_depth) {
    return map_target_to_source_with_depth_scaled(
        target_coordinate,
        mapping,
        linear_depth,
        1.0);
}


MappedCoordinate map_target_to_source(
    float2 target_coordinate,
    CameraMapping mapping) {
    return map_target_to_source_with_depth_scaled(
        target_coordinate,
        mapping,
        0.0,
        0.0);
}

MappedCoordinate map_source_to_target_with_depth(
    float2 source_coordinate,
    CameraMapping mapping,
    float linear_depth) {
    MappedCoordinate output;
    output.coordinate = float2(0.0, 0.0);
    output.valid = 0.0;
    output.reprojected_depth = 0.0;
    float2 source_minimum = mapping.SourceRect.xy;
    float2 source_extent = mapping.SourceRect.zw;
    float2 source_maximum = source_minimum + source_extent - 1.0;
    bool source_inside =
        source_coordinate.x >= source_minimum.x &&
        source_coordinate.y >= source_minimum.y &&
        source_coordinate.x <= source_maximum.x &&
        source_coordinate.y <= source_maximum.y;

    float2 source_uv =
        (source_coordinate - source_minimum + 0.5) / source_extent;
    float2 source_tangent = float2(
        lerp(mapping.SourceTangents.x, mapping.SourceTangents.y, source_uv.x),
        lerp(mapping.SourceTangents.z, mapping.SourceTangents.w, source_uv.y));
    float3 source_ray = float3(source_tangent, -1.0);
    float4 source_to_target_rotation = float4(
        -mapping.TargetToSourceRotation.xyz,
        mapping.TargetToSourceRotation.w);
    float3 target_ray = rotate_by_quaternion(
        source_to_target_rotation,
        source_ray);
    if (enable_6dof_parallax_flag() && linear_depth > 1.0e-4) {
        float3 tgt_to_src = target_to_source_translation_vec();
        float3 src_to_tgt = rotate_by_quaternion(
            source_to_target_rotation,
            -tgt_to_src);
        target_ray += src_to_tgt / linear_depth;
        output.reprojected_depth = -target_ray.z * linear_depth;
    }
    bool target_in_front = target_ray.z < -1.0e-5;
    float inverse_depth = 1.0 / max(-target_ray.z, 1.0e-5);
    float2 target_tangent = target_ray.xy * inverse_depth;
    float target_width = mapping.TargetTangents.y - mapping.TargetTangents.x;
    float target_height = mapping.TargetTangents.w - mapping.TargetTangents.z;
    bool valid_fov = target_width > 1.0e-7 && abs(target_height) > 1.0e-7;
    target_width = max(target_width, 1.0e-7);
    target_height = target_height < 0.0
                        ? min(target_height, -1.0e-7)
                        : max(target_height, 1.0e-7);
    float2 target_uv = float2(
        (target_tangent.x - mapping.TargetTangents.x) / target_width,
        (target_tangent.y - mapping.TargetTangents.z) / target_height);
    float2 target_minimum = mapping.TargetRect.xy;
    float2 target_extent = mapping.TargetRect.zw;
    float2 target_maximum = target_minimum + target_extent;
    output.coordinate = target_minimum + target_uv * target_extent - 0.5;
    output.valid = source_inside && target_in_front && valid_fov &&
        output.coordinate.x >= target_minimum.x - 0.5 &&
        output.coordinate.y >= target_minimum.y - 0.5 &&
        output.coordinate.x <= target_maximum.x - 0.5 &&
        output.coordinate.y <= target_maximum.y - 0.5
            ? 1.0
            : 0.0;
    return output;
}

MappedCoordinate map_source_to_target(
    float2 source_coordinate,
    CameraMapping mapping) {
    return map_source_to_target_with_depth(source_coordinate, mapping, 0.0);
}

float4 bilinear_previous_source(float2 coordinate, uint slice) {
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(
        top_left + int2(1, 1),
        int2(int(Width) - 1, int(Height) - 1));
    float2 fraction = bounded - float2(top_left);
    float4 top = lerp(
        PreviousFrame.Load(int4(top_left, int(slice), 0)),
        PreviousFrame.Load(int4(bottom_right.x, top_left.y, int(slice), 0)),
        fraction.x);
    float4 bottom = lerp(
        PreviousFrame.Load(int4(top_left.x, bottom_right.y, int(slice), 0)),
        PreviousFrame.Load(int4(bottom_right, int(slice), 0)),
        fraction.x);
    return lerp(top, bottom, fraction.y);
}

float4 bilinear_current_source(float2 coordinate, uint slice) {
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(
        top_left + int2(1, 1),
        int2(int(Width) - 1, int(Height) - 1));
    float2 fraction = bounded - float2(top_left);
    float4 top = lerp(
        CurrentFrame.Load(int4(top_left, int(slice), 0)),
        CurrentFrame.Load(int4(bottom_right.x, top_left.y, int(slice), 0)),
        fraction.x);
    float4 bottom = lerp(
        CurrentFrame.Load(int4(top_left.x, bottom_right.y, int(slice), 0)),
        CurrentFrame.Load(int4(bottom_right, int(slice), 0)),
        fraction.x);
    return lerp(top, bottom, fraction.y);
}

// The ratio the flow input was packed at, relative to the source image.
float2 flow_input_scale() {
    float ratio = min(
        1.0,
        float(PackedWidth) / max(float(Width), 1.0));
    return float2(ratio, ratio);
}

float2 flow_input_coordinate(float2 full_resolution_coordinate) {
    return (full_resolution_coordinate + 0.5) *
        flow_input_scale() - 0.5;
}

struct CameraSample {
    float4 color;
    float valid;
    float3 padding;
};

CameraSample sample_previous_target_with_depth(
    float2 target_coordinate,
    uint slice,
    uint view_index,
    float linear_depth) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    MappedCoordinate mapped = map_target_to_source_with_depth(
        target_coordinate,
        PreviousMappings[view_index],
        linear_depth);
    if (mapped.valid > 0.5) {
        output.color = bilinear_previous_source(mapped.coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

CameraSample sample_previous_target(
    float2 target_coordinate,
    uint slice,
    uint view_index) {
    return sample_previous_target_with_depth(
        target_coordinate,
        slice,
        view_index,
        0.0);
}

CameraSample sample_current_target(
    float2 target_coordinate,
    uint slice,
    uint view_index) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    float2 target_minimum = PreviousMappings[view_index].TargetRect.xy;
    float2 target_maximum = target_minimum +
        PreviousMappings[view_index].TargetRect.zw;
    bool valid = target_coordinate.x >= target_minimum.x - 0.5 &&
                 target_coordinate.y >= target_minimum.y - 0.5 &&
                 target_coordinate.x <= target_maximum.x - 0.5 &&
                 target_coordinate.y <= target_maximum.y - 0.5;
    if (valid) {
        output.color = bilinear_current_source(target_coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

CameraSample sample_current_target_with_asw(
    float2 target_coordinate,
    uint slice,
    uint view_index,
    float2 asw_cam_shift) {
    float2 shifted = target_coordinate + asw_cam_shift;
    if ((SynthesisFlags & (1u << 28)) != 0u) {
        float4 rect = PreviousMappings[view_index].TargetRect;
        shifted = clamp(
            shifted,
            rect.xy,
            rect.xy + max(rect.zw - 1.0, float2(0.0, 0.0)));
    }
    return sample_current_target(
        shifted,
        slice,
        view_index);
}

[numthreads(8, 8, 1)]
void PackFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = thread_id.y / PackedEyeStride;
    uint local_y = thread_id.y - slice * PackedEyeStride;
    float2 input_scale = flow_input_scale();
    if (slice >= (ArraySize & 0xffffu)) {
        PackedColor[thread_id.xy] = pack_flow_input_color(current_color);
        return;
    }
    if (PackedWidth < Width) {
        float2 source_coordinate =
            (float2(float(thread_id.x), float(local_y)) + 0.5) /
                max(input_scale, float2(1.0e-6, 1.0e-6)) - 0.5;
        if (source_coordinate.y < float(Height)) {
            current_color =
                bilinear_current_source(source_coordinate, slice);
        }
    } else if (thread_id.x < Width && local_y < Height) {
        current_color = CurrentFrame.Load(
            int4(int2(thread_id.x, local_y), int(slice), 0));
    }
    PackedColor[thread_id.xy] = pack_flow_input_color(current_color);
}

uint pack_view_for_pixel(float2 target_coordinate, uint slice) {
    uint view = 2u;
    [unroll] for (uint v = 0; v < 2; ++v) {
        float4 rect = PreviousMappings[v].TargetRect;
        float2 minimum = rect.xy - 0.5;
        float2 maximum = rect.xy + rect.zw - 0.5;
        bool inside = rect.z > 0.0 && rect.w > 0.0 &&
            target_coordinate.x >= minimum.x && target_coordinate.y >= minimum.y &&
            target_coordinate.x <= maximum.x && target_coordinate.y <= maximum.y;
        if (inside && (view == 2u || v == slice)) {
            view = v;
        }
    }
    return view;
}

[numthreads(8, 8, 1)]
void PackNvidiaFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 previous_color = float4(0.0, 0.0, 0.0, 1.0);
    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = Slice & 0xffffu;
    bool downscaled = PackedWidth < Width || PackedHeight < Height;
    if (slice < (ArraySize & 0xffffu)) {
        float2 target_coordinate = downscaled
            ? (float2(thread_id.xy) + 0.5) / flow_input_scale() - 0.5
            : float2(thread_id.xy);
        if (target_coordinate.x < float(Width) &&
            target_coordinate.y < float(Height)) {
            uint view_index = pack_view_for_pixel(target_coordinate, slice);
            if (view_index < 2u) {
                CameraSample previous = sample_previous_target(
                    target_coordinate, slice, view_index);
                if (previous.valid >= 0.5) {
                    previous_color = previous.color;
                }
            } else {
                previous_color =
                    bilinear_previous_source(target_coordinate, slice);
            }
            current_color = downscaled
                ? bilinear_current_source(target_coordinate, slice)
                : CurrentFrame.Load(int4(int2(thread_id.xy), int(slice), 0));
        }
    }
#ifdef XRFG_NVIDIA_LUMA_INPUT
    NvidiaPreviousColor[thread_id.xy] = pack_flow_input_luma(previous_color);
    NvidiaCurrentColor[thread_id.xy] = pack_flow_input_luma(current_color);
#else
    NvidiaPreviousColor[thread_id.xy] = pack_flow_input_color(previous_color);
    NvidiaCurrentColor[thread_id.xy] = pack_flow_input_color(current_color);
#endif
}

float2 load_backward_flow_clamped(
    int2 coordinate,
    int2 minimum,
    int2 maximum) {
    return float2(BackwardFlow.Load(int3(clamp(coordinate, minimum, maximum), 0)));
}

float2 load_forward_flow_clamped(
    int2 coordinate,
    int2 minimum,
    int2 maximum) {
    return float2(ForwardFlow.Load(int3(clamp(coordinate, minimum, maximum), 0)));
}

struct FlowGridBounds {
    int2 minimum;
    int2 maximum;
};

FlowGridBounds flow_grid_bounds(
    float4 image_rect,
    uint slice) {
    uint2 image_minimum = uint2(image_rect.xy);
    uint2 image_maximum = image_minimum + uint2(image_rect.zw) - 1U;
    {
        float2 input_scale = flow_input_scale();
        uint2 scaled_minimum = uint2(floor(
            float2(image_minimum) * input_scale));
        uint2 scaled_end = uint2(ceil(
            float2(image_maximum + 1U) * input_scale));
        image_minimum = scaled_minimum;
        image_maximum = max(scaled_minimum, scaled_end - 1U);
    }
    FlowGridBounds bounds;
    bounds.minimum = int2(
        int(image_minimum.x / FlowBlockSize),
        int((slice * PackedEyeStride + image_minimum.y) / FlowBlockSize));
    bounds.maximum = int2(
        int(image_maximum.x / FlowBlockSize),
        int((slice * PackedEyeStride + image_maximum.y) / FlowBlockSize));
    bounds.maximum = min(
        bounds.maximum,
        int2(int(FlowWidth) - 1, int(FlowHeight) - 1));
    return bounds;
}

float2 flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale,
    bool use_nvidia_input_scale) {
    float2 scaled = flow_input_coordinate(pixel);
    float2 packed_coordinate = float2(
        scaled.x,
        scaled.y + float(slice * PackedEyeStride));
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice);
    float2 bounded = clamp(
        flow_coordinate,
        float2(bounds.minimum),
        float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        load_backward_flow_clamped(
            top_left,
            bounds.minimum,
            bounds.maximum),
        load_backward_flow_clamped(
            int2(bottom_right.x, top_left.y),
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 bottom = lerp(
        load_backward_flow_clamped(
            int2(top_left.x, bottom_right.y),
            bounds.minimum,
            bounds.maximum),
        load_backward_flow_clamped(
            bottom_right,
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 result = lerp(top, bottom, fraction.y) * value_scale;
    result /= max(flow_input_scale(), float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float2 forward_flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale,
    bool warped_previous) {
    float2 input_scale = flow_input_scale();
    float2 packed_coordinate = flow_input_coordinate(pixel);
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        warped_previous
            ? PreviousMappings[view_index].TargetRect
            : PreviousMappings[view_index].SourceRect,
        slice);
    float2 bounded = clamp(
        flow_coordinate,
        float2(bounds.minimum),
        float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        load_forward_flow_clamped(
            top_left,
            bounds.minimum,
            bounds.maximum),
        load_forward_flow_clamped(
            int2(bottom_right.x, top_left.y),
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 bottom = lerp(
        load_forward_flow_clamped(
            int2(top_left.x, bottom_right.y),
            bounds.minimum,
            bounds.maximum),
        load_forward_flow_clamped(
            bottom_right,
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 result = lerp(top, bottom, fraction.y) * value_scale /
        max(input_scale, float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float2 game_motion_for_pixel(float2 pixel, uint slice, uint view_index) {
    float4 output_rect = float4(
        float(FlowWidth), float(FlowHeight),
        float(FlowBlockSize), float(GameMotionPadding));
    float2 uv = (pixel - output_rect.xy + 0.5) /
        max(output_rect.zw, float2(1.0, 1.0));
    float2 coordinate = GameMotionRect.xy + uv * GameMotionRect.zw - 0.5;
    float2 minimum = GameMotionRect.xy;
    float2 maximum = GameMotionRect.xy + GameMotionRect.zw - 1.0;
    float2 bounded = clamp(coordinate, minimum, maximum);
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), int2(maximum));
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        GameMotionVectors.Load(int4(top_left, 0, 0)),
        GameMotionVectors.Load(int4(bottom_right.x, top_left.y, 0, 0)),
        fraction.x);
    float2 bottom = lerp(
        GameMotionVectors.Load(int4(top_left.x, bottom_right.y, 0, 0)),
        GameMotionVectors.Load(int4(bottom_right, 0, 0)),
        fraction.x);
    float2 raw = lerp(top, bottom, fraction.y);
    float2 result = raw * GameMotionScale + GameMotionJitterDelta;
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float nvidia_cost_for_pixel(float2 pixel, uint slice, uint view_index) {
    float2 packed_coordinate = flow_input_coordinate(pixel);
    int2 coordinate = int2(round(
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5));
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice);
    return float(FlowAuxiliary.Load(int3(clamp(
        coordinate,
        bounds.minimum,
        bounds.maximum), 0)) & 0xffU);
}

float nvidia_forward_cost_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    bool warped_previous) {
    float2 packed_coordinate = flow_input_coordinate(pixel);
    int2 coordinate = int2(round(
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5));
    FlowGridBounds bounds = flow_grid_bounds(
        warped_previous
            ? PreviousMappings[view_index].TargetRect
            : PreviousMappings[view_index].SourceRect,
        slice);
    return float(ForwardAuxiliary.Load(int3(clamp(
        coordinate,
        bounds.minimum,
        bounds.maximum), 0)) & 0xffU);
}

bool coordinate_inside_rect(float2 coordinate, float4 rect) {
    float2 minimum = rect.xy - 0.5;
    float2 maximum = rect.xy + rect.zw - 0.5;
    return coordinate.x >= minimum.x && coordinate.y >= minimum.y &&
        coordinate.x <= maximum.x && coordinate.y <= maximum.y;
}

float rgb_error(float4 a, float4 b) {
    float3 difference = abs(a.rgb - b.rgb);
    return max(difference.r, max(difference.g, difference.b));
}

float fast_endpoint_confidence(
    float2 pixel, float2 backward, uint slice, bool warped_previous) {
    uint view_idx = active_view_index();
    static const float2 offsets[5] = {
        float2(0, 0), float2(-2, 0), float2(2, 0),
        float2(0, -2), float2(0, 2)
    };
    float error = 0.0;
    bool valid = true;
    [unroll] for (uint i = 0; i < 5; ++i) {
        float2 current = pixel + offsets[i];
        float2 previous = current + backward;
        if (!coordinate_inside_rect(current, PreviousMappings[view_idx].TargetRect)) {
            valid = false;
        }
        float4 previous_color;
        if (warped_previous) {
            CameraSample sample = sample_previous_target(previous, slice, view_idx);
            if (sample.valid < 0.5) {
                valid = false;
            }
            previous_color = sample.color;
        } else {
            if (!coordinate_inside_rect(previous, PreviousMappings[view_idx].SourceRect)) {
                valid = false;
            }
            previous_color = bilinear_previous_source(previous, slice);
        }
        error = max(error, rgb_error(
            bilinear_current_source(current, slice),
            previous_color));
    }
    return valid ? 1.0 - smoothstep(8.0 / 255.0, 48.0 / 255.0, error) : 0.0;
}

bool fast_stationary_patch(float2 pixel, uint slice, float4 current) {
    uint view_idx = active_view_index();
    CameraSample previous = sample_previous_target(pixel, slice, view_idx);
    static const float2 offsets[4] = {
        float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2)
    };
    bool stationary = previous.valid >= 0.5 &&
        rgb_error(previous.color, current) <= 1.0 / 255.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        CameraSample a = sample_previous_target(pixel + offsets[i], slice, view_idx);
        CameraSample b = sample_current_target(pixel + offsets[i], slice, view_idx);
        if (a.valid < 0.5 || b.valid < 0.5 || rgb_error(a.color, b.color) > 1.0 / 255.0) {
            stationary = false;
        }
    }
    return stationary;
}

// Cross-eye stereo-temporal synthesis (alternate-eye stereo reconstruction):
// Projects the target eye pixel into the freshly rendered opposite eye (other_slice)
// using 6-DOF stereo baseline + depth in PreviousMappings[0], checks for stereo
// occlusion against PreviousDepth (which binds the opposite eye's depth buffer),
// and fills disoccluded holes from the temporal history of the same eye (Slice)
// in PreviousFrame via PreviousMappings[1].
float4 synthesize_cross_eye_stereo_pixel(
    float2 pixel,
    uint slice,
    bool use_game_motion_pipeline) {
    float z_target = has_current_depth_flag()
        ? sample_current_linear_depth(pixel)
        : (has_previous_depth_flag() ? sample_previous_linear_depth(pixel) : 2.0);
    uint other_slice = (ArraySize & 0xffffu) > 1u ? (1u - min(slice, 1u)) : slice;
    MappedCoordinate mapped_other = map_target_to_source_with_depth(
        pixel,
        PreviousMappings[0],
        z_target);

    float other_weight = 0.0;
    float4 other_color = float4(0.0, 0.0, 0.0, 1.0);
    if (mapped_other.valid >= 0.5) {
        other_color = bilinear_current_source(mapped_other.coordinate, other_slice);
        other_weight = 1.0;
        if (has_previous_depth_flag() && mapped_other.reprojected_depth > 1.0e-4) {
            float z_other_sampled = sample_previous_linear_depth(mapped_other.coordinate);
            if (z_other_sampled > 1.0e-4) {
                float occlusion_delta = mapped_other.reprojected_depth - z_other_sampled;
                float tolerance = max(0.025 * mapped_other.reprojected_depth, 0.015);
                if (occlusion_delta > tolerance) {
                    other_weight = 1.0 - saturate(
                        (occlusion_delta - tolerance) / max(tolerance * 2.0, 0.01));
                }
            }
        }
    }

    MappedCoordinate mapped_temporal = map_target_to_source_with_depth(
        pixel,
        PreviousMappings[1],
        z_target);
    float2 temporal_coord = mapped_temporal.coordinate;
    if (use_game_motion_pipeline && use_game_motion_flag() && repeated_capture_flag() == 0u) {
        float2 mv = game_motion_for_pixel(pixel, slice, 1u);
        temporal_coord = pixel + mv;
    }
    bool temporal_valid = mapped_temporal.valid >= 0.5 &&
        coordinate_inside_rect(temporal_coord, PreviousMappings[1].SourceRect);
    float4 temporal_color = temporal_valid
        ? bilinear_previous_source(temporal_coord, slice)
        : other_color;

    float4 result = saturate(lerp(temporal_color, other_color, other_weight));
    if (!temporal_valid && mapped_other.valid >= 0.5) {
        result = saturate(other_color);
    } else if (mapped_other.valid < 0.5 && temporal_valid) {
        result = saturate(temporal_color);
    }
    return result;
}

struct FullscreenVertex {
    float4 position : SV_Position;
};

FullscreenVertex FullscreenTriangleVS(uint vertex_id : SV_VertexID) {
    FullscreenVertex output;
    float2 position = vertex_id == 0 ? float2(-1.0, -1.0)
                     : vertex_id == 1 ? float2(-1.0, 3.0)
                                      : float2(3.0, -1.0);
    output.position = float4(position, 0.0, 1.0);
    return output;
}

float4 synthesize_midpoint(
    FullscreenVertex input,
    float flow_value_scale,
    bool use_nvidia_cost,
    bool use_nvidia_bidirectional,
    bool validate_fast,
    bool use_game_motion_pipeline,
    bool input_pose_compensated) {
    float4 output_color = float4(0.0, 0.0, 0.0, 1.0);
    uint2 integer_pixel = uint2(input.position.xy);
    uint slice_idx = Slice & 0xffffu;
    uint array_count = ArraySize & 0xffffu;
    uint view_idx = active_view_index();
    bool in_bounds = integer_pixel.x < Width && integer_pixel.y < Height &&
        slice_idx < array_count;
    if (in_bounds) {
        float2 pixel = float2(integer_pixel);
        if (use_game_motion_pipeline && cross_eye_stereo_mode_flag()) {
            output_color = synthesize_cross_eye_stereo_pixel(
                pixel, slice_idx, use_game_motion_pipeline);
        } else {
        float frac = synthesis_fraction();
        bool extrapolating = asw_extrapolation_mode_flag();
        float pixel_linear_depth =
            (enable_6dof_parallax_flag() && has_current_depth_flag())
                ? sample_current_linear_depth(pixel)
                : 0.0;
        MappedCoordinate previous_coverage = map_target_to_source_with_depth(
            pixel,
            PreviousMappings[view_idx],
            pixel_linear_depth);
        float2 asw_cam_shift = float2(0.0, 0.0);
        if ((SynthesisFlags & (1u << 26)) != 0u) {
            if (asw_single_source_flag()) {
                // Meta Single-Source forward mode: the current frame is already the target.
                // No artificial cam_shift needed because ATW reprojection aligns the pose.
                asw_cam_shift = float2(0.0, 0.0);
            } else {
                float asw_scale = (SynthesisFlags & (1u << 28)) != 0u
                    ? (-frac / max(1.0 - frac, 0.25))
                    : ((SynthesisFlags & (1u << 27)) != 0u
                           ? 1.0
                           : saturate((frac - 1.0) / max(frac, 1.0)));
                asw_cam_shift = (previous_coverage.coordinate - pixel) * asw_scale;
            }
        }
        CameraSample current_fallback = sample_current_target_with_asw(
            pixel,
            slice_idx,
            view_idx,
            asw_cam_shift);
        output_color = saturate(current_fallback.color);
        bool scene_changed = repeated_capture_flag() == 0 && !use_game_motion_pipeline &&
            !use_nvidia_cost &&
            (FlowAuxiliary.Load(int3(1, 0, 0)) & 0x0fU) != 0;
        if (previous_coverage.valid >= 0.5 && !scene_changed) {
            float4 previous_coverage_color = bilinear_previous_source(
                previous_coverage.coordinate,
                slice_idx);
            bool preserve_stationary = validate_fast && repeated_capture_flag() == 0 &&
                current_fallback.valid >= 0.5 &&
                fast_stationary_patch(pixel, slice_idx, current_fallback.color);
            if (preserve_stationary) {
                output_color = asw_single_source_flag()
                    ? saturate(current_fallback.color)
                    : saturate(lerp(
                        previous_coverage_color,
                        current_fallback.color,
                        saturate(frac)));
            } else {
                float2 raw_backward = float2(0.0, 0.0);
                CameraSample previous_sample = (CameraSample)0;
                CameraSample current_sample = (CameraSample)0;
                bool endpoints_valid = true;
                float2 resolved_current_endpoint = pixel;
                float2 resolved_previous_endpoint = previous_coverage.coordinate;

                if (use_game_motion_pipeline) {
                    float2 current_endpoint = pixel;
                    MappedCoordinate previous_endpoint;
                    [unroll] for (uint iteration = 0; iteration < 3; ++iteration) {
                        raw_backward = repeated_capture_flag() != 0
                            ? float2(0.0, 0.0)
                            : game_motion_for_pixel(
                                current_endpoint, slice_idx, view_idx);
                        previous_endpoint = map_source_to_target_with_depth(
                            current_endpoint + raw_backward,
                            PreviousMappings[view_idx],
                            pixel_linear_depth);
                        if (previous_endpoint.valid < 0.5) {
                            endpoints_valid = false;
                            break;
                        }
                        float2 target_displacement =
                            previous_endpoint.coordinate - current_endpoint;
                        current_endpoint =
                            pixel - (1.0 - frac) * target_displacement;
                    }

                    // Depth-ordered foreground horizon search:
                    // When hardware depth is bound, check whether a closer foreground surface in
                    // CurrentFrame crosses `pixel` at time `frac`, or whether `current_endpoint`
                    // was falsely pulled off a background pixel by an arriving foreground object.
                    if (has_current_depth_flag() && repeated_capture_flag() == 0) {
                        float2 cam_shift = previous_coverage.coordinate - pixel;
                        float best_depth = endpoints_valid
                            ? sample_current_linear_depth(current_endpoint)
                            : 1.0e6;
                        if (endpoints_valid) {
                            float2 verify_mv = game_motion_for_pixel(
                                current_endpoint, slice_idx, view_idx);
                            float2 verify_disp = verify_mv - cam_shift;
                            float2 landed = current_endpoint + (1.0 - frac) * verify_disp;
                            if (length(landed - pixel) > 2.5) {
                                current_endpoint = pixel;
                                best_depth = pixel_linear_depth;
                            }
                        }

                        static const float2 probe_offsets[6] = {
                            float2(-16.0, 0.0), float2(16.0, 0.0),
                            float2(-8.0, 0.0), float2(8.0, 0.0),
                            float2(0.0, -12.0), float2(0.0, 12.0)
                        };
                        [loop] for (uint p = 0; p < 6; ++p) {
                            float2 probe_pos = pixel + probe_offsets[p];
                            if (coordinate_inside_rect(probe_pos, PreviousMappings[view_idx].TargetRect)) {
                                float2 mv_p = game_motion_for_pixel(probe_pos, slice_idx, view_idx);
                                float2 disp_p = mv_p - cam_shift;
                                float2 cand_curr = pixel - (1.0 - frac) * disp_p;
                                if (coordinate_inside_rect(cand_curr, PreviousMappings[view_idx].TargetRect)) {
                                    float2 mv_cand = game_motion_for_pixel(cand_curr, slice_idx, view_idx);
                                    float2 disp_cand = mv_cand - cam_shift;
                                    float2 landed_cand = cand_curr + (1.0 - frac) * disp_cand;
                                    float cand_depth = sample_current_linear_depth(cand_curr);
                                    if (length(landed_cand - pixel) <= 2.0 &&
                                        cand_depth > 1.0e-4 &&
                                        cand_depth < best_depth * 0.95) {
                                        best_depth = cand_depth;
                                        current_endpoint = cand_curr;
                                        endpoints_valid = true;
                                    }
                                }
                            }
                        }
                    }

                    if (endpoints_valid) {
                        raw_backward = repeated_capture_flag() != 0
                            ? float2(0.0, 0.0)
                            : game_motion_for_pixel(
                                current_endpoint, slice_idx, view_idx);
                        previous_endpoint = map_source_to_target_with_depth(
                            current_endpoint + raw_backward,
                            PreviousMappings[view_idx],
                            pixel_linear_depth);
                        endpoints_valid = previous_endpoint.valid >= 0.5;
                    }
                    if (endpoints_valid) {
                        resolved_current_endpoint = current_endpoint;
                        resolved_previous_endpoint = previous_endpoint.coordinate;
                        previous_sample = sample_previous_target_with_depth(
                            previous_endpoint.coordinate,
                            slice_idx,
                            view_idx,
                            pixel_linear_depth);
                        current_sample = sample_current_target_with_asw(
                            current_endpoint,
                            slice_idx,
                            view_idx,
                            asw_cam_shift);
                    }
                } else {
                    raw_backward = repeated_capture_flag() != 0
                        ? float2(0.0, 0.0)
                        : flow_for_pixel(
                            pixel,
                            slice_idx,
                            view_idx,
                            flow_value_scale,
                            use_nvidia_cost);
                    float2 residual_backward = input_pose_compensated
                        ? raw_backward
                        : raw_backward - (previous_coverage.coordinate - pixel);
                    resolved_previous_endpoint = pixel + residual_backward * frac;
                    resolved_current_endpoint = pixel - residual_backward * (1.0 - frac);
                    previous_sample = sample_previous_target_with_depth(
                        resolved_previous_endpoint,
                        slice_idx,
                        view_idx,
                        pixel_linear_depth);
                    current_sample = sample_current_target_with_asw(
                        resolved_current_endpoint,
                        slice_idx,
                        view_idx,
                        asw_cam_shift);
                }

                if (asw_single_source_flag()) {
                    // Meta SpaceWarp Single-Source Forward Mode:
                    // Projette UNIQUEMENT la frame la plus recente deployee vers l'avant.
                    // N'echantillonne JAMAIS PreviousFrame pour eviter tout ghosting ou effacement de texte.
                    if (current_sample.valid >= 0.5) {
                        float luma_fallback = dot(current_fallback.color.rgb, float3(0.2126, 0.7152, 0.0722));
                        float luma_warp = dot(current_sample.color.rgb, float3(0.2126, 0.7152, 0.0722));
                        // Anti-mangeur de texte / HUD : si le pixel de base est du texte blanc / clair (luma > 0.6)
                        // et que le flux optique du fond l'a etire vers un fond sombre, preserver le texte net !
                        if (luma_fallback > 0.60 && luma_warp < luma_fallback * 0.55) {
                            output_color = saturate(current_fallback.color);
                        } else {
                            output_color = saturate(current_sample.color);
                        }
                    } else {
                        output_color = saturate(current_fallback.color);
                    }
                } else if (endpoints_valid) {
                    if (previous_sample.valid >= 0.5 && current_sample.valid >= 0.5) {
                        if (extrapolating) {
                            float disagreement_ext = rgb_error(previous_sample.color, current_sample.color);
                            float conf_ext = saturate(1.0 - disagreement_ext * 4.0);
                            output_color = saturate(lerp(
                                current_fallback.color,
                                current_sample.color,
                                conf_ext));
                        } else {
                            float4 flow_midpoint = lerp(
                                previous_sample.color, current_sample.color, frac);
                            float disagreement = max(
                                abs(previous_sample.color.r - current_sample.color.r),
                                max(
                                    abs(previous_sample.color.g - current_sample.color.g),
                                    abs(previous_sample.color.b - current_sample.color.b)));
                            float confidence = saturate(1.0 - disagreement * 6.0);
                            float4 stable_midpoint = lerp(
                                previous_coverage_color,
                                current_fallback.color,
                                frac);

                            // Depth-guided disocclusion resolution:
                            // When previous_sample and current_sample disagree on a disoccluded
                            // background strip behind a moving foreground object, select the
                            // unoccluded background (larger linear depth Z) rather than blending
                            // foreground and background into a double-edge ghost.
                            if (has_current_depth_flag() && has_previous_depth_flag() &&
                                disagreement > 0.05) {
                                float z_curr = sample_current_linear_depth(resolved_current_endpoint);
                                float z_prev = sample_previous_linear_depth(resolved_previous_endpoint);
                                if (z_curr > z_prev * 1.05) {
                                    // Previous frame was occluded by departing foreground; Current is unoccluded background.
                                    flow_midpoint = current_sample.color;
                                    confidence = 1.0;
                                } else if (z_prev > z_curr * 1.05) {
                                    // Current frame is occluded by arriving foreground; Previous is unoccluded background.
                                    flow_midpoint = previous_sample.color;
                                    confidence = 1.0;
                                }
                            }

                            if (use_nvidia_cost) {
                                float consistency_confidence = 1.0;
                                float cost_confidence = 1.0;
                                if (repeated_capture_flag() == 0) {
                                    if (validate_fast) {
                                        confidence *= fast_endpoint_confidence(
                                            pixel, raw_backward, slice_idx, input_pose_compensated);
                                    }
                                    if (use_nvidia_bidirectional) {
                                        float2 previous_coordinate = pixel + raw_backward;
                                        if (coordinate_inside_rect(
                                                previous_coordinate,
                                                input_pose_compensated
                                                    ? PreviousMappings[view_idx].TargetRect
                                                    : PreviousMappings[view_idx].SourceRect)) {
                                            float2 raw_forward = forward_flow_for_pixel(
                                                previous_coordinate,
                                                slice_idx,
                                                view_idx,
                                                flow_value_scale,
                                                input_pose_compensated);
                                            float cycle_error = length(
                                                raw_backward + raw_forward);
                                            consistency_confidence =
                                                1.0 - smoothstep(2.0, 6.0, cycle_error);
                                            float maximum_cost = max(
                                                nvidia_cost_for_pixel(
                                                    pixel,
                                                    slice_idx,
                                                    view_idx),
                                                nvidia_forward_cost_for_pixel(
                                                    previous_coordinate,
                                                    slice_idx,
                                                    view_idx,
                                                    input_pose_compensated));
                                            cost_confidence = saturate(
                                                1.0 - maximum_cost / 255.0);
                                        } else {
                                            consistency_confidence = 0.0;
                                            cost_confidence = 0.0;
                                        }
                                    } else {
                                        cost_confidence = saturate(
                                            1.0 - nvidia_cost_for_pixel(
                                                pixel,
                                                slice_idx,
                                                view_idx) / 255.0);
                                    }
                                }
                                confidence *= consistency_confidence * cost_confidence;
                                output_color = saturate(lerp(
                                    stable_midpoint,
                                    flow_midpoint,
                                    confidence));
                            } else {
                                output_color = saturate(lerp(
                                    stable_midpoint,
                                    flow_midpoint,
                                    confidence));
                            }
                        }
                    } else if (extrapolating && current_sample.valid >= 0.5) {
                        output_color = saturate(current_sample.color);
                    }
                }
            }
        }
        }
    }
    return output_color;
}

float4 SynthesizeMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0, false, false, false, false, false);
}

float4 SynthesizeGameMotionMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0, false, false, false, true, false);
}

// The NVIDIA variants read flow in signed S10.5 fixed point, and from a
// previous input the pack warped into the current camera (PackNvidiaFlowInput).
float4 SynthesizeNvidiaMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, false, false, false, true);
}

float4 SynthesizeNvidiaBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, true, false, false, true);
}

float4 SynthesizeNvidiaFastMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, false, true, false, true);
}

float4 SynthesizeNvidiaFastBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0 / 32.0, true, true, true, false, true);
}
