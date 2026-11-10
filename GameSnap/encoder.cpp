#include "pch.h"

#include "encoder.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>

#include "codecs.h"
#include "d3d.h"
#include "simd.h"
#include "tonemap.h"

namespace gamesnap {
    namespace {

        // 去掉 Windows 文件名里不允许的字符；空标题回退到 GameSnap。
        // 按值接收，直接原地修改后返回，省掉一次拷贝。
        std::wstring sanitize_filename(std::wstring title, std::size_t max_length = 80, bool fallback = true) {
            for (wchar_t& ch : title) {
                const bool invalid = ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
                                     ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*';
                if (invalid)
                    ch = L'_';
            }
            const auto trim = [](std::wstring& text) {
                const auto first = text.find_first_not_of(L" .");  // 开头和结尾的空格、点全部删掉
                const auto last = text.find_last_not_of(L" .");
                if (first == std::wstring::npos)
                    text.clear();
                else
                    text = text.substr(first, last - first + 1);
            };
            trim(title);

            if (title.size() > max_length) {
                title.resize(max_length);
                // 不要把 UTF-16 代理对截成半个字符。
                if (!title.empty() && title.back() >= 0xD800 && title.back() <= 0xDBFF)
                    title.pop_back();
                // 截断后可能正好停在空格/点上，Windows 建文件时会把它们去掉，
                // 那样 UI 显示的名字和真实文件名就对不上了，所以再 trim 一次。
                trim(title);
            }
            if (title.empty() && fallback)
                title = L"GameSnap";
            return title;
        }

