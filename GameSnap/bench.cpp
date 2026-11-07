#include "pch.h"

#include "bench.h"

// 见 bench.h：默认不编入（Release / Debug 都关）。
#if GAMESNAP_BENCH

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

#include <spng.h>

#include "codecs.h"
#include "image.h"

namespace gamesnap {
    namespace {

        using SteadyClock = std::chrono::steady_clock;

        // ── PNG 解码 ─────────────────────────────────────────────────────────
        struct DecodedImage {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            ByteBuffer bgra;  // 4 字节/像素、alpha 一律 255
        };

        bool load_png(std::filesystem::path const& path, DecodedImage& out, std::string& error) {
            std::FILE* file = nullptr;
            if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
                error = "打不开源文件";
                return false;
            }
            std::vector<std::uint8_t> bytes;
            std::array<std::uint8_t, 256 * 1024> chunk{};
            std::size_t read = 0;
            while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0)
                bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
            std::fclose(file);
            if (bytes.empty()) {
                error = "源文件为空";
                return false;
            }

            spng_ctx* ctx = spng_ctx_new(0);
            if (ctx == nullptr) {
                error = "spng_ctx_new 失败";
                return false;
            }
            bool ok = false;
            spng_ihdr ihdr{};
            std::vector<std::uint8_t> rgba;
            if (spng_set_png_buffer(ctx, bytes.data(), bytes.size()) == 0 && spng_get_ihdr(ctx, &ihdr) == 0) {
                std::size_t decoded_size = 0;
                if (spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &decoded_size) == 0) {
                    rgba.resize(decoded_size);
                    if (spng_decode_image(ctx, rgba.data(), rgba.size(), SPNG_FMT_RGBA8, 0) == 0) {
                        out.width = ihdr.width;
                        out.height = ihdr.height;
                        out.bgra.resize(static_cast<std::size_t>(out.width) * out.height * 4);
                        for (std::size_t i = 0, n = static_cast<std::size_t>(out.width) * out.height; i < n; ++i) {
                            out.bgra[i * 4 + 0] = rgba[i * 4 + 2];  // B
                            out.bgra[i * 4 + 1] = rgba[i * 4 + 1];  // G
                            out.bgra[i * 4 + 2] = rgba[i * 4 + 0];  // R
                            out.bgra[i * 4 + 3] = 255;              // 截图不透明
                        }
                        ok = true;
                    }
                }
            }
            spng_ctx_free(ctx);
            if (!ok)
                error = "PNG 解码失败";
            return ok;
        }

        // ── 一个待测组合 ─────────────────────────────────────────────────────
        struct Case {
            std::string label;
            ImageFormat format = ImageFormat::Png;
            CodecOptions options;
        };

        struct Result {
            std::string label;
            std::string file;
            std::uint64_t bytes = 0;
            double ms = 0.0;
            bool ok = false;
            std::string error;
        };

        CodecOptions base_options(Config const& config) {
            CodecOptions options = config.codec;
            // 基准点：一律无损、444 优先，避免混进有损差异。
            options.webp_lossless = true;
            options.avif_lossless = true;
            options.heic_lossless = true;
            options.jxl_lossless = true;
            options.jxl_distance = 0.0F;
            options.chroma = ChromaSubsampling::Yuv444;
            return options;
        }

        std::vector<Case> make_cases(Config const& config) {
            std::vector<Case> cases;
            const CodecOptions base = base_options(config);

            cases.push_back(Case{"bmp  (无压缩)", ImageFormat::Bmp, base});

            for (int level : {1, 6, 9}) {
                auto o = base;
                o.png_compression = level;
                cases.push_back(Case{fmt::format("png  c={} (RGB, deflate)", level), ImageFormat::Png, o});
            }

            for (int method : {0, 4, 6}) {
                auto o = base;
                o.webp_method = method;
                cases.push_back(Case{fmt::format("webp method={} (RGB 无损)", method), ImageFormat::Webp, o});
            }

            for (int effort : {1, 3, 5, 6, 7, 9}) {
                auto o = base;
                o.jxl_effort = effort;
                cases.push_back(Case{fmt::format("jxl  effort={} (RGB 无损)", effort), ImageFormat::Jxl, o});
            }

            for (int speed : {6, 10}) {
                auto o = base;
                o.av1_encoder = Av1Encoder::Aom;
                o.avif_speed = speed;
                o.chroma = ChromaSubsampling::Yuv444;
                cases.push_back(
                    Case{fmt::format("avif aom s={} (YUV444 无损)", speed), ImageFormat::Avif, o});
            }

            for (int speed : {6, 10}) {
                auto o = base;
                o.av1_encoder = Av1Encoder::Svt;
                o.avif_speed = speed;
                o.chroma = ChromaSubsampling::Yuv420;  // SVT 只支持 4:2:0
                cases.push_back(
                    Case{fmt::format("avif svt s={} (YUV420, 非真无损)", speed), ImageFormat::Avif, o});
            }

            for (std::string_view preset : {"fast", "medium", "slow"}) {
                auto o = base;
                o.heic_preset = preset;
                o.chroma = ChromaSubsampling::Yuv444;
                cases.push_back(
                    Case{fmt::format("heic x265 preset={} (YUV444 无损)", preset), ImageFormat::Heic, o});
            }

            // JPEG 只能有损：作为"YUV + 8bit"的体积参考放进来。
            {
                auto o = base;
                o.jpeg_quality = 100;
                cases.push_back(Case{"jpeg q=100 (YUV444 有损, 参考)", ImageFormat::Jpeg, o});
            }
            return cases;
        }

        // ── 16-bit PNG（HDR 源，PQ/BT.2020 RGB48）→ 10-bit PQ ────────────────
        struct HdrSource {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            U16Buffer rgba10;
        };

        bool load_png_hdr(std::filesystem::path const& path, HdrSource& out, std::string& error) {
            std::FILE* file = nullptr;
            if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
                error = "打不开源文件";
                return false;
            }
            std::vector<std::uint8_t> bytes;
            std::array<std::uint8_t, 256 * 1024> chunk{};
            std::size_t read = 0;
            while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0)
                bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
            std::fclose(file);

            spng_ctx* ctx = spng_ctx_new(0);
            if (ctx == nullptr) {
                error = "spng_ctx_new 失败";
                return false;
            }
            bool ok = false;
            spng_ihdr ihdr{};
            std::vector<std::uint8_t> rgba16;
            if (!bytes.empty() && spng_set_png_buffer(ctx, bytes.data(), bytes.size()) == 0 &&
                spng_get_ihdr(ctx, &ihdr) == 0 && ihdr.bit_depth == 16) {
                std::size_t decoded_size = 0;
                if (spng_decoded_image_size(ctx, SPNG_FMT_RGBA16, &decoded_size) == 0) {
                    rgba16.resize(decoded_size);
                    if (spng_decode_image(ctx, rgba16.data(), rgba16.size(), SPNG_FMT_RGBA16, 0) == 0) {
                        out.width = ihdr.width;
                        out.height = ihdr.height;
                        const std::size_t pixels = static_cast<std::size_t>(out.width) * out.height;
                        out.rgba10.resize(pixels * 4);
                        const auto* src = reinterpret_cast<std::uint16_t const*>(rgba16.data());
                        for (std::size_t i = 0; i < pixels * 4; ++i)
                            out.rgba10[i] = static_cast<std::uint16_t>(src[i] >> 6);  // 16 → 10 bit
                        ok = true;
                    }
                }
            }
            spng_ctx_free(ctx);
            if (!ok)
                error = "16-bit PNG 解码失败（HDR 基准需要 rgb48 源）";
            return ok;
        }

        // PQ → nits → BT.709 → sRGB，给 gain map 造一个 SDR 底图（基准只关心体积/耗时）。
        inline float bench_pq_to_nits(float pq) {
            constexpr float m1 = 2610.0F / 16384.0F;
            constexpr float m2 = 2523.0F / 4096.0F * 128.0F;
            constexpr float c1 = 3424.0F / 4096.0F;
            constexpr float c2 = 2413.0F / 4096.0F * 32.0F;
            constexpr float c3 = 2392.0F / 4096.0F * 32.0F;
            const float p = std::pow(std::clamp(pq, 0.0F, 1.0F), 1.0F / m2);
            const float num = std::max(p - c1, 0.0F);
            const float den = c2 - (c3 * p);
            if (den <= 0.0F)
                return 0.0F;
            return 10000.0F * std::pow(num / den, 1.0F / m1);
        }

        inline std::uint8_t bench_linear_to_srgb8(float linear) {
            if (!(linear > 0.0F))
                return 0;
            if (linear >= 1.0F)
                return 255;
            const float encoded = linear <= 0.0031308F ? linear * 12.92F
                                                        : (1.055F * std::pow(linear, 1.0F / 2.4F)) - 0.055F;
            return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
        }

        Image hdr10_to_sdr(HdrSource const& source, float sdr_white) {
            Image image;
            image.width = source.width;
            image.height = source.height;
            image.stride = source.width * 4;
            image.pixels.resize(static_cast<std::size_t>(image.stride) * image.height);
            image.metadata.title = "bench-hdr";
            image.metadata.date_time = "2026:11:07 00:00:00";
            for (std::size_t i = 0, n = static_cast<std::size_t>(source.width) * source.height; i < n; ++i) {
                const float r = bench_pq_to_nits(static_cast<float>(source.rgba10[i * 4 + 0]) / 1023.0F);
                const float g = bench_pq_to_nits(static_cast<float>(source.rgba10[i * 4 + 1]) / 1023.0F);
                const float b = bench_pq_to_nits(static_cast<float>(source.rgba10[i * 4 + 2]) / 1023.0F);
                // BT.2020 → BT.709
                const float r709 = (1.6605F * r) + (-0.5876F * g) + (-0.0728F * b);
                const float g709 = (-0.1246F * r) + (1.1329F * g) + (-0.0083F * b);
                const float b709 = (-0.0182F * r) + (-0.1006F * g) + (1.1187F * b);
                auto* q = image.pixels.data() + i * 4;
                q[0] = bench_linear_to_srgb8(b709 / sdr_white);
                q[1] = bench_linear_to_srgb8(g709 / sdr_white);
                q[2] = bench_linear_to_srgb8(r709 / sdr_white);
                q[3] = 255;
            }
            return image;
        }

        // HDR 矩阵：10-bit PQ 直入，AVIF/HEIC 走 YUV，JXL 走 10-bit RGB；再测一档 gain map。
        struct HdrCase {
            std::string label;
            ImageFormat format = ImageFormat::Avif;
            CodecOptions options;
            bool gain_map = false;
        };
        std::vector<HdrCase> make_hdr_cases(Config const& config) {
            std::vector<HdrCase> cases;
            const CodecOptions base = base_options(config);

            for (int speed : {6, 10}) {
                auto o = base;
                o.av1_encoder = Av1Encoder::Aom;
                o.avif_speed = speed;
                o.chroma = ChromaSubsampling::Yuv444;
                cases.push_back(HdrCase{fmt::format("avif aom s={} (10bit PQ, YUV444)", speed),
                                        ImageFormat::Avif, o, false});
            }
            {
                auto o = base;
                o.av1_encoder = Av1Encoder::Svt;
                o.avif_speed = 10;
                o.chroma = ChromaSubsampling::Yuv420;
                cases.push_back(
                    HdrCase{"avif svt s=10 (10bit PQ, YUV420 有损)", ImageFormat::Avif, o, false});
            }
            {
                auto o = base;
                o.heic_preset = "fast";
                o.chroma = ChromaSubsampling::Yuv444;
                cases.push_back(HdrCase{"heic fast (8bit PQ 回退, YUV444)", ImageFormat::Heic, o, false});
            }
            for (int effort : {3, 5, 6, 7}) {
                auto o = base;
                o.jxl_effort = effort;
                cases.push_back(HdrCase{fmt::format("jxl e{} (10bit RGB 无损)", effort), ImageFormat::Jxl, o, false});
            }
            {
                auto o = base;
                o.av1_encoder = Av1Encoder::Aom;
                o.avif_speed = 10;
                o.chroma = ChromaSubsampling::Yuv444;
                cases.push_back(HdrCase{"avif aom s=10 + gain map (SDR 底图)", ImageFormat::Avif, o, true});
            }
            return cases;
        }

        std::wstring_view extension_of(ImageFormat format) {
            switch (format) {
            case ImageFormat::Bmp:
                return L".bmp";
            case ImageFormat::Png:
                return L".png";
            case ImageFormat::Jpeg:
                return L".jpg";
            case ImageFormat::Webp:
                return L".webp";
            case ImageFormat::Avif:
                return L".avif";
            case ImageFormat::Heic:
                return L".heic";
            case ImageFormat::Jxl:
                return L".jxl";
            }
            return L".bin";
        }

    }  // namespace

    // 定义在文件末尾；run_encode_bench 先用到它。
    bool run_hdr_bench(Config const& config, HdrSource const& source);

    bool run_encode_bench(Config const& config) {
        if (!config.bench_source) {
            SPDLOG_ERROR("基准模式需要 --bench <源 PNG>");
            return false;
        }

        // 16-bit PNG = HDR 源（PQ/BT.2020），走另一套矩阵；8-bit 走 SDR 矩阵。
        {
            HdrSource probe;
            std::string hdr_error;
            if (load_png_hdr(*config.bench_source, probe, hdr_error)) {
                return run_hdr_bench(config, probe);
            }
        }

        DecodedImage source;
        std::string error;
        if (!load_png(*config.bench_source, source, error)) {
            SPDLOG_ERROR("读取源图失败: {} ({})", wstring_to_utf8(config.bench_source->wstring()), error);
            return false;
        }
        SPDLOG_INFO("=== 编码基准：{}（{}x{}）===", wstring_to_utf8(config.bench_source->filename().wstring()),
                    source.width, source.height);

        Image image;
        image.width = source.width;
        image.height = source.height;
        image.stride = source.width * 4;
        image.pixels = std::move(source.bgra);
        image.metadata.title = "bench";
        image.metadata.date_time = "2026:11:07 00:00:00";

        std::error_code ec;
        std::filesystem::create_directories(config.output_dir, ec);

        const auto cases = make_cases(config);
        std::vector<Result> results;
        results.reserve(cases.size());

        const std::uint64_t raw_bytes = static_cast<std::uint64_t>(image.stride) * image.height;
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const Case& item = cases[index];
            const std::filesystem::path path =
                config.output_dir / fmt::format(L"bench_{:02d}{}", index, extension_of(item.format));

            set_codec_options(item.options);
            Result result;
            result.label = item.label;
            result.file = wstring_to_utf8(path.filename().wstring());

            const auto start = SteadyClock::now();
            result.ok = write_sdr(path, item.format, image, error);
            const auto finish = SteadyClock::now();
            result.ms = std::chrono::duration<double, std::milli>(finish - start).count();

            if (result.ok) {
                const auto size = std::filesystem::file_size(path, ec);
                result.bytes = ec ? 0 : static_cast<std::uint64_t>(size);
            } else {
                result.error = error;
            }
            SPDLOG_INFO("[{:2}/{}] {:<38} {:>10} 字节  {:>10.1f} ms{}", index + 1, cases.size(), result.label,
                        result.bytes, result.ms, result.ok ? "" : ("  失败: " + result.error));
            results.push_back(std::move(result));
        }

        // ── 汇总表（按体积排序）────────────────────────────────────────────
        std::vector<Result const*> ranked;
        for (const auto& result : results) {
            if (result.ok)
                ranked.push_back(&result);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](Result const* a, Result const* b) { return a->bytes < b->bytes; });

        const auto baseline = std::find_if(ranked.begin(), ranked.end(),
                                           [](Result const* r) { return r->label.rfind("bmp", 0) == 0; });
        const std::uint64_t bmp_bytes = baseline != ranked.end() ? (*baseline)->bytes : 0;

        SPDLOG_INFO("=== 结果（按体积升序；倍率相对 BMP 未压缩 {} 字节）===", bmp_bytes);
        for (Result const* result : ranked) {
            const double ratio = bmp_bytes != 0 ? static_cast<double>(bmp_bytes) / static_cast<double>(result->bytes)
                                                : 0.0;
            const double mb_per_s = result->ms > 0.5 ? (static_cast<double>(result->bytes) / 1.0e6) / (result->ms / 1000.0)
                                                      : 0.0;
            SPDLOG_INFO("{:<38} {:>10} 字节  压缩 {:5.1f}x  {:>10.1f} ms  {:>7.2f} MB/s", result->label, result->bytes,
                        ratio, result->ms, mb_per_s);
        }

        // ── CSV（方便画图）──────────────────────────────────────────────────
        const std::filesystem::path csv_path = config.output_dir / L"bench.csv";
        std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
        if (csv) {
            csv << "label,file,bytes,ms,ok,raw_bytes\n";
            for (const auto& result : results) {
                csv << '"' << result.label << "\"," << result.file << ',' << result.bytes << ',' << result.ms << ','
                    << (result.ok ? 1 : 0) << ',' << raw_bytes << '\n';
            }
            SPDLOG_INFO("CSV: {}", wstring_to_utf8(csv_path.wstring()));
        }
        return std::all_of(results.begin(), results.end(), [](Result const& r) { return r.ok; });
    }

    // ── HDR 基准 ─────────────────────────────────────────────────────────────
    bool run_hdr_bench(Config const& config, HdrSource const& source) {
        SPDLOG_INFO("=== HDR 编码基准：{}（{}x{}，10-bit PQ）===",
                    wstring_to_utf8(config.bench_source->filename().wstring()), source.width, source.height);

        std::error_code ec;
        std::filesystem::create_directories(config.output_dir, ec);

        const float sdr_white = 203.0F;  // HDR10 常用参考白点
        const Image sdr_base = hdr10_to_sdr(source, sdr_white);

        const auto cases = make_hdr_cases(config);
        std::vector<Result> results;
        results.reserve(cases.size());
        const std::uint64_t raw_bytes = static_cast<std::uint64_t>(source.width) * source.height * 8;

        for (std::size_t index = 0; index < cases.size(); ++index) {
            const HdrCase& item = cases[index];
            const std::filesystem::path path =
                config.output_dir / fmt::format(L"hdr_{:02d}{}", index, extension_of(item.format));

            set_codec_options(item.options);
            Result result;
            result.label = item.label;
            result.file = wstring_to_utf8(path.filename().wstring());

            const auto start = SteadyClock::now();
            if (item.gain_map) {
                result.ok = encode_avif_gainmap(path, sdr_base, source.rgba10.data(), source.width,
                                                source.height, result.error);
            } else if (item.format == ImageFormat::Avif) {
                result.ok = encode_avif_hdr10(path, source.rgba10.data(), source.width, source.height,
                                              sdr_base.metadata, result.error);
            } else if (item.format == ImageFormat::Heic) {
                result.ok = encode_heic_hdr10(path, source.rgba10.data(), source.width, source.height,
                                              sdr_base.metadata, result.error);
            } else {
                result.ok = write_hdr10(path, item.format, source.rgba10.data(), source.width, source.height,
                                        sdr_base.metadata, result.error);
            }
            const auto finish = SteadyClock::now();
            result.ms = std::chrono::duration<double, std::milli>(finish - start).count();

            if (result.ok) {
                const auto size = std::filesystem::file_size(path, ec);
                result.bytes = ec ? 0 : static_cast<std::uint64_t>(size);
            }
            SPDLOG_INFO("[{}/{}] {:<42} {:>10} 字节  {:>10.1f} ms{}", index + 1, cases.size(), result.label,
                        result.bytes, result.ms, result.ok ? "" : ("  失败: " + result.error));
            results.push_back(std::move(result));
        }

        const std::filesystem::path csv_path = config.output_dir / L"bench_hdr.csv";
        std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
        if (csv) {
            csv << "label,file,bytes,ms,ok,raw_bytes\n";
            for (const auto& result : results) {
                csv << '"' << result.label << "\"," << result.file << ',' << result.bytes << ',' << result.ms << ','
                    << (result.ok ? 1 : 0) << ',' << raw_bytes << '\n';
            }
            SPDLOG_INFO("CSV: {}", wstring_to_utf8(csv_path.wstring()));
        }
        return std::all_of(results.begin(), results.end(), [](Result const& r) { return r.ok; });
    }

}  // namespace gamesnap

#endif  // GAMESNAP_BENCH
