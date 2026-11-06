#include "pch.h"

#include "selftest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include "d3d.h"
#include "display.h"
#include "encoder.h"
#include "messages.h"

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

        // PQ 编码值 0..0.58 约等于 0..203 nit（SDR 白点），色调映射后应是完整灰阶。
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
        };

        std::shared_ptr<ID3D11Texture2D> create_gpu_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc) {
            winrt::com_ptr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
                return {};
            return std::shared_ptr<ID3D11Texture2D>{texture.detach(), [](ID3D11Texture2D* p) {
                if (p != nullptr)
                    p->Release();
            }};
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
            EncodeWorkers workers(config.output_dir, jobs, config.codec, config.tone_map_backend, {});
            workers.start(1);

            std::error_code ec;
            std::vector<std::filesystem::path> before;
            for (const auto& entry : std::filesystem::directory_iterator(config.output_dir, ec))
                before.push_back(entry.path());

            EncodeJob job;
            auto texture_slot = std::make_shared<TextureSlot>();
            texture_slot->texture = std::move(target);
            texture_slot->hdr = hdr_target;
            job.texture_slot = std::move(texture_slot);
            job.d3d = d3d;
            job.color_space = test_case.color_space;
            job.sdr_white_nits = test_case.sdr_white_nits;
            job.hdr = test_case.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            job.hdr_format = config.hdr_format;
            job.sdr_format = config.sdr_format;
            job.tone_map = config.tone_map;
            job.sequence = 7777;
            job.title = L"GameSnap SelfTest";
            job.local_time = Clock::now();
            job.timings.input = SteadyClock::now();
            job.timings.dequeued = job.timings.input;
            job.timings.title_done = job.timings.input;
            job.timings.prepared = job.timings.input;
            job.timings.start_capture = job.timings.input;
            job.timings.frame_received = job.timings.input;
            job.timings.copied = job.timings.input;

            const bool hdr_job = job.hdr;
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
        auto d3d = D3DContext::create(display.monitor, &error);
        if (!d3d) {
            SPDLOG_ERROR("自检失败: 无法创建 D3D 设备: {}", error);
            return false;
        }

        bool ok = true;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_B8G8R8A8_UNORM, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, .sdr_white_nits = 80, .label = "SDR BGRA"}) &&
             ok;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R8G8B8A8_UNORM, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, .sdr_white_nits = 80, .label = "SDR RGBA"}) &&
             ok;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, .sdr_white_nits = 203, .label = "HDR10(PQ) FP16"}) &&
             ok;
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, .sdr_white_nits = 203, .label = "HDR10(PQ) FP16 641x361", .width = 641, .height = 361}) &&
             ok;
        // WGC 的 FP16 帧实际是 scRGB（线性，1.0 = 80 nits），这才是真实采集帧的编码。
        ok = run_case(config, d3d, Case{.format = DXGI_FORMAT_R16G16B16A16_FLOAT, .color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, .sdr_white_nits = 80, .label = "HDR scRGB FP16"}) &&
             ok;

        SPDLOG_INFO("=== 自检{}：输出目录 {} ===", ok ? "全部通过" : "失败",
                    wstring_to_utf8(config.output_dir.wstring()));
        return ok;
    }

}  // namespace gamesnap