        // CON / PRN / AUX / NUL / COM1-9 / LPT1-9 是设备名：拿它们当文件名会写到设备上，
        // 或者干脆失败。模板是用户可控的，这里兜底加下划线。
        // 注意 Windows 只看**第一个点之前**的部分，所以不能简单在末尾补 "_"：
        // "CON.txt" + "_" 还是 CON 设备名；这里改成在前面加 "_"。
        std::wstring avoid_reserved_device_name(std::wstring name) {
            std::wstring stem = name;
            if (const auto dot = stem.find(L'.'); dot != std::wstring::npos)
                stem.resize(dot);
            std::ranges::transform(stem, stem.begin(),
                                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

            bool reserved = false;
            constexpr std::wstring_view kFixed[] = {L"con", L"prn", L"aux", L"nul"};
            for (const std::wstring_view fixed : kFixed)
                reserved = reserved || stem == fixed;
            if (!reserved && stem.size() == 4) {
                const std::wstring_view prefix = std::wstring_view(stem).substr(0, 3);
                const wchar_t digit = stem[3];
                reserved = (prefix == L"com" || prefix == L"lpt") && digit >= L'1' && digit <= L'9';
            }
            if (reserved)
                name.insert(0, L"_");
            return name;
        }

        // 文件名：<标题>_YYYYMMDD_HHMMSS_mmm_序号[_HDR|_SDR].<ext>
        std::string exif_date_time(Clock::time_point time) {
            const auto local = std::chrono::zoned_time(std::chrono::current_zone(), time).get_local_time();
            const auto days = std::chrono::floor<std::chrono::days>(local);
            const std::chrono::year_month_day date{days};
            const std::chrono::hh_mm_ss clock{local - days};
            return fmt::format("{:04}:{:02}:{:02} {:02}:{:02}:{:02}",
                               static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                               static_cast<unsigned>(date.day()), clock.hours().count(),
                               clock.minutes().count(), clock.seconds().count());
        }

        // 输出文件的基本名，由 filename_pattern 决定（不含 _HDR/_SDR 与扩展名）：
        //   {title}   前台窗口标题
        //   {exe}     前台进程 exe 名（去扩展名）
        //   {process} 前台进程显示名（版本信息 FileDescription，取不到时等于 exe 名）
        //   {time}    YYYYMMDD_HHMMSS_mmm
        //   {seq}     截图序号（4 位）
        // 未知的 {…} 原样保留；替换后整体再清洗一次（非法字符、首尾空格、长度）。
        // 只对模板里出现的占位符做替换：{time} 要查时区，没用到就不查。
        std::wstring make_base_name(std::wstring const& title, std::wstring const& process_exe,
                                    std::wstring const& process_name, Clock::time_point time,
                                    std::uint64_t sequence, std::wstring const& pattern) {
            // 单遍扫描模板：替换进去的值直接追加到输出，不会再被后面的占位符命中
            // （否则标题里字面的 "{time}" 会被当成占位符二次替换）。
            const std::wstring& source = pattern.empty() ? std::wstring{L"{title}_{time}_{seq}"} : pattern;

            std::wstring time_text;  // 只有真的遇到 {time} 才去查时区/格式化
            const auto time_stamp = [&]() -> std::wstring const& {
                if (!time_text.empty())
                    return time_text;
                // fmt 没有 zoned_time 的 formatter，用 C++20 chrono 拆出本地日期/时间再格式化。
                const auto local = std::chrono::zoned_time(std::chrono::current_zone(), time).get_local_time();
                const auto days = std::chrono::floor<std::chrono::days>(local);
                const std::chrono::year_month_day date{days};
                const std::chrono::hh_mm_ss clock{local - days};
                const auto millis =
                    std::chrono::duration_cast<std::chrono::milliseconds>(local.time_since_epoch()).count() % 1000;
                time_text = utf8_to_wstring(fmt::format("{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}",
                                                        static_cast<int>(date.year()),
                                                        static_cast<unsigned>(date.month()),
                                                        static_cast<unsigned>(date.day()), clock.hours().count(),
                                                        clock.minutes().count(), clock.seconds().count(), millis));
                return time_text;
            };

            std::wstring name;
            name.reserve(source.size() + 32);
            for (std::size_t index = 0; index < source.size();) {
                const std::wstring_view rest = std::wstring_view(source).substr(index);
                // 值用 lambda 传：只有 key 命中才求值。否则像 "{title}_{seq}" 这种模板
                // 在检查 {time} 时会先调用 time_stamp()，变成“没写 {time} 也查时区”。
                const auto take = [&](std::wstring_view key, auto&& make_value) {
                    if (rest.size() < key.size() || rest.substr(0, key.size()) != key)
                        return false;
                    name += make_value();
                    index += key.size();
                    return true;
                };
                if (rest.front() == L'{') {
                    if (take(L"{title}", [&] { return sanitize_filename(title, 80, false); }))
                        continue;
                    if (take(L"{exe}", [&] { return sanitize_filename(process_exe, 60, false); }))
                        continue;
                    if (take(L"{process}", [&] { return sanitize_filename(process_name, 60, false); }))
                        continue;
                    if (take(L"{time}", [&]() -> std::wstring const& { return time_stamp(); }))
                        continue;
                    if (take(L"{seq}", [&] {
                            return utf8_to_wstring(
                                fmt::format("{:04}", static_cast<unsigned>(sequence % 10000U)));
                        }))
                        continue;
                }
                name += source[index++];
            }

            // 整体清洗：模板里可能出现 < > : 等非法字符，也可能替换后变成空串。
            name = sanitize_filename(std::move(name), 180, true);
            return avoid_reserved_device_name(std::move(name));
        }

        std::filesystem::path make_output_path(std::filesystem::path const& dir, std::wstring const& base_name,
                                               std::wstring_view extension, std::wstring_view suffix) {
            std::wstring name = base_name;
            if (!suffix.empty()) {
                name += L"_";
                name += suffix;
            }
            name += extension;
            return dir / name;
        }

        // ── PQ ───────────────────────────────────────────────────────────────
        // ST 2084 (PQ) EOTF：PQ 编码值 → 绝对亮度(nit)
        inline float pq_to_nits(float pq) {
            if (!(pq > 0.0F))  // 同时拦掉 NaN
                return 0.0F;
            if (pq >= 1.0F)
                return 10000.0F;

            constexpr float m1 = 2610.0F / 16384.0F;
            constexpr float m2 = 2523.0F / 4096.0F * 128.0F;
            constexpr float c1 = 3424.0F / 4096.0F;
            constexpr float c2 = 2413.0F / 4096.0F * 32.0F;
            constexpr float c3 = 2392.0F / 4096.0F * 32.0F;

            const float p = std::pow(pq, 1.0F / m2);
            const float numerator = std::max(p - c1, 0.0F);
            const float denominator = c2 - (c3 * p);
            if (denominator <= 0.0F)
                return 0.0F;
            return 10000.0F * std::pow(numerator / denominator, 1.0F / m1);
        }

        // 线性光 → sRGB 编码（8 bit）
        inline std::uint8_t linear_to_srgb8(float linear) {
            if (!(linear > 0.0F))
                return 0;
            if (linear >= 1.0F)
                return 255;
            const float encoded = linear <= 0.0031308F
                                      ? linear * 12.92F
                                      : (1.055F * std::pow(linear, 1.0F / 2.4F)) - 0.055F;
            return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
        }

        // ── staging 纹理 → CPU 像素 ──────────────────────────────────────────
        // RawCopy 就是 Image 加一个源 DXGI 格式；不要重复维护宽高/stride。
        struct RawCopy : Image {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            bool alpha_normalized = false;
        };

        // ── 色调映射（与 tonemap.hlsl 逐行对应） ─────────────────────────────
        // CPU 回退必须和 GPU 出一样的图，所以这里照抄 shader 的数学：
        // PQ 域 spline + IPT 的 I 通道 + hull 色度收缩，矩阵和常量都一一对应。

        // nits → ST 2084 PQ 码值（对应 shader 的 nits_to_pq）。
        inline float nits_to_pq(float nits) {
            constexpr float m1 = 2610.0F / 16384.0F;
            constexpr float m2 = 2523.0F / 4096.0F * 128.0F;
            constexpr float c1 = 3424.0F / 4096.0F;
            constexpr float c2 = 2413.0F / 4096.0F * 32.0F;
            constexpr float c3 = 2392.0F / 4096.0F * 32.0F;
            const float y = std::pow(std::clamp(nits / 10000.0F, 0.0F, 1.0F), m1);
            return std::pow((c1 + (c2 * y)) / (1.0F + (c3 * y)), m2);
        }

        struct Mat3 {
            float m[9];
        };

        // 和 tonemap.hlsl 的 static const float3x3 完全一致（行主序）。
        constexpr Mat3 kRgbToLms709{
            {0.2957413F, 0.6230997F, 0.0811802F,
             0.1561450F, 0.7272835F, 0.1165603F,
             0.0350734F, 0.1565968F, 0.8082766F}};
        constexpr Mat3 kLmsToIpt{
            {0.4000000F, 0.4000000F, 0.2000000F,
             4.4550000F, -4.8510000F, 0.3960000F,
             0.8056000F, 0.3572000F, -1.1628000F}};
        constexpr Mat3 kIptToLms{
            {1.0000000F, 0.0975689F, 0.2052260F,
             1.0000000F, -0.1138760F, 0.1332170F,
             1.0000000F, 0.0326151F, -0.6768870F}};
        constexpr Mat3 kLmsToRgb709{
            {6.1728843F, -5.3203218F, 0.1472545F,
             -1.3234594F, 2.5597126F, -0.2362089F,
             -0.0114500F, -0.2650588F, 1.2765739F}};
        constexpr Mat3 kSrgbToBt2020{
            {0.6274F, 0.3293F, 0.0433F,
             0.0691F, 0.9195F, 0.0114F,
             0.0164F, 0.0880F, 0.8956F}};
        constexpr Mat3 kBt2020To709{
            {1.6605F, -0.5876F, -0.0728F,
             -0.1246F, 1.1329F, -0.0083F,
             -0.0182F, -0.1006F, 1.1187F}};

        inline void mat3_mul(Mat3 const& mat, float const in[3], float out[3]) {
            out[0] = (mat.m[0] * in[0]) + (mat.m[1] * in[1]) + (mat.m[2] * in[2]);
            out[1] = (mat.m[3] * in[0]) + (mat.m[4] * in[1]) + (mat.m[5] * in[2]);
            out[2] = (mat.m[6] * in[0]) + (mat.m[7] * in[1]) + (mat.m[8] * in[2]);
        }

        // spline 曲线参数只依赖 (in_max, sdr_white)：每张图算一次，逐像素只求值。
        struct SplineCurve {
            float src_pivot = 0.0F;
            float dst_pivot = 0.0F;
            float slope = 1.0F;
            float pk = 0.0F;
            float qk = 0.0F;
            float out_min = 0.0F;
            float out_max = 1.0F;
        };

        // libplacebo 的 spline 曲线（PQ 域），参数与 shader 的 tone_map_spline 一致。
        inline SplineCurve make_spline(float in_max, float sdr_white) {
            constexpr float in_min = 0.0F;
            constexpr float out_min = 0.0F;
            SplineCurve curve;
            curve.out_min = out_min;
            curve.out_max = nits_to_pq(std::max(sdr_white, 1.0F));

            curve.src_pivot = std::lerp(in_min, in_max, 0.7F);
            float dst_pivot = std::lerp(out_min, curve.out_max, 0.7F);
            dst_pivot = std::clamp(dst_pivot, std::lerp(out_min, curve.out_max, 0.1F),
                                   std::lerp(out_min, curve.out_max, 0.9F));
            curve.dst_pivot = dst_pivot;

            float slope = (dst_pivot - out_min) / std::max(curve.src_pivot - in_min, 1e-6F);
            const float ratio =
                std::clamp((in_max / std::max(curve.out_max, 1e-6F)) - 1.0F, 0.0F, 1.0F);
            slope = std::pow(std::max(slope, 1e-6F), (1.0F - 0.5F) * ratio);
            curve.slope = slope;

            const float in_span_min = in_min - curve.src_pivot;
            const float in_span_max = in_max - curve.src_pivot;
            const float out_span_min = out_min - dst_pivot;
            const float out_span_max = curve.out_max - dst_pivot;
            curve.pk = ((slope * in_span_min) - out_span_min) / (out_span_min * in_span_min);
            curve.qk = ((slope * in_span_max) - out_span_max) / (out_span_max * in_span_max);
            return curve;
        }

        inline float eval_spline(SplineCurve const& curve, float x) {
            const float d = x - curve.src_pivot;
            const float y =
                ((curve.slope * d) / (1.0F + ((d > 0.0F ? curve.qk : curve.pk) * d))) + curve.dst_pivot;
            return std::clamp(y, curve.out_min, curve.out_max);
        }

        // 线性 RGB（相对 100 nit 白）→ SDR 线性 RGB（显示白 = 1.0），对应 shader 的 tone_map_ipt。
        inline void tone_map_ipt(float const rgb[3], SplineCurve const& curve, float out[3]) {
            float lms[3];
            mat3_mul(kRgbToLms709, rgb, lms);
            float lmspq[3]{nits_to_pq(lms[0] * 100.0F), nits_to_pq(lms[1] * 100.0F),
                           nits_to_pq(lms[2] * 100.0F)};
            float ipt[3];
            mat3_mul(kLmsToIpt, lmspq, ipt);

            const float i_orig = ipt[0];
            ipt[0] = eval_spline(curve, i_orig);

            // hull：提亮时不抬饱和度，压暗时按色域体积收缩色度。
            float hull[2]{i_orig, ipt[0]};
            hull[0] = ((((hull[0] - 6.0F) * hull[0]) + 9.0F) * hull[0]);
            hull[1] = ((((hull[1] - 6.0F) * hull[1]) + 9.0F) * hull[1]);
            const float scale = std::min(i_orig / std::max(ipt[0], 1e-6F), hull[1] / std::max(hull[0], 1e-9F));
            ipt[1] *= scale;
            ipt[2] *= scale;

            float lmspq2[3];
            mat3_mul(kIptToLms, ipt, lmspq2);
            float lms2[3]{pq_to_nits(std::clamp(lmspq2[0], 0.0F, 1.0F)) * 0.01F,
                          pq_to_nits(std::clamp(lmspq2[1], 0.0F, 1.0F)) * 0.01F,
                          pq_to_nits(std::clamp(lmspq2[2], 0.0F, 1.0F)) * 0.01F};
            mat3_mul(kLmsToRgb709, lms2, out);
        }

        // 统一成 32bpp BGRA（Image 定义在 image.h）。

        std::uint32_t bytes_per_pixel(DXGI_FORMAT format) {
            switch (format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            default:
                return 0;
            }
        }

        // GPU→CPU：在编码线程里 Map staging、逐行拷到紧凑缓冲、再 Unmap。
        // HDR 现在直接使用 DEFAULT+SRV，不能 Map；只有 CPU fallback 时才创建一块 staging
        // 并做一次 CopyResource，正常 GPU 路径没有这次拷贝。
        bool map_to_cpu(D3DContext& d3d, ID3D11Texture2D* texture, std::uint64_t gpu_fence,
                        RawCopy& raw, std::string& error) {
            if (gpu_fence != 0 && !d3d.wait_fence(gpu_fence, std::chrono::seconds(5), error))
                return false;
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);

            winrt::com_ptr<ID3D11Texture2D> readback;
            if (desc.Usage != D3D11_USAGE_STAGING) {
                D3D11_TEXTURE2D_DESC read_desc = desc;
                read_desc.Usage = D3D11_USAGE_STAGING;
                read_desc.BindFlags = 0;
                read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                read_desc.MiscFlags = 0;
                readback = create_staging_texture(d3d.device(), read_desc);
                if (!readback) {
                    error = "创建 HDR 回读 staging 失败";
                    return false;
                }
                // 拷贝完成后立刻记一个 fence：《锁外》等它，绝不在锁内等 GPU。
                // 否则 4K 拷贝期间一直握着 context_mutex，capture 的 CopyResource 和其他
                // 编码线程都会被堵住，而且没有超时兜底。
                std::uint64_t copy_fence = 0;
                {
                    std::scoped_lock lock(d3d.context_mutex());
                    d3d.context()->CopyResource(readback.get(), texture);
                    copy_fence = d3d.signal_fence_locked();
                }
                texture = readback.get();
                texture->GetDesc(&desc);
                if (copy_fence != 0 && !d3d.wait_fence(copy_fence, std::chrono::seconds(5), error))
                    return false;
            }
            const std::uint32_t bpp = bytes_per_pixel(desc.Format);
            if (desc.Width == 0 || desc.Height == 0 || bpp == 0) {
                error = fmt::format("不支持的 staging 纹理: {}x{} fmt={}",
                                    desc.Width, desc.Height, static_cast<int>(desc.Format));
                return false;
            }

            raw.width = desc.Width;
            raw.height = desc.Height;
            raw.stride = desc.Width * bpp;
            raw.format = desc.Format;
            raw.alpha_normalized = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                   desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            raw.pixels.resize(static_cast<std::size_t>(raw.stride) * raw.height);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            {
                // 阻塞 Map：拷贝/渲染的完成已经由上面的 wait_fence 在锁外等到，
                // 所以这里正常会立即返回，持锁时间只有一次 Map 调用。
                // 之前用 DO_NOT_WAIT 轮询，在 NVIDIA 上即使 fence 已完成也会持续返回
                // DXGI_ERROR_WAS_STILL_DRAWING，只能靠 sleep 轮询，白白浪费 10~20 ms。
                std::scoped_lock lock(d3d.context_mutex());
                const HRESULT hr = d3d.context()->Map(texture, 0, D3D11_MAP_READ, 0, &mapped);
                if (FAILED(hr)) {
                    error = fmt::format("Map staging 纹理失败: {}", describe_hresult(hr));
                    return false;
                }
            }
            for (std::size_t y = 0; y < raw.height; ++y) {
                auto const* src = static_cast<const std::uint8_t*>(mapped.pData) +
                                  y * mapped.RowPitch;
                auto* dst = raw.pixels.data() + y * raw.stride;
                if (raw.alpha_normalized)
                    bgra_copy_set_alpha_row(src, dst, raw.width);
                else
                    std::memcpy(dst, src, raw.stride);
            }
            {
                std::scoped_lock lock(d3d.context_mutex());
                d3d.context()->Unmap(texture, 0);
            }
            return true;
        }

