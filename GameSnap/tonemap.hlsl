Texture2D<float4> Hdr : register(t0);
StructuredBuffer<uint> Peak : register(t1);   // 本帧峰值（10-bit PQ 码值），由 peak pass 写入
RWTexture2D<float4> Out : register(u0);
RWTexture2D<float4> HdrOut : register(u1);
RWStructuredBuffer<uint> PeakOut : register(u2);

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

// ── 色调映射：照 libplacebo（mpv 的 --tone-mapping=spline）────────────────────
// 1) 线性 RGB 换到 IPT：只对感知亮度 I 做 tone map，色度用 hull 曲线收缩；
// 2) tone map 本身是 libplacebo 的 "spline"：PQ 域、单支点、双曲段
//    y = slope*(x-piv)/(1+Qk*(x-piv)) + piv（肩部） / 线性（趾部）。
// 参数按 libplacebo 默认（knee_default=0.7、spline_contrast=0.5、slope_tuning=1），
// 没有 HDR 元数据时 st2094_pick_knee 退化成“支点取输入 PQ 范围 70% 且输入输出同一支点”，
// 即 slope=1、Pk=0，正是下面用的形式。

static const float3x3 kRgbToLms709 = {
    0.2957413, 0.6230997, 0.0811802,
    0.1561450, 0.7272835, 0.1165603,
    0.0350734, 0.1565968, 0.8082766,
};
static const float3x3 kLmsToIpt = {
    0.4000000, 0.4000000, 0.2000000,
    4.4550000, -4.8510000, 0.3960000,
    0.8056000, 0.3572000, -1.1628000,
};
static const float3x3 kIptToLms = {
    1.0000000, 0.0975689, 0.2052260,
    1.0000000, -0.1138760, 0.1332170,
    1.0000000, 0.0326151, -0.6768870,
};
static const float3x3 kLmsToRgb709 = {
    6.1728843, -5.3203218, 0.1472545,
    -1.3234594, 2.5597126, -0.2362089,
    -0.0114500, -0.2650588, 1.2765739,
};

// libplacebo 的 spline 查找表：输入 PQ 码值，输出 PQ 码值。
// 支点由 st2094_pick_knee 给出：无 HDR 元数据时 src_pivot = mix(in_min, in_max, 0.7)，
// dst_pivot = mix(out_min, out_max, 0.7)，斜率为两者之比，再按 spline_contrast 微调；
// 肩部/趾部是双曲段 y = slope*(x-piv)/(1+k*(x-piv)) + dst_piv。
float tone_map_spline(float x, float in_max)
{
    const float in_min = 0.0;
    const float out_min = 0.0;
    const float out_max = nits_to_pq(max(SdrWhite, 1.0));

    // st2094_pick_knee（默认 knee=0.7，无场景平均亮度）
    const float src_pivot = lerp(in_min, in_max, 0.7);
    float dst_pivot = lerp(out_min, out_max, 0.7);
    dst_pivot = clamp(dst_pivot, lerp(out_min, out_max, 0.1), lerp(out_min, out_max, 0.9));

    // slope_tuning=1、slope_offset=0、spline_contrast=0.5
    float slope = (dst_pivot - out_min) / max(src_pivot - in_min, 1e-6);
    const float ratio = clamp(in_max / max(out_max, 1e-6) - 1.0, 0.0, 1.0);
    slope = pow(max(slope, 1e-6), (1.0 - 0.5) * ratio);

    const float in_span_min = in_min - src_pivot;
    const float in_span_max = in_max - src_pivot;
    const float out_span_min = out_min - dst_pivot;
    const float out_span_max = out_max - dst_pivot;
    const float pk = (slope * in_span_min - out_span_min) / (out_span_min * in_span_min);
    const float qk = (slope * in_span_max - out_span_max) / (out_span_max * in_span_max);

    const float d = x - src_pivot;
    const float y = slope * d / (1.0 + (d > 0.0 ? qk : pk) * d) + dst_pivot;
    return clamp(y, out_min, out_max);
}

