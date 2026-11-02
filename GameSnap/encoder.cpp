#include "pch.h"

#include "encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#include "d3d.h"

namespace gamesnap {
    namespace {

        // HDR 色调映射 LUT 的采样点数：只影响 LUT 内存（16 KB）和建表耗时。
        constexpr std::size_t kToneLutSize = 16384;

        // 去掉 Windows 文件名里不允许的字符；空标题回退到 GameSnap。
        // 按值接收，直接原地修改后返回，省掉一次拷贝。
        std::wstring sanitize_filename(std::wstring title) {
            for (wchar_t& ch : title) {
                const bool invalid = ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
                                     ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*';
                if (invalid)
                    ch = L'_';
            }
            const auto first = title.find_first_not_of(L" .");
            const auto last = title.find_last_not_of(L" .");
            if (first == std::wstring::npos)
                title.clear();
            else
                title = title.substr(first, last - first + 1);

            constexpr std::size_t max_title_length = 80;
            if (title.size() > max_title_length)
                title.resize(max_title_length);
            if (title.empty())
                title = L"GameSnap";
            return title;
        }

        // 文件名：<前台窗口标题>_YYYYMMDD_HHMMSS_mmm_序号.bmp（本地时间，std::chrono 格式化）
        std::filesystem::path make_output_path(std::filesystem::path const& dir, Clock::time_point time,
                                               std::wstring title, std::uint64_t sequence) {
            // fmt 没有 zoned_time 的 formatter，用 C++20 chrono 拆出本地日期/时间再格式化。
            const auto local = std::chrono::zoned_time(std::chrono::current_zone(), time).get_local_time();
            const auto days = std::chrono::floor<std::chrono::days>(local);
            const std::chrono::year_month_day date{days};
            const std::chrono::hh_mm_ss clock{local - days};
            const auto millis =
                std::chrono::duration_cast<std::chrono::milliseconds>(local.time_since_epoch()).count() % 1000;
            const std::string stamp = fmt::format(
                "{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}_{:04}",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                clock.hours().count(), clock.minutes().count(), clock.seconds().count(),
                millis, static_cast<unsigned>(sequence % 10000U));
            return dir / (sanitize_filename(std::move(title)) + L"_" + utf8_to_wstring(stamp) + L".bmp");
        }

        // ── 半精度浮点 → float（避免引入 DirectXMath 依赖） ──────────────────
        inline float half_to_float(std::uint16_t value) {
            const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16;
            std::uint32_t exponent = (value >> 10) & 0x1FU;
            std::uint32_t mantissa = value & 0x3FFU;
            std::uint32_t bits = 0;

            if (exponent == 0x1F) {  // Inf / NaN
                bits = sign | 0x7F800000U | (mantissa << 13);
            } else if (exponent != 0) {
                bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
            } else if (mantissa == 0) {
                bits = sign;
            } else {  // 次正规数
                exponent = 127 - 15 + 1;
                while ((mantissa & 0x400U) == 0) {
                    mantissa <<= 1;
                    --exponent;
                }
                mantissa &= 0x3FFU;
                bits = sign | (exponent << 23) | (mantissa << 13);
            }

            float result = 0.0F;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }

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
        struct RawCopy {
            std::vector<std::uint8_t> bytes;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t stride = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        };

        // 统一成 32bpp BGRA，BMP 直接按这个顺序写盘。
        struct Image {
            std::vector<std::uint8_t> pixels;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t stride = 0;
        };

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
        // 锁只覆盖 Map/拷贝/Unmap，像素转换和写盘都在锁外。
        bool map_to_cpu(D3DContext& d3d, ID3D11Texture2D* texture, RawCopy& raw, std::string& error) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
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
            raw.bytes.resize(static_cast<std::size_t>(raw.stride) * raw.height);

            std::scoped_lock lock(d3d.context_mutex());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = d3d.context()->Map(texture, 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr)) {
                error = fmt::format("Map staging 纹理失败: {}", describe_hresult(hr));
                return false;
            }
            for (std::uint32_t y = 0; y < raw.height; ++y) {
                std::memcpy(raw.bytes.data() + (static_cast<std::size_t>(y) * raw.stride),
                            static_cast<const std::uint8_t*>(mapped.pData) + (static_cast<std::size_t>(y) * mapped.RowPitch),
                            raw.stride);
            }
            d3d.context()->Unmap(texture, 0);
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