        // FP16 像素（scRGB 或 PQ）→ SDR BGRA8，走 spline + IPT 链路（与 tonemap.hlsl 逐行对应）。
        // 单独抽出来，供 convert_pixels 和外部工具（--bench 目录模式）共用。
        bool tone_map_fp16_to_sdr(std::uint16_t const* fp16, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t stride_bytes, DXGI_COLOR_SPACE_TYPE color_space,
                                  std::uint32_t sdr_white_nits, Image& out) {
            out.width = width;
            out.height = height;
            out.stride = width * 4;
            out.pixels.resize(static_cast<std::size_t>(out.stride) * height);

            // FP16：HDR10(PQ) 或 scRGB 的色调映射到 SDR（独立函数版本，供外部工具复用）。
            // 走的是和 shader 完全相同的 spline + IPT 链路（见上面的 tone_map_ipt），
            // 所以 GPU 失败回退到 CPU 时 SDR 结果不会变。
            // 代价是每像素十几次 pow，比原来的逐通道 LUT 慢；只在 GPU 不可用时才会走到。
            const bool is_pq = color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                               color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
            const float sdr_white = static_cast<float>(std::max<std::uint32_t>(sdr_white_nits, 1));

            std::vector<float> row_floats(static_cast<std::size_t>(width) * 4);

            // Pass 1：峰值。GPU 用 peak pass 取“最大通道 → 10-bit PQ 码值”，这里做同样的事，
            // 否则 CPU 和 GPU 的 in_max 不同，曲线就不同。
            const auto load_row = [&](std::uint32_t y) {
                const auto* src = reinterpret_cast<const std::uint16_t*>(
                    reinterpret_cast<const std::uint8_t*>(fp16) + (static_cast<std::size_t>(y) * stride_bytes));
                half_to_float_row(src, row_floats.data(), static_cast<std::size_t>(width) * 4);
            };
            const auto pixel_nits_2020 = [is_pq](float const rgb[3], float out[3]) {
                if (is_pq) {
                    out[0] = pq_to_nits(rgb[0]);
                    out[1] = pq_to_nits(rgb[1]);
                    out[2] = pq_to_nits(rgb[2]);
                } else {
                    float scaled[3]{rgb[0] * 80.0F, rgb[1] * 80.0F, rgb[2] * 80.0F};
                    mat3_mul(kSrgbToBt2020, scaled, out);
                }
            };

            float peak_pq = 0.0F;
            for (std::uint32_t y = 0; y < height; ++y) {
                load_row(y);
                for (std::uint32_t x = 0; x < width; ++x) {
                    const float rgb[3]{row_floats[(static_cast<std::size_t>(x) * 4) + 0],
                                       row_floats[(static_cast<std::size_t>(x) * 4) + 1],
                                       row_floats[(static_cast<std::size_t>(x) * 4) + 2]};
                    float nits2020[3];
                    pixel_nits_2020(rgb, nits2020);
                    const float m = std::max({nits2020[0], nits2020[1], nits2020[2], 0.0F});
                    peak_pq = std::max(peak_pq, nits_to_pq(m));
                }
            }
            // 和 shader 一样把峰值量化到 10-bit 码值，再和 SDR 白点取大。
            const float peak_code = std::round(std::clamp(peak_pq, 0.0F, 1.0F) * 1023.0F) / 1023.0F;
            const float in_max = std::max(nits_to_pq(sdr_white), peak_code);
            const float white_scale = 100.0F / sdr_white;
            const SplineCurve curve = make_spline(in_max, sdr_white);

            // Pass 2：逐像素 tone map → sRGB → BGRA。
            for (std::uint32_t y = 0; y < height; ++y) {
                load_row(y);
                std::uint8_t* dst = out.pixels.data() + (static_cast<std::size_t>(y) * out.stride);
                for (std::uint32_t x = 0; x < width; ++x) {
                    const float rgb[3]{row_floats[(static_cast<std::size_t>(x) * 4) + 0],
                                       row_floats[(static_cast<std::size_t>(x) * 4) + 1],
                                       row_floats[(static_cast<std::size_t>(x) * 4) + 2]};
                    float nits2020[3];
                    pixel_nits_2020(rgb, nits2020);
                    // shader 里是 nits2020 → BT.709 → *0.01 再进 IPT，这里照做。
                    float nits709[3];
                    mat3_mul(kBt2020To709, nits2020, nits709);
                    const float relative[3]{nits709[0] * 0.01F, nits709[1] * 0.01F, nits709[2] * 0.01F};
                    float linear[3];
                    tone_map_ipt(relative, curve, linear);
                    std::uint8_t* q = dst + (static_cast<std::size_t>(x) * 4);
                    q[0] = linear_to_srgb8(linear[2] * white_scale);  // B
                    q[1] = linear_to_srgb8(linear[1] * white_scale);  // G
                    q[2] = linear_to_srgb8(linear[0] * white_scale);  // R
                    q[3] = 255;
                }
            }
            return true;
        }

