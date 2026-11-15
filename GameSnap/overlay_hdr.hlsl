// ImGui DX11 顶点接口。HDR 交换链为线性 scRGB，普通 UI 需要 sRGB 解码；
// HDR 缩略图已经是 scRGB，只乘 tint，不得再次 gamma/PQ 或做 tone map。
Texture2D Texture : register(t0);
SamplerState Sampler : register(s0);
cbuffer Parameters : register(b0) {
    uint IsThumbnail;
    float UiWhiteScale;
    float2 Padding;
};

float3 srgb_to_linear(float3 value) {
    // 编译器可能先计算两个分支：只保护 pow 的底数，线性负值分支和 HDR 图片原值不裁切。
    const float3 nonlinear = pow(max((value + 0.055) / 1.055, 0.0), 2.4);
    return float3(value.r <= 0.04045 ? value.r / 12.92 : nonlinear.r,
                  value.g <= 0.04045 ? value.g / 12.92 : nonlinear.g,
                  value.b <= 0.04045 ? value.b / 12.92 : nonlinear.b);
}

float4 main(float4 position : SV_POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target {
    const float4 sampled = Texture.Sample(Sampler, uv);
    const float alpha = sampled.a * color.a;
    const float3 rgb = IsThumbnail
        ? sampled.rgb * srgb_to_linear(color.rgb)
        : srgb_to_linear(sampled.rgb * color.rgb) * UiWhiteScale;
    // ImGui blend state 使用 SRC_ALPHA，混合后生成 DirectComposition 所需的预乘 alpha。
    return float4(rgb, alpha);
}
