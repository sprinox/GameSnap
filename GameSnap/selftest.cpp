#include "pch.h"

#include "selftest.h"

// Release 不编自检（见 selftest.h 的 GAMESNAP_SELFTEST）。
#if GAMESNAP_SELFTEST

#    include <algorithm>
#    include <cmath>
#    include <cstdint>
#    include <cstdio>
#    include <cstring>
#    include <filesystem>
#    include <vector>

#    include <spng.h>

#    include "d3d.h"
#    include "display.h"
#    include "encoder.h"
#    include "messages.h"

namespace gamesnap {
    namespace {

        constexpr std::uint32_t kWidth = 640;
        constexpr std::uint32_t kHeight = 360;
        constexpr float kMaxPq = 0.58F;

        // 左上红、右上绿、左下蓝、右下灰阶渐变：
        // 任何通道顺序错误或上下翻转都能一眼看出来。
        void fill_bgra(std::uint8_t* data, std::uint32_t row_pitch, std::uint32_t width, std::uint32_t height) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint8_t r = 0;
                    std::uint8_t g = 0;
                    std::uint8_t b = 0;
                    if (y < height / 2 && x < width / 2) {
                        r = 255;
                    } else if (y < height / 2) {
                        g = 255;
                    } else if (x < width / 2) {
                        b = 255;
                    } else {
                        const auto v = static_cast<std::uint8_t>(x * 255 / (width - 1));
                        r = g = b = v;
                    }
                    std::uint8_t* pixel = data + (static_cast<std::size_t>(y) * row_pitch) + (static_cast<std::size_t>(x) * 4);
                    pixel[0] = b;
                    pixel[1] = g;
                    pixel[2] = r;
                    pixel[3] = 255;
                }
            }
        }

        void fill_rgba(std::uint8_t* data, std::uint32_t row_pitch, std::uint32_t width,
                       std::uint32_t height) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint8_t r = 0;
                    std::uint8_t g = 0;
                    std::uint8_t b = 0;
                    if (y < height / 2 && x < width / 2) {
                        r = 255;
                    } else if (y < height / 2) {
                        g = 255;
                    } else if (x < width / 2) {
                        b = 255;
                    } else {
                        const auto v = static_cast<std::uint8_t>(x * 255 / (width - 1));
                        r = g = b = v;
                    }
                    std::uint8_t* pixel = data + (static_cast<std::size_t>(y) * row_pitch) +
                                          (static_cast<std::size_t>(x) * 4);
                    pixel[0] = r;
                    pixel[1] = g;
                    pixel[2] = b;
                    pixel[3] = 255;
                }
            }
        }

        std::uint16_t float_to_half(float value) {
            if (!(value > 0.0F))
                return 0;
            if (value >= 1.0F)
                return 0x3C00;  // 1.0
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            const int exponent = static_cast<int>((bits >> 23) & 0xFFU) - 127;
            const std::uint32_t mantissa = bits & 0x7FFFFFU;
            if (exponent >= -14)
                return static_cast<std::uint16_t>(((exponent + 15) << 10) | (mantissa >> 13));
            const double m = std::ldexp(1.0 + (static_cast<double>(mantissa) / 8388608.0), exponent + 24);
            if (m < 1.0)
                return 0;
            return static_cast<std::uint16_t>(std::lround(m));
        }

        // PQ 编码值 0..0.58 约等于 0..203 nit；HDR 用例的白点故意设成 100 nit，
        // 让渐变的高端超过白点，这样"肩部有没有生效"才会体现在输出上
        // （白点设成 203 时曲线本来就该是恒等，测不出峰值归约失效这类问题）。
        void fill_pq(std::uint8_t* data, std::uint32_t row_pitch, std::uint32_t width, std::uint32_t height) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    float pq = static_cast<float>(x) / static_cast<float>(width - 1) * kMaxPq;
                    if (y < height / 3)
                        pq = kMaxPq;  // 顶部一条纯白，确认高光裁切正常
                    auto* pixel = reinterpret_cast<std::uint16_t*>(
                        data + (static_cast<std::size_t>(y) * row_pitch) + (static_cast<std::size_t>(x) * 8));
                    pixel[0] = float_to_half(pq);
                    pixel[1] = float_to_half(pq);
                    pixel[2] = float_to_half(pq);
                    pixel[3] = float_to_half(1.0F);
                }
            }
        }

        struct Case {
            DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            std::uint32_t sdr_white_nits = 80;
            char const* label = "";
            std::uint32_t width = kWidth;
            std::uint32_t height = kHeight;
            // 覆盖 SDR 伴生格式；空 = HDR 用例固定 PNG（给像素级校验用），SDR 用例跟随配置。
            std::optional<ImageFormat> sdr_format;
        };

        winrt::com_ptr<ID3D11Texture2D> create_gpu_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc) {
            winrt::com_ptr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
                return {};
            return texture;
        }

        // 解码一张 PNG，统计三通道同时为 255 的像素占比。
        // 自检图顶部 1/3 是一条故意写满的纯白条，会干扰统计，所以只统计它下面的渐变区——
        // 那段如果被大面积裁成纯白，就说明色调映射退化了（例如峰值归约失效 → 曲线恒等）。
        bool saturated_fraction(std::filesystem::path const& path, double& fraction) {
            std::FILE* file = nullptr;
            if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
                return false;
            std::vector<std::uint8_t> bytes;
            std::uint8_t chunk[64 * 1024];
            std::size_t read = 0;
            while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
                bytes.insert(bytes.end(), chunk, chunk + read);
            std::fclose(file);
            if (bytes.empty())
                return false;

            spng_ctx* ctx = spng_ctx_new(0);
            if (ctx == nullptr)
                return false;
            bool ok = false;
            if (spng_set_png_buffer(ctx, bytes.data(), bytes.size()) == 0) {
                spng_ihdr ihdr{};
                std::size_t decoded_size = 0;
                if (spng_get_ihdr(ctx, &ihdr) == 0 &&
                    spng_decoded_image_size(ctx, SPNG_FMT_RGB8, &decoded_size) == 0) {
                    std::vector<std::uint8_t> rgb(decoded_size);
                    if (spng_decode_image(ctx, rgb.data(), rgb.size(), SPNG_FMT_RGB8, 0) == 0) {
                        // 跳过顶部 1/3 的纯白条，只统计渐变区。
                        const std::size_t row_bytes = static_cast<std::size_t>(ihdr.width) * 3;
                        const std::size_t start_row = static_cast<std::size_t>(ihdr.height) / 3;
                        std::size_t saturated = 0;
                        std::size_t counted = 0;
                        for (std::size_t y = start_row; y < ihdr.height; ++y) {
                            const std::size_t begin = y * row_bytes;
                            const std::size_t end = begin + row_bytes;
                            if (end > rgb.size())
                                break;
                            for (std::size_t i = begin; i + 2 < end; i += 3) {
                                ++counted;
                                if (rgb[i] == 255 && rgb[i + 1] == 255 && rgb[i + 2] == 255)
                                    ++saturated;
                            }
                        }
                        if (counted > 0) {
                            fraction = static_cast<double>(saturated) / static_cast<double>(counted);
                            ok = true;
                        }
                    }
                }
            }
            spng_ctx_free(ctx);
            return ok;
        }

        bool run_case(Config const& config, std::shared_ptr<D3DContext> const& d3d, Case const& test_case) {
            SPDLOG_INFO("--- 自检用例: {} ---", test_case.label);

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = test_case.width;
            desc.Height = test_case.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = test_case.format;
            desc.SampleDesc.Count = 1;
            const bool hdr_target = test_case.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.Usage = hdr_target ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
            desc.BindFlags = hdr_target ? D3D11_BIND_SHADER_RESOURCE : 0;
            desc.CPUAccessFlags = hdr_target ? 0 : D3D11_CPU_ACCESS_READ;

            auto target = hdr_target ? create_gpu_texture(d3d->device(), desc)
                                     : create_staging_texture(d3d->device(), desc);
            if (!target) {
                SPDLOG_ERROR("自检失败: 创建目标纹理失败");
                return false;
            }

            auto source_desc = desc;
            source_desc.Usage = D3D11_USAGE_STAGING;
            source_desc.BindFlags = 0;
            source_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            winrt::com_ptr<ID3D11Texture2D> source;
            if (FAILED(d3d->device()->CreateTexture2D(&source_desc, nullptr, source.put()))) {
                SPDLOG_ERROR("自检失败: 创建源纹理失败");
                return false;
            }

            {
                std::scoped_lock lock(d3d->context_mutex());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(d3d->context()->Map(source.get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
                    SPDLOG_ERROR("自检失败: Map 源纹理失败");
                    return false;
                }
                auto* bytes = static_cast<std::uint8_t*>(mapped.pData);
                if (test_case.format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                    fill_pq(bytes, mapped.RowPitch, test_case.width, test_case.height);
                else if (test_case.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         test_case.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                    fill_rgba(bytes, mapped.RowPitch, test_case.width, test_case.height);
                else
                    fill_bgra(bytes, mapped.RowPitch, test_case.width, test_case.height);
                d3d->context()->Unmap(source.get(), 0);
                d3d->context()->CopyResource(target.get(), source.get());
            }

            auto jobs = std::make_shared<EncodeQueue>();
            EncodeWorkers workers(config.output_dir, jobs, config.codec, config.tone_map_backend,
                                  config.filename_pattern, {});
            workers.start(1);

            std::error_code ec;
            std::vector<std::filesystem::path> before;
            for (const auto& entry : std::filesystem::directory_iterator(config.output_dir, ec))
                before.push_back(entry.path());

            EncodeJob job;
            job.texture = std::move(target);
            job.d3d = d3d;
            job.color_space = test_case.color_space;
            job.sdr_white_nits = test_case.sdr_white_nits;
            job.hdr = test_case.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            job.hdr_format = config.hdr_format;
            // HDR 用例的 SDR 伴生文件默认写 PNG：这样像素级校验（需要无损解码）不依赖用户配置的
            // format_sdr，默认流程也会跑到。需要覆盖特定路径时可显式指定 sdr_format。
            job.sdr_format = test_case.sdr_format.value_or(job.hdr ? ImageFormat::Png : config.sdr_format);
            job.tone_map = config.tone_map;
            job.sequence = 7777;
            job.title = L"GameSnap SelfTest";
            job.process_exe = L"GameSnap";
            job.process_name = L"GameSnap SelfTest";
            job.local_time = Clock::now();
            job.timings.input = SteadyClock::now();
            job.timings.dequeued = job.timings.input;
            job.timings.title_done = job.timings.input;
            job.timings.prepared = job.timings.input;
            job.timings.start_capture = job.timings.input;
            job.timings.frame_received = job.timings.input;
            job.timings.copied = job.timings.input;

            const bool hdr_job = job.hdr;
            const ImageFormat sdr_format = job.sdr_format;  // move 之后 job 的成员值不再可信
            if (!jobs->enqueue(std::move(job))) {
                SPDLOG_ERROR("自检失败: 编码队列入队失败");
                return false;
            }
            // stop() 会给编码线程投递停止哨兵，并等它把任务落盘。
            workers.stop();

            std::vector<std::filesystem::path> produced;
            for (const auto& entry : std::filesystem::directory_iterator(config.output_dir, ec)) {
                if (!entry.is_regular_file())
                    continue;
                if (std::ranges::find(before, entry.path()) == before.end())
                    produced.push_back(entry.path());
            }
            if (produced.empty()) {
                SPDLOG_ERROR("自检失败: 没有生成输出文件（检查输出目录与日志中的编码错误）");
                return false;
            }

            bool has_hdr = false;
            bool has_sdr = false;
            for (const auto& path : produced) {
                const std::wstring name = path.filename().wstring();
                if (name.find(L"_HDR.") != std::wstring::npos)
                    has_hdr = true;
                if (name.find(L"_SDR.") != std::wstring::npos)
                    has_sdr = true;
                const auto size = std::filesystem::file_size(path, ec);
                if (ec || size == 0) {
                    SPDLOG_ERROR("自检失败: 输出文件为空: {}", wstring_to_utf8(path.wstring()));
                    return false;
                }
                SPDLOG_INFO("自检输出: {} ({} 字节)", wstring_to_utf8(path.wstring()), size);
            }
            if (hdr_job && (!has_hdr || !has_sdr || produced.size() != 2)) {
                SPDLOG_ERROR("自检失败: HDR 用例应且只应生成 _HDR 与 _SDR 两个文件（实际 {} 个）",
                             produced.size());
                return false;
            }
            if (!hdr_job && produced.size() != 1) {
                SPDLOG_ERROR("自检失败: SDR 用例应只生成一个文件（实际 {} 个）", produced.size());
                return false;
            }

            // 像素级校验：光看"文件存在且非空"抓不到色调映射绑错槽位这类问题
            // （曲线退化成硬裁剪时文件照样正常）。HDR 用例的 SDR 伴生文件固定是 PNG（见上），
            // 所以这里的条件与格式无关，默认配置也会执行。
            if (hdr_job && sdr_format == ImageFormat::Png) {
                for (const auto& path : produced) {
                    if (path.filename().wstring().find(L"_SDR.") == std::wstring::npos)
                        continue;
                    double fraction = 0.0;
                    if (saturated_fraction(path, fraction)) {
                        SPDLOG_INFO("SDR 渐变区饱和像素 {:.2f}%（阈值 5%）", fraction * 100.0);
                        if (fraction > 0.05) {
                            SPDLOG_ERROR("自检失败: SDR 渐变区有 {:.0f}% 像素被裁成纯白，色调映射可能退化了",
                                         fraction * 100.0);
                            return false;
                        }
                    }
                }
            }

            SPDLOG_INFO("自检用例通过: {} 个文件", produced.size());
            return true;
        }  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): custom shared_ptr deleter is opaque

    }  // namespace

    bool run_selftest(Config const& config) {
        SPDLOG_INFO("=== GameSnap 自检：不经过屏幕捕获，验证 staging → 像素转换 → 编码 → 写盘 ===");

        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (winrt::hresult_error const& e) {
            SPDLOG_WARN("自检: 初始化 COM 失败: {}", describe_hresult(e.code()));
        }

        auto display = select_monitor(config.monitor);
        if (display.monitor == nullptr) {
            SPDLOG_ERROR("自检失败: 没有可用显示器");
            return false;
        }

        std::string error;
        auto d3d = D3DContext::create(display.monitor, display.dxgi.adapter.get(), &error);
        if (!d3d) {
            SPDLOG_ERROR("自检失败: 无法创建 D3D 设备: {}", error);
            return false;
        }

        bool ok = true;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_B8G8R8A8_UNORM, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, .sdr_white_nits = 80, .label = "SDR BGRA"}) &&
             ok;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R8G8B8A8_UNORM, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, .sdr_white_nits = 80, .label = "SDR RGBA"}) &&
             ok;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, .sdr_white_nits = 100, .label = "HDR10(PQ) FP16"}) &&
             ok;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, .sdr_white_nits = 100, .label = "HDR10(PQ) FP16 641x361", .width = 641, .height = 361}) &&
             ok;
        // WGC 的 FP16 帧实际是 scRGB（线性，1.0 = 80 nits），这才是真实采集帧的编码。
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, .sdr_white_nits = 80, .label = "HDR scRGB FP16"}) &&
             ok;
        // AVIF + AVIF：唯一会走 encode_avif_gainmap() 的组合（HDR 与 SDR 都写 AVIF）。
        // 这条路径内存最重（4K 下增益图计算约 100 MiB），必须留在自检里。
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, .sdr_white_nits = 80, .label = "HDR scRGB + AVIF gain map", .sdr_format = ImageFormat::Avif}) &&
             ok;

        SPDLOG_INFO("=== 自检{}：输出目录 {} ===", ok ? "全部通过" : "失败",
                    wstring_to_utf8(config.output_dir.wstring()));
        return ok;
    }

}  // namespace gamesnap

#endif  // GAMESNAP_SELFTEST