        // 把原始像素转换成 BMP 需要的 32bpp BGRA（HDR 的 FP16 顺便做色调映射）。
        bool convert_pixels(RawCopy& raw, DXGI_COLOR_SPACE_TYPE color_space, std::uint32_t sdr_white_nits, Image& out) {
            const bool is_bgra = raw.format == DXGI_FORMAT_B8G8R8A8_UNORM || raw.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            const bool is_rgba = raw.format == DXGI_FORMAT_R8G8B8A8_UNORM || raw.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool is_half = raw.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (!is_bgra && !is_rgba && !is_half)
                return false;

            out.width = raw.width;
            out.height = raw.height;
            out.stride = raw.width * 4;

            // SDR + BGRA：直接搬缓冲，但统一 alpha 为 255（截图不需要窗口 alpha）。
            if (is_bgra) {
                out.pixels.swap(raw.pixels);
                if (!raw.alpha_normalized) {
                    for (std::uint32_t y = 0; y < out.height; ++y) {
                        auto* row = out.pixels.data() + static_cast<std::size_t>(y) * out.stride;
                        bgra_set_alpha_row(row, out.width);
                    }
                }
                return true;
            }

            out.pixels.resize(static_cast<std::size_t>(out.stride) * raw.height);

            if (is_rgba) {
                for (std::uint32_t y = 0; y < raw.height; ++y) {
                    const std::uint8_t* src = raw.pixels.data() + (static_cast<std::size_t>(y) * raw.stride);
                    std::uint8_t* dst = out.pixels.data() + (static_cast<std::size_t>(y) * out.stride);
                    for (std::uint32_t x = 0; x < raw.width; ++x) {
                        const std::uint8_t* p = src + (static_cast<std::size_t>(x) * 4);
                        std::uint8_t* q = dst + (static_cast<std::size_t>(x) * 4);
                        q[0] = p[2];
                        q[1] = p[1];
                        q[2] = p[0];
                        q[3] = 255;  // 截图不需要窗口 alpha
                    }
                }
                return true;
            }

            // FP16：HDR10(PQ) 或 scRGB 的色调映射到 SDR（和 shader 同一条 spline + IPT 链路）。
            return tone_map_fp16_to_sdr(reinterpret_cast<const std::uint16_t*>(raw.pixels.data()), raw.width,
                                        raw.height, raw.stride, color_space, sdr_white_nits, out);
        }

    }  // namespace