            // SDR + BGRA：完全不需要转换，直接把缓冲搬过去。
            if (is_bgra) {
                out.pixels = std::move(raw.bytes);
                return true;
            }

            out.pixels.resize(static_cast<std::size_t>(out.stride) * raw.height);

            if (is_rgba) {
                for (std::uint32_t y = 0; y < raw.height; ++y) {
                    const std::uint8_t* src = raw.bytes.data() + (static_cast<std::size_t>(y) * raw.stride);
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

            // FP16：HDR10(PQ) 或 scRGB 的色调映射到 SDR。
            // 逐像素做 PQ EOTF + sRGB OETF（各含一次 pow）在 4K 上要一秒多，
            // 所以先把“输入码值 → 8bit sRGB”预计算成 LUT，热路径上只查表。
            const bool is_pq = color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                               color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
            const float sdr_white = static_cast<float>(std::max<std::uint32_t>(sdr_white_nits, 1));

            // PQ 的输入域是 [0,1]；scRGB 中 SDR 白点在 80 nit 单位下可到 ~2.5，
            // 高光更大，统一按 [0,4] 采样，超出部分直接当作白色。
            const float domain = is_pq ? 1.0F : 4.0F;
            std::array<std::uint8_t, kToneLutSize> lut{};
            for (std::size_t i = 0; i < kToneLutSize; ++i) {
                const float value = static_cast<float>(i) / static_cast<float>(kToneLutSize - 1) * domain;
                const float linear = is_pq ? (pq_to_nits(value) / sdr_white)
                                           : (value * 80.0F / sdr_white);
                lut[i] = linear_to_srgb8(linear);
            }
            const auto sample = [&lut, domain](float value) -> std::uint8_t {
                if (!(value > 0.0F))  // 同时拦掉 NaN
                    return 0;
                if (value >= domain)
                    return lut.back();
                const auto index = static_cast<std::size_t>(value / domain * (kToneLutSize - 1));
                return lut[index];
            };

            for (std::uint32_t y = 0; y < raw.height; ++y) {
                const auto* src = reinterpret_cast<const std::uint16_t*>(raw.bytes.data() + (static_cast<std::size_t>(y) * raw.stride));
                std::uint8_t* dst = out.pixels.data() + (static_cast<std::size_t>(y) * out.stride);
                for (std::uint32_t x = 0; x < raw.width; ++x) {
                    const std::uint16_t* p = src + (static_cast<std::size_t>(x) * 4);
                    std::uint8_t* q = dst + (static_cast<std::size_t>(x) * 4);
                    q[0] = sample(half_to_float(p[2]));
                    q[1] = sample(half_to_float(p[1]));
                    q[2] = sample(half_to_float(p[0]));
                    q[3] = 255;
                }
            }
            return true;
        }

        // 写 32bpp BMP（BITMAPFILEHEADER + BITMAPINFOHEADER，bottom-up）。
        // TODO: 以后要 PNG / JPEG 的话在这里加对应的编码分支。
        bool write_bmp(std::filesystem::path const& path, Image const& image) {
            if (image.pixels.empty() || image.width == 0 || image.height == 0)
                return false;

            const std::uint32_t row_bytes = image.width * 4;  // 32bpp，天然 4 字节对齐
            BITMAPFILEHEADER file_header{};
            file_header.bfType = 0x4D42;  // 'BM'
            file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            file_header.bfSize = file_header.bfOffBits + (row_bytes * image.height);

            BITMAPINFOHEADER info_header{};
            info_header.biSize = sizeof(BITMAPINFOHEADER);
            info_header.biWidth = static_cast<LONG>(image.width);
            info_header.biHeight = static_cast<LONG>(image.height);
            info_header.biPlanes = 1;
            info_header.biBitCount = 32;
            info_header.biCompression = BI_RGB;
            info_header.biSizeImage = row_bytes * image.height;

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file.write(reinterpret_cast<char const*>(&file_header), sizeof(file_header));
            file.write(reinterpret_cast<char const*>(&info_header), sizeof(info_header));
            // BMP 是 bottom-up：从最后一行往上写。
            for (std::uint32_t y = image.height; y-- > 0;) {
                const auto offset = static_cast<std::size_t>(y) * row_bytes;
                file.write(reinterpret_cast<char const*>(image.pixels.data() + offset), row_bytes);
            }
            file.flush();
            return file.good();
        }

    }  // namespace

