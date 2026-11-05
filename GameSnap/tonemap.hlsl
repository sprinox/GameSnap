Texture2D<float4> Hdr : register(t0);
RWTexture2D<float4> Out : register(u0);
RWTexture2D<uint4> HdrOut : register(u1);

cbuffer Params : register(b0)
{
    uint Width;
    uint Height;
    float SdrWhite;
    float PeakNits;
    uint Pq;
    uint Algo;
    float2 Pad;
};

float pq_to_nits(float p)
{
    p = saturate(p);
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float p2 = pow(p, 1.0 / m2);
    float num = max(p2 - c1, 0.0);
    float den = c2 - c3 * p2;
    if (den <= 0.0) return 0.0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

float3 pq_to_nits3(float3 p)
{
    return float3(pq_to_nits(p.r), pq_to_nits(p.g), pq_to_nits(p.b));
}

// nits → ST 2084 PQ 码值；和 CPU 端 nits_to_pq 保持一致。
float nits_to_pq(float nits)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float3 nits_to_pq3(float3 nits)
{
    return float3(nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b));
}

float3 bt2020_to_709(float3 c)
{
    return float3(dot(c, float3(1.6605, -0.5876, -0.0728)),
                  dot(c, float3(-0.1246, 1.1329, -0.0083)),
                  dot(c, float3(-0.0182, -0.1006, 1.1187)));
}

float3 srgb_to_bt2020(float3 c)
{
    return float3(dot(c, float3(0.6274, 0.3293, 0.0433)),
                  dot(c, float3(0.0691, 0.9195, 0.0114)),
                  dot(c, float3(0.0164, 0.0880, 0.8956)));
}

float3 linear_to_srgb(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}

// 单调三次 Hermite shoulder：
//   x <= knee 时线性；knee..peak 用 Hermite 过渡到 1。
// SDR 白点固定落在 0.8 左右，保留一点高光层次。
float spline_shoulder(float x)
{
    const float knee = saturate(SdrWhite / max(PeakNits, 1.0));
    const float white = 0.8;
    if (x <= 0.0) return 0.0;
    if (x <= knee) return x * (white / max(knee, 1e-4));
    if (x >= 1.0) return 1.0;

    float t = (x - knee) / max(1.0 - knee, 1e-4);
    float h00 = 2.0 * t * t * t - 3.0 * t * t + 1.0;
    float h10 = t * t * t - 2.0 * t * t + t;
    float h01 = -2.0 * t * t * t + 3.0 * t * t;
    const float slope0 = (white / max(knee, 1e-4)) * (1.0 - knee);
    return h00 * white + h10 * slope0 + h01;
}

// analysis 的简化版：按亮度做 false color，方便看 HDR 分布。
float3 analysis_debug(float nits)
{
    if (nits < 1.0) return float3(0.02, 0.02, 0.15);
    if (nits < 10.0) return float3(0.0, 0.0, 1.0);
    if (nits < 100.0) return float3(0.0, 1.0, 0.0);
    if (nits < 300.0) return float3(1.0, 1.0, 0.0);
    if (nits < 1000.0) return float3(1.0, 0.4, 0.0);
    return float3(1.0, 0.0, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Width || tid.y >= Height)
        return;

    float3 hdr = Hdr.Load(int3(tid.xy, 0)).rgb;
    float3 nits2020;
    uint3 pq_code = uint3(0, 0, 0);
    if (Pq)
    {
        // PQ 输入：HDR 文件直接复用原始 10-bit 码值，避免 nits→PQ 的恒等往返。
        pq_code = uint3(round(saturate(hdr) * 1023.0));
        nits2020 = pq_to_nits3(hdr);
    }
    else
    {
        nits2020 = srgb_to_bt2020(hdr * 80.0);
    }
    nits2020 = max(nits2020, float3(0.0, 0.0, 0.0));
    if (!Pq)
        pq_code = uint3(round(saturate(nits_to_pq3(nits2020)) * 1023.0));

    // 同一个 kernel 里输出 10-bit PQ / BT.2020，供 AVIF/HEIC 直接使用。
    HdrOut[tid.xy] = uint4(pq_code, 1023);

    float3 nits = bt2020_to_709(nits2020);
    float luma = dot(nits, float3(0.2126, 0.7152, 0.0722));
    if (Algo == 1) {
        Out[tid.xy] = float4(analysis_debug(luma), 1.0);
        return;
    }

    float mapped = spline_shoulder(luma / max(PeakNits, 1.0));
    float3 sdr = float3(0.0, 0.0, 0.0);
    if (luma > 1e-6)
        sdr = nits * (mapped / luma);
    // 直接按 BGRA 写出，CPU 侧可以整行 memcpy，省一次全帧 swizzle。
    Out[tid.xy] = float4(linear_to_srgb(sdr).bgr, 1.0);
}