    // ── staging 纹理 ─────────────────────────────────────────────────────────

    // 公开包装：匿名命名空间里的实现只给编码线程用，基准工具也需要同一条链路。
    bool tone_map_hdr_to_sdr(std::uint16_t const* fp16, std::uint32_t width, std::uint32_t height,
                             std::uint32_t stride_bytes, DXGI_COLOR_SPACE_TYPE color_space,
                             std::uint32_t sdr_white_nits, Image& out) {
        if (fp16 == nullptr || width == 0 || height == 0)
            return false;
        return tone_map_fp16_to_sdr(fp16, width, height, stride_bytes, color_space, sdr_white_nits, out);
    }

    // 纹理都是“谁持有谁释放”，用 com_ptr 表达独占（不可拷贝，只能移动）。
    winrt::com_ptr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc) {
        if (device == nullptr)
            return {};
        winrt::com_ptr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
            return {};
        // job 析构时立刻 Release，临时纹理不会留下。
        return texture;
    }

    // ── EncodeWorkers ────────────────────────────────────────────────────────

    EncodeWorkers::EncodeWorkers(std::filesystem::path output_dir, std::shared_ptr<EncodeQueue> jobs,
                                 CodecOptions options, ToneMapBackend tone_map_backend,
                                 std::wstring filename_pattern, std::shared_ptr<UiBridge> ui)
        // clang-format off
        : output_dir_(std::move(output_dir))
        , jobs_(std::move(jobs))
        , ui_(std::move(ui))
        , options_(options)
        , tone_map_backend_(tone_map_backend)
        , filename_pattern_(std::move(filename_pattern)) {
        // clang-format on
        set_codec_options(options_);
    }

    EncodeWorkers::~EncodeWorkers() {
        stop();
    }

    void EncodeWorkers::start(std::size_t thread_count) {
        thread_count = std::clamp<std::size_t>(thread_count, 1, 8);
        threads_.reserve(thread_count);
        worker_count_ = thread_count;
        stopped_.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back([this, i] {
                // 线程入口不能抛：逃逸的异常会直接 std::terminate。
                try {
                    worker_main(i);
                } catch (std::exception const& e) {
                    SPDLOG_CRITICAL("编码线程 {} 异常退出: {}", i, e.what());
                } catch (...) {
                    SPDLOG_CRITICAL("编码线程 {} 异常退出", i);
                }
            });
        }
    }

    void EncodeWorkers::stop() {
        if (!jobs_)
            return;
        // 每个线程一个哨兵。注意 MPMC 队列只在同一生产者内保序，主线程投递的哨兵
        // 可能越过 capture 线程此前投递的任务；worker_main() 里由最后一个拿到哨兵
        // 的线程清空残留任务，保证退出时不丢图。
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            EncodeJob sentinel;
            sentinel.stop = true;
            jobs_->enqueue(std::move(sentinel));
        }
        for (auto& thread : threads_) {
            if (thread.joinable())
                thread.join();
        }
        threads_.clear();
    }

    void EncodeWorkers::worker_main(std::size_t index) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        const std::wstring thread_name = utf8_to_wstring(fmt::format("GameSnap.Encode.{}", index));
        ::SetThreadDescription(::GetCurrentThread(), thread_name.c_str());

        // 每个编码线程自己的 consumer token，队列按线程分开缓存，争用更小。
        moodycamel::ConsumerToken token(*jobs_);
        for (;;) {
            EncodeJob job;
            jobs_->wait_dequeue(token, job);
            if (job.stop) {
                // moodycamel 的 MPMC 队列只在同一生产者内保序：主线程投递的 stop
                // 哨兵可能越过 capture 线程此前投递的任务。最后一个消费到哨兵的
                // 线程负责清空残留任务，避免退出时丢图。
                if (stopped_.fetch_add(1, std::memory_order_acq_rel) + 1 == worker_count_) {
                    EncodeJob leftover;
                    while (jobs_->try_dequeue(leftover)) {
                        if (!leftover.stop)
                            encode_one(index, leftover);
                    }
                }
                break;
            }
            encode_one(index, job);
        }

        SPDLOG_INFO("编码线程 {} 退出", index);
    }

    void EncodeWorkers::encode_one(std::size_t index, EncodeJob& job) {
        // 不声明 noexcept：catch 里写日志本身仍可能失败（OOM 级别的极端情况），
        // 那时让异常交给线程入口的兜底 catch，而不是直接 std::terminate。
        try {
            encode(job);
        } catch (winrt::hresult_error const& e) {
            SPDLOG_ERROR("编码线程 {}: [#{}] 失败: {}", index, job.sequence, describe_hresult(e.code()));
        } catch (std::exception const& e) {
            SPDLOG_ERROR("编码线程 {}: [#{}] 失败: {}", index, job.sequence, e.what());
        } catch (...) {
            SPDLOG_ERROR("编码线程 {}: [#{}] 失败（未知异常）", index, job.sequence);
        }
    }

    void EncodeWorkers::encode(EncodeJob& job) {
        auto ui_job_guard = ScopeExit{[&] {
            if (ui_)
                ui_->job_finished();
        }};

        if (job.canceled && job.canceled->load(std::memory_order_acquire))
            return;
        if (!job.d3d || !job.texture) {
            SPDLOG_WARN("[#{:04}] 非法的编码任务", job.sequence);
            return;
        }

        // 所有大缓冲都按 job 动态分配，闲置时不占内存/显存。
        RawCopy raw;
        Image sdr;
        U16Buffer hdr10;
        sdr.metadata.title = wstring_to_utf8(job.title);
        sdr.metadata.date_time = exif_date_time(job.local_time);
        std::string error;
        D3D11_TEXTURE2D_DESC texture_desc{};
        job.texture->GetDesc(&texture_desc);

        // 一进编码就先把基本名推给面板（不含 _HDR/_SDR 与扩展名）：
        // 文件名和时间戳在开拍时就定了，没必要等编码完。
        const std::wstring base_name = make_base_name(job.title, job.process_exe, job.process_name, job.local_time,
                                                      job.sequence, filename_pattern_);
        if (ui_)
            ui_->publish(job.sequence, base_name, nullptr);

        // HDR 优先走 GPU：mapper 直接采样 DEFAULT+SRV 纹理，fence/event 等待 GPU 完成。
        // 只有它失败时才把 DEFAULT 纹理回读到 staging，再走 CPU 回退。
        //
        // ToneMapper 每个 HDR job 现建现用（4 张全尺寸纹理，4K 下约 127 MiB），map 完随 job 析构。
        // 这是刻意选择：复用会把这几张纹理变成闲置常驻显存，而截图工具绝大多数时间是闲置的。
        bool gpu_ok = false;
        if (job.hdr && tone_map_backend_ != ToneMapBackend::Cpu) {
            auto mapper = std::make_unique<ToneMapper>(job.d3d);
            if (!mapper->ok()) {
                SPDLOG_WARN("tone mapper 初始化失败: {}", mapper->init_error());
            } else {
                gpu_ok = mapper->map(job.texture.get(), job.color_space,
                                     job.sdr_white_nits, job.tone_map, sdr, hdr10, error);
                if (!gpu_ok)
                    SPDLOG_WARN("[#{:04}] compute tone map 失败，回退 CPU: {}", job.sequence, error);
            }
        }

        if (!gpu_ok) {
            if (!map_to_cpu(*job.d3d, job.texture.get(), job.gpu_fence, raw, error)) {
                SPDLOG_ERROR("[#{:04}] 取像素失败: {}", job.sequence, error);
                return;
            }
        }
        // 像素已经在 CPU 手里（或 GPU 路径已经采样完），这里就把全尺寸捕获纹理放掉：
        // 队列积压时每张 4K FP16 纹理占 33 MiB，早放一刻就能少顶住一次显存峰值。
        job.texture = nullptr;
        const auto mapped_at = SteadyClock::now();

        if (job.canceled && job.canceled->load(std::memory_order_acquire))
            return;

        const bool is_pq = job.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                           job.color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
        std::vector<std::filesystem::path> written;
        int width = static_cast<int>(texture_desc.Width);
        int height = static_cast<int>(texture_desc.Height);

        if (job.hdr) {
            ImageFormat hdr_format = format_supports_hdr(job.hdr_format) ? job.hdr_format : ImageFormat::Avif;

            // 先把 SDR 像素转出来并立刻发布缩略图：面板马上显示这一张，
            // 不用等 HDR 那 1 秒多的编码（文件名等写盘成功后再补）。
            if (!gpu_ok && !convert_pixels(raw, job.color_space, job.sdr_white_nits, sdr)) {
                SPDLOG_ERROR("[#{:04}] 不支持的像素格式 {}", job.sequence, static_cast<int>(raw.format));
                return;
            }
            if (ui_)
                ui_->publish(job.sequence, base_name, &sdr);

            const auto hdr_path = make_output_path(output_dir_, base_name,
                                                   image_extension(hdr_format), L"HDR");
            bool hdr_ok = false;
            if (gpu_ok) {
                hdr_ok = write_hdr10(hdr_path, hdr_format, hdr10.data(), texture_desc.Width,
                                     texture_desc.Height, sdr.metadata, error);
            } else {
                const HdrFrame frame{.bytes = raw.pixels.data(),
                                     .width = raw.width,
                                     .height = raw.height,
                                     .stride = raw.stride,
                                     .bytes_per_pixel = raw.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U,
                                     .pq = is_pq,
                                     .sdr_white_nits = job.sdr_white_nits,
                                     .metadata = sdr.metadata};
                hdr_ok = write_hdr(hdr_path, hdr_format, frame, error, &hdr10);
            }
            if (!hdr_ok) {
                SPDLOG_ERROR("[#{:04}] HDR 编码失败 ({}): {}", job.sequence, to_string(hdr_format), error);
            } else {
                written.push_back(hdr_path);
            }

            if (job.sdr_format == ImageFormat::Heic)
                SPDLOG_WARN("[#{:04}] libheif 不支持 gain map，HEIC SDR 不包含 HDR 重建信息", job.sequence);
            const auto sdr_path = make_output_path(output_dir_, base_name,
                                                   image_extension(job.sdr_format), L"SDR");
            const bool sdr_ok =
                (!hdr10.empty() && job.sdr_format == ImageFormat::Avif)
                    ? encode_avif_gainmap(sdr_path, sdr, hdr10.data(), sdr.width, sdr.height, error)
                    : write_sdr(sdr_path, job.sdr_format, sdr, error);
            if (!sdr_ok) {
                SPDLOG_ERROR("[#{:04}] SDR 编码失败 ({}): {}", job.sequence, to_string(job.sdr_format), error);
            } else {
                written.push_back(sdr_path);
            }
        } else {
            if (!convert_pixels(raw, job.color_space, job.sdr_white_nits, sdr)) {
                SPDLOG_ERROR("[#{:04}] 不支持的像素格式 {}", job.sequence, static_cast<int>(raw.format));
                return;
            }
            // 同上：写盘之前就先把缩略图推给面板。
            if (ui_)
                ui_->publish(job.sequence, base_name, &sdr);

            const auto path = make_output_path(output_dir_, base_name,
                                               image_extension(job.sdr_format), {});
            if (!write_sdr(path, job.sdr_format, sdr, error)) {
                SPDLOG_ERROR("[#{:04}] 编码失败 ({}): {}", job.sequence, to_string(job.sdr_format), error);
                return;
            }
            written.push_back(path);
            width = static_cast<int>(sdr.width);
            height = static_cast<int>(sdr.height);
        }

        const auto encoded_at = SteadyClock::now();
        if (written.empty())
            return;

        // 文件已落盘：补上真实文件名（缩略图早就在上面发布过了，这里不再重算）。
        if (ui_)
            ui_->publish(job.sequence, written.front().filename().wstring(), nullptr);

        std::string paths;
        for (auto const& path : written) {
            if (!paths.empty())
                paths += ", ";
            paths += wstring_to_utf8(path.filename().wstring());
        }

        const auto& t = job.timings;
        const double key_to_start = duration_ms(t.start_capture - t.input);
        const double queue_wait = duration_ms(t.dequeued - t.input);
        const double title_lookup = duration_ms(t.title_done - t.dequeued);
        const double prepare = duration_ms(t.prepared - t.title_done);
        const double start_to_frame = duration_ms(t.frame_received - t.start_capture);
        const double copy = duration_ms(t.copied - t.frame_received);
        const double pixel = duration_ms(mapped_at - t.copied);
        const double convert = duration_ms(encoded_at - mapped_at);
        const double total = duration_ms(encoded_at - t.input);

        SPDLOG_INFO(
            "[#{:04}] {} {}x{} {} | 按键→StartCapture {:.2f}ms "
            "(排队 {:.2f} | 标题 {:.2f} | 准备 {:.2f}) | "
            "StartCapture→首帧 {:.2f}ms | "
            "GPU 拷贝 {:.2f}ms | 取像素 {:.2f}ms | 编码/写盘 {:.2f}ms | "
            "按键→落盘 {:.2f}ms",
            job.sequence, paths, width, height, job.hdr ? "HDR+SDR" : "SDR",
            key_to_start, queue_wait, title_lookup, prepare,
            start_to_frame, copy, pixel, convert, total);
    }

}  // namespace gamesnap
