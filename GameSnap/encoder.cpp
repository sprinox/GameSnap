#include "pch.h"

#include "encoder.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#include "codecs.h"
#include "d3d.h"
#include "simd.h"
#include "tonemap.h"

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
            const auto first = title.find_first_not_of(L" ."); // 把字符串开头和结尾的“空格”和“点”全部删掉
            const auto last = title.find_last_not_of(L" .");
            if (first == std::wstring::npos)
                title.clear();
            else
                title = title.substr(first, last - first + 1);

            constexpr std::size_t max_title_length = 80;
            if (title.size() > max_title_length) {
                title.resize(max_title_length);
                // 不要把 UTF-16 代理对截成半个字符。
                if (!title.empty() && title.back() >= 0xD800 && title.back() <= 0xDBFF)
                    title.pop_back();
            }
            if (title.empty())
                title = L"GameSnap";
            return title;
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

        std::filesystem::path make_output_path(std::filesystem::path const& dir, Clock::time_point time,
                                               std::wstring title, std::uint64_t sequence,
                                               std::wstring_view extension, std::wstring_view suffix) {
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
            std::wstring name = sanitize_filename(std::move(title)) + L"_" + utf8_to_wstring(stamp);
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

        struct ToneLut {
            bool pq = false;
            std::uint32_t white = 0;
            std::array<std::uint8_t, kToneLutSize> values{};
        };

        struct ToneLutCache {
            std::mutex mutex;
            std::shared_ptr<const ToneLut> current;
            // 快速判断是否为空；允许与 mutex 保护的 current 有短暂不一致，
            // 只用于避免绝大多数 SDR job 拿锁。
            std::atomic<bool> empty{true};
        };

        ToneLutCache& tone_lut_cache() {
            static ToneLutCache cache;
            return cache;
        }

        void release_tone_lut() {
            auto& cache = tone_lut_cache();
            if (cache.empty.load(std::memory_order_acquire))
                return;
            std::scoped_lock lock(cache.mutex);
            cache.current.reset();
            cache.empty.store(true, std::memory_order_release);
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

            std::shared_ptr<ID3D11Texture2D> readback;
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
                {
                    std::scoped_lock lock(d3d.context_mutex());
                    d3d.context()->CopyResource(readback.get(), texture);
                }
                texture = readback.get();
                texture->GetDesc(&desc);
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
            const auto map_deadline = SteadyClock::now() + std::chrono::seconds(5);
            for (;;) {
                HRESULT hr = E_FAIL;
                {
                    std::scoped_lock lock(d3d.context_mutex());
                    hr = d3d.context()->Map(texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                }
                if (hr == S_OK)
                    break;
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                    error = fmt::format("Map staging 纹理失败: {}", describe_hresult(hr));
                    return false;
                }
                if (SteadyClock::now() >= map_deadline) {
                    error = "Map staging 纹理超时";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

            // FP16：HDR10(PQ) 或 scRGB 的色调映射到 SDR。
            // 逐像素做 PQ EOTF + sRGB OETF（各含一次 pow）在 4K 上要一秒多，
            // 所以先把“输入码值 → 8bit sRGB”预计算成 LUT，热路径上只查表。
            const bool is_pq = color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                               color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
            const float sdr_white = static_cast<float>(std::max<std::uint32_t>(sdr_white_nits, 1));

            // PQ 的输入域是 [0,1]；scRGB 统一按 [0,4] 采样，超出部分当作白色。
            const float domain = is_pq ? 1.0F : 4.0F;

            // LUT 只依赖 (PQ/scRGB, SDR 白点)，首次用到时构建，之后常驻（16 KB）。
            // 只有 HDR CPU 回退路径会进来，SDR 编码不会构建也不需要释放它。
            auto& lut_cache = tone_lut_cache();
            const std::uint32_t white_key = std::max<std::uint32_t>(sdr_white_nits, 1);
            std::shared_ptr<const ToneLut> lut;
            {
                std::scoped_lock lock(lut_cache.mutex);
                if (!lut_cache.current || lut_cache.current->pq != is_pq ||
                    lut_cache.current->white != white_key) {
                    auto next = std::make_shared<ToneLut>();
                    next->pq = is_pq;
                    next->white = white_key;
                    for (std::size_t i = 0; i < kToneLutSize; ++i) {
                        const float value =
                            static_cast<float>(i) / static_cast<float>(kToneLutSize - 1) * domain;
                        const float linear = is_pq ? (pq_to_nits(value) / sdr_white)
                                                   : (value * 80.0F / sdr_white);
                        next->values[i] = linear_to_srgb8(linear);
                    }
                    lut_cache.current = std::move(next);
                }
                lut = lut_cache.current;
                lut_cache.empty.store(false, std::memory_order_release);
            }
            const auto& table = lut->values;
            const auto sample = [&table, domain](float value) -> std::uint8_t {
                if (!(value > 0.0F))  // 同时拦掉 NaN
                    return 0;
                if (value >= domain)
                    return table.back();
                const auto index = static_cast<std::size_t>(value / domain * (kToneLutSize - 1));
                return table[index];
            };

            std::vector<float> row_floats(static_cast<std::size_t>(raw.width) * 4);
            for (std::uint32_t y = 0; y < raw.height; ++y) {
                const auto* src = reinterpret_cast<const std::uint16_t*>(raw.pixels.data() + (static_cast<std::size_t>(y) * raw.stride));
                half_to_float_row(src, row_floats.data(), static_cast<std::size_t>(raw.width) * 4);
                std::uint8_t* dst = out.pixels.data() + (static_cast<std::size_t>(y) * out.stride);
                for (std::uint32_t x = 0; x < raw.width; ++x) {
                    std::uint8_t* q = dst + (static_cast<std::size_t>(x) * 4);
                    q[0] = sample(row_floats[static_cast<std::size_t>(x) * 4 + 2]);
                    q[1] = sample(row_floats[static_cast<std::size_t>(x) * 4 + 1]);
                    q[2] = sample(row_floats[static_cast<std::size_t>(x) * 4 + 0]);
                    q[3] = 255;
                }
            }
            return true;
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

    EncodeWorkers::EncodeWorkers(std::filesystem::path output_dir, std::shared_ptr<EncodeQueue> jobs,
                                 CodecOptions options, ToneMapBackend tone_map_backend,
                                 std::shared_ptr<UiBridge> ui)
        : output_dir_(std::move(output_dir)), jobs_(std::move(jobs)), ui_(std::move(ui)),
          options_(options), tone_map_backend_(tone_map_backend) {
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
        for (std::size_t i = 0; i < threads_.size(); ++i)
            jobs_->enqueue(EncodeJob{.stop = true});
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
        if (!job.d3d || !job.texture_slot || !job.texture_slot->texture) {
            SPDLOG_WARN("[#{:04}] 非法的编码任务", job.sequence);
            return;
        }

        // 所有大缓冲都按 job 动态分配，闲置时不占内存/显存。
        RawCopy raw;
        Image sdr;
        std::vector<std::uint16_t> hdr10;
        sdr.metadata.title = wstring_to_utf8(job.title);
        sdr.metadata.date_time = exif_date_time(job.local_time);
        // 保留旧行为：HDR 缓存 LUT，SDR job 结束后释放；empty 为 true 时不拿锁。
        auto lut_release = ScopeExit{[&] {
            if (!job.hdr)
                release_tone_lut();
        }};
        std::string error;
        D3D11_TEXTURE2D_DESC texture_desc{};
        job.texture_slot->texture->GetDesc(&texture_desc);

        // HDR 优先走 GPU：mapper 直接采样 DEFAULT+SRV 纹理，fence/event 等待 GPU 完成。
        // 只有它失败时才把 DEFAULT 纹理回读到 staging，再走 CPU 回退。
        bool gpu_ok = false;
        if (job.hdr && tone_map_backend_ != ToneMapBackend::Cpu) {
            auto mapper = std::make_unique<ToneMapper>(job.d3d);
            if (!mapper->ok()) {
                SPDLOG_WARN("tone mapper 初始化失败: {}", mapper->init_error());
            } else {
                gpu_ok = mapper->map(job.texture_slot->texture.get(), job.color_space,
                                     job.sdr_white_nits, job.tone_map, sdr, hdr10, error);
                if (!gpu_ok)
                    SPDLOG_WARN("[#{:04}] compute tone map 失败，回退 CPU: {}", job.sequence, error);
            }
        }

        if (!gpu_ok) {
            if (!map_to_cpu(*job.d3d, job.texture_slot->texture.get(), job.gpu_fence, raw, error)) {
                SPDLOG_ERROR("[#{:04}] 取像素失败: {}", job.sequence, error);
                return;
            }
        }
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
            const auto hdr_path = make_output_path(output_dir_, job.local_time, job.title, job.sequence,
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

            if (!gpu_ok && !convert_pixels(raw, job.color_space, job.sdr_white_nits, sdr)) {
                SPDLOG_ERROR("[#{:04}] 不支持的像素格式 {}", job.sequence, static_cast<int>(raw.format));
                return;
            }
            if (job.sdr_format == ImageFormat::Heic)
                SPDLOG_WARN("[#{:04}] libheif 不支持 gain map，HEIC SDR 不包含 HDR 重建信息", job.sequence);
            const auto sdr_path = make_output_path(output_dir_, job.local_time, job.title, job.sequence,
                                                   image_extension(job.sdr_format), L"SDR");
            if (ui_ && !sdr.pixels.empty())
                ui_->publish(hdr_path.filename().wstring(), sdr);
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
            const auto path = make_output_path(output_dir_, job.local_time, job.title, job.sequence,
                                               image_extension(job.sdr_format), {});
            if (ui_ && !sdr.pixels.empty())
                ui_->publish(path.filename().wstring(), sdr);
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