// 线性 RGB（相对 100 nit 白）→ SDR 线性 RGB（显示白 = 1.0）。
float3 tone_map_ipt(float3 rgb, float in_max)
{
    // 线性 → LMS → PQ → IPT
    const float3 lms = mul(kRgbToLms709, rgb);
    const float3 lmspq = nits_to_pq3(lms * 100.0);
    float3 ipt = mul(kLmsToIpt, lmspq);

    const float i_orig = ipt.x;
    ipt.x = tone_map_spline(i_orig, in_max);

    // libplacebo：提亮时别把饱和度一起抬起来，压暗时按色域体积收缩色度。
    float2 hull = float2(i_orig, ipt.x);
    hull = ((hull - 6.0) * hull + 9.0) * hull;
    ipt.yz *= min(i_orig / max(ipt.x, 1e-6), hull.y / max(hull.x, 1e-9));

    // IPT → PQ → LMS → 线性
    const float3 lmspq2 = mul(kIptToLms, ipt);
    const float3 lms2 = pq_to_nits3(saturate(lmspq2)) * 0.01;
    return mul(kLmsToRgb709, lms2);
}

// analysis 的 false color：400 nit 以下按亮度做灰阶，400 nit 以上按对数刻度把色相
// 从蓝(240°)连续过渡到红(0°)，中间没有任何分级色带。
float3 hsv_to_rgb(float3 hsv)
{
    const float3 p = abs(frac(hsv.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return hsv.z * lerp(float3(1.0, 1.0, 1.0), saturate(p - 1.0), hsv.y);
}

float3 analysis_colormap(float nits)
{
    const float knee = 400.0;                       // 灰/彩分界
    const float top = 10000.0;                      // 色相走到红的上限
    const float gray = pow(saturate(nits / knee), 1.0 / 2.2) * 0.6;   // 暗部也能看清
    const float3 gray_rgb = float3(gray, gray, gray);

    // 400 → 10000 nit：对数刻度（人眼与显示亮度都是对数感知），色相 240° → 0°。
    // 用 saturate + lerp 而不是 if/return：整条曲线处处连续，也不会有分支发散。
    const float t = saturate(log2(max(nits, 1e-4) / knee) / log2(top / knee));
    const float3 color = hsv_to_rgb(float3((240.0 - 240.0 * t) / 360.0, 1.0, 1.0));
    // 分界处 50 nit 内与灰阶插值，避免硬跳变
    const float blend = saturate((nits - knee) / 50.0);
    return lerp(gray_rgb, color, blend);
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
    HdrOut[tid.xy] = float4(float3(pq_code) / 1023.0, 1.0);

    float3 nits = bt2020_to_709(nits2020);
    float luma = dot(nits, float3(0.2126, 0.7152, 0.0722));
    if (Algo == 1) {
        // CPU 侧按 BGRA 读回，调试路径也必须按 BGRA 写出，否则红蓝互换。
        Out[tid.xy] = float4(analysis_colormap(luma).bgr, 1.0);
        return;
    }

    // 本帧峰值：peak pass 已经把 10-bit PQ 码值写进 Peak[0]，这里只做上限保护。
    const float peak_pq = min(Peak[0] / 1023.0, nits_to_pq(max(PeakNits, 1.0)));
    const float in_max = max(nits_to_pq(max(SdrWhite, 1.0)), peak_pq);

    // tone map 走 libplacebo 的 IPT + spline + hull 这条链；输出线性 RGB（显示白 = 1.0）。
    // tone_map_ipt 返回的是相对 100 nit 白的值，这里按显示器 SDR 白点归一化。
    const float3 sdr = tone_map_ipt(nits * 0.01, in_max) * (100.0 / max(SdrWhite, 1.0));
    // 直接按 BGRA 写出，CPU 侧可以整行 memcpy，省一次全帧 swizzle。
    Out[tid.xy] = float4(linear_to_srgb(sdr).bgr, 1.0);
}

// ── 峰值归约 ─────────────────────────────────────────────────────────────────
// 每个 8x8 线程组先在组内取最大，再由组 0 号线程做一次全局 InterlockedMax，
// 避免 8.3M 次原子操作。结果留在 GPU 上，不回读 CPU（tone map 直接当 SRV 读）。
groupshared uint g_peak;

[numthreads(8, 8, 1)]
void peak(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
        g_peak = 0;
    GroupMemoryBarrierWithGroupSync();

    uint v = 0;
    if (tid.x < Width && tid.y < Height)
    {
        const float3 hdr = Hdr.Load(int3(tid.xy, 0)).rgb;
        const float3 nits = Pq ? pq_to_nits3(hdr) : srgb_to_bt2020(hdr * 80.0);
        const float m = max(max(nits.r, nits.g), nits.b);
        v = uint(round(saturate(nits_to_pq(max(m, 0.0))) * 1023.0));
    }
    InterlockedMax(g_peak, v);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
        InterlockedMax(PeakOut[0], g_peak);
}