    // ── staging 纹理 ─────────────────────────────────────────────────────────

    // 只有一块主 staging 纹理；被编码线程占用时，截图线程用这个函数临时再建一块。
    std::shared_ptr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc) {
        if (device == nullptr)
            return nullptr;

        winrt::com_ptr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
            return nullptr;

        // job 析构时立刻 Release，临时纹理不会留下。
        return {texture.detach(), [](ID3D11Texture2D* raw) {
                    if (raw != nullptr)
                        raw->Release();
                }};
    }

    // ── EncodeWorkers ────────────────────────────────────────────────────────

    EncodeWorkers::EncodeWorkers(std::filesystem::path output_dir, std::shared_ptr<EncodeQueue> jobs)
        : output_dir_(std::move(output_dir)), jobs_(std::move(jobs)) {}

    EncodeWorkers::~EncodeWorkers() {
        stop();
    }

    void EncodeWorkers::start(std::size_t thread_count) {
        thread_count = std::clamp<std::size_t>(thread_count, 1, 8);
        threads_.reserve(thread_count);
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
        // 每个线程一个哨兵；队列是 FIFO，所以前面的任务会先被编码完。
        for (std::size_t i = 0; i < threads_.size(); ++i)
            jobs_->enqueue(EncodeJob{.stop = true});
        for (auto& thread : threads_) {
            if (thread.joinable())
                thread.join();
        }
        threads_.clear();
    }

    void EncodeWorkers::worker_main(std::size_t index) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const std::wstring thread_name = utf8_to_wstring(fmt::format("GameSnap.Encode.{}", index));
        ::SetThreadDescription(::GetCurrentThread(), thread_name.c_str());

        // 每个编码线程自己的 consumer token，队列按线程分开缓存，争用更小。
        moodycamel::ConsumerToken token(*jobs_);
        for (;;) {
            EncodeJob job;
            jobs_->wait_dequeue(token, job);
            if (job.stop)
                break;
            // 单张图编码失败不能让整个线程挂掉（更不能让异常逃出 std::thread）。
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

        SPDLOG_INFO("编码线程 {} 退出", index);
    }

    void EncodeWorkers::encode(EncodeJob& job) const {
        if (!job.d3d || !job.texture) {
            SPDLOG_WARN("[#{:04}] 非法的编码任务", job.sequence);
            return;
        }

        RawCopy raw;
        std::string error;
        if (!map_to_cpu(*job.d3d, job.texture.get(), raw, error)) {
            SPDLOG_ERROR("[#{:04}] 取像素失败: {}", job.sequence, error);
            return;
        }
        const auto mapped_at = Clock::now();

        Image image;
        if (!convert_pixels(raw, job.color_space, job.sdr_white_nits, image)) {
            SPDLOG_ERROR("[#{:04}] 不支持的像素格式 {}", job.sequence, static_cast<int>(raw.format));
            return;
        }
        const auto converted_at = Clock::now();

        const std::filesystem::path path =
            make_output_path(output_dir_, job.local_time, std::move(job.title), job.sequence);
        if (!write_bmp(path, image)) {
            SPDLOG_ERROR("[#{:04}] 写文件失败: {}", job.sequence, wstring_to_utf8(path.wstring()));
            return;
        }
        const auto encoded_at = Clock::now();

        const auto& t = job.timings;
        const double key_to_start = duration_ms(t.start_capture - t.input);
        const double start_to_frame = duration_ms(t.frame_received - t.start_capture);
        const double copy = duration_ms(t.copied - t.frame_received);
        const double pixel = duration_ms(mapped_at - t.copied);
        const double convert = duration_ms(converted_at - mapped_at);
        const double write = duration_ms(encoded_at - converted_at);
        const double total = duration_ms(encoded_at - t.input);

        SPDLOG_INFO(
            "[#{:04}] {} {}x{} bmp | 按键→StartCapture {:.2f}ms | StartCapture→首帧 {:.2f}ms | "
            "GPU 拷贝 {:.2f}ms | 取像素 {:.2f}ms | 转换 {:.2f}ms | 写盘 {:.2f}ms | 按键→落盘 {:.2f}ms",
            job.sequence, wstring_to_utf8(path.wstring()), image.width, image.height,
            key_to_start, start_to_frame, copy, pixel, convert, write, total);
    }

}  // namespace gamesnap
