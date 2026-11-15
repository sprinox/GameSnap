Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer Parameters : register(b0) {
    uint2 SourceSize;
    uint2 DrawSize;
    uint2 Offset;
    uint IsHdr;
    float UiWhiteScale;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= 320 || tid.y >= 180)
        return;
    if (tid.x < Offset.x || tid.y < Offset.y ||
        tid.x >= Offset.x + DrawSize.x || tid.y >= Offset.y + DrawSize.y) {
        float3 background = float3(45, 48, 58) / 255.0;
        if (IsHdr)
            background = pow((background + 0.055) / 1.055, 2.4) * UiWhiteScale;
        Output[tid.xy] = float4(background, 1);
        return;
    }
    // 与原预览相同的最近邻/等比 letterbox，只取样，不做 PQ、spline、曝光或高光裁切。
    const uint2 pixel = min(SourceSize - 1, (tid.xy - Offset) * SourceSize / DrawSize);
    Output[tid.xy] = float4(Source.Load(int3(pixel, 0)).rgb, 1);
}
