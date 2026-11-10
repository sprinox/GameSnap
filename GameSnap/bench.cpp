#include "pch.h"

#include "bench.h"

// 见 bench.h：默认不编入（Release / Debug 都关）。
#if GAMESNAP_BENCH

#    include <array>
#    include <chrono>
#    include <cmath>
#    include <cstdio>
#    include <cwctype>
#    include <fstream>
#    include <vector>

#    include <wincodec.h>
// WIC 只有基准工具用（JXR 解码），所以链接依赖放在这里，主程序不背这个 lib。
#    pragma comment(lib, "windowscodecs.lib")

#    include <jxl/decode.h>
#    include <spng.h>

#    include "codecs.h"
#    include "encoder.h"
#    include "image.h"
#    include "simd.h"

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
    // 目录模式：按“当前配置”（默认编码器）逐张编码，算压缩率。
    bool run_dir_bench(Config const& config, std::filesystem::path const& directory);

    bool run_encode_bench(Config const& config) {
        if (!config.bench_source) {
            SPDLOG_ERROR("基准模式需要 --bench <源 PNG 或 目录>");
            return false;
        }

        // 目录模式：用当前配置逐张编码，只算体积/压缩率。
        {
            std::error_code dir_ec;
            if (std::filesystem::is_directory(*config.bench_source, dir_ec))
                return run_dir_bench(config, *config.bench_source);
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

    // ── 目录模式：按当前配置逐张编码，只关心体积/压缩率 ────────────────────
    namespace {

        // NVIDIA 的 HDR 截图是 JXR：线性 scRGB（1.0 = 80 nits，BT.709 原色）。
        struct ScRgbSource {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::vector<float> rgba;
        };

        bool load_jxr(std::filesystem::path const& path, ScRgbSource& out, std::string& error) {
            winrt::com_ptr<IWICImagingFactory> factory;
            if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(factory.put())))) {
                error = "CoCreateInstance(WIC) 失败";
                return false;
            }
            winrt::com_ptr<IWICBitmapDecoder> decoder;
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad, decoder.put()))) {
                error = "WIC 打不开文件（缺 JPEG XR 解码器？）";
                return false;
            }
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            UINT width = 0;
            UINT height = 0;
            if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(frame->GetSize(&width, &height)) ||
                width == 0 || height == 0) {
                error = "WIC 取帧/尺寸失败";
                return false;
            }
            winrt::com_ptr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(converter.put())) ||
                FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat64bppRGBAHalf,
                                             WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom))) {
                error = "WIC 转换到 RGBAHalf 失败";
                return false;
            }

            const std::size_t pixels = static_cast<std::size_t>(width) * height;
            std::vector<std::uint16_t> half(pixels * 4);
            const UINT stride = width * 8;  // 4 通道 * 2 字节
            if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(half.size() * 2),
                                             reinterpret_cast<BYTE*>(half.data())))) {
                error = "WIC CopyPixels 失败";
                return false;
            }
            out.width = width;
            out.height = height;
            out.rgba.resize(pixels * 4);
            half_to_float_row(half.data(), out.rgba.data(), pixels * 4);
            return true;
        }

        inline float nits_to_pq(float nits) {
            constexpr float m1 = 2610.0F / 16384.0F;
            constexpr float m2 = 2523.0F / 4096.0F * 128.0F;
            constexpr float c1 = 3424.0F / 4096.0F;
            constexpr float c2 = 2413.0F / 4096.0F * 32.0F;
            constexpr float c3 = 2392.0F / 4096.0F * 32.0F;
            const float y = std::pow(std::clamp(nits, 0.0F, 10000.0F) / 10000.0F, m1);
            return std::pow((c1 + (c2 * y)) / (1.0F + (c3 * y)), m2);
        }

        // 线性 scRGB（709 原色，1.0 = 80 nits）→ 10-bit PQ / BT.2020。
        void scrgb_to_pq10(ScRgbSource const& source, U16Buffer& rgba10) {
            const std::size_t pixels = static_cast<std::size_t>(source.width) * source.height;
            rgba10.resize(pixels * 4);
            for (std::size_t i = 0; i < pixels; ++i) {
                const float r = source.rgba[i * 4 + 0] * 80.0F;
                const float g = source.rgba[i * 4 + 1] * 80.0F;
                const float b = source.rgba[i * 4 + 2] * 80.0F;
                // BT.709 → BT.2020
                const float r2020 = (0.6274F * r) + (0.3293F * g) + (0.0433F * b);
                const float g2020 = (0.0691F * r) + (0.9195F * g) + (0.0114F * b);
                const float b2020 = (0.0164F * r) + (0.0880F * g) + (0.8956F * b);
                rgba10[i * 4 + 0] = static_cast<std::uint16_t>(std::lround(nits_to_pq(r2020) * 1023.0F));
                rgba10[i * 4 + 1] = static_cast<std::uint16_t>(std::lround(nits_to_pq(g2020) * 1023.0F));
                rgba10[i * 4 + 2] = static_cast<std::uint16_t>(std::lround(nits_to_pq(b2020) * 1023.0F));
                rgba10[i * 4 + 3] = 1023;
            }
        }

        // 给 HDR 配一张 SDR 底图：走编码线程同一条 spline + IPT 色调映射（不是简单裁切）。
        // scRGB 源（1.0 = 80 nits）。
        Image tone_map_scrgb_to_sdr(ScRgbSource const& source, std::uint32_t sdr_white) {
            std::vector<std::uint16_t> half(source.rgba.size());
            float_to_half_row(source.rgba.data(), half.data(), source.rgba.size());
            Image image;
            if (!tone_map_hdr_to_sdr(half.data(), source.width, source.height, source.width * 8,
                                     DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, sdr_white, image)) {
                return {};
            }
            image.metadata.title = "bench-jxr";
            image.metadata.date_time = "2026:11:07 00:00:00";
            return image;
        }

        // PQ / BT.2020 的 10-bit 源 → SDR，同样走那条链路。
        Image tone_map_pq10_to_sdr(std::uint16_t const* rgba10, std::uint32_t width, std::uint32_t height,
                                   float sdr_white) {
            const std::size_t count = static_cast<std::size_t>(width) * height * 4;
            std::vector<float> floats(count);
            for (std::size_t i = 0; i < count; ++i)
                floats[i] = static_cast<float>(rgba10[i]) / 1023.0F;
            std::vector<std::uint16_t> half(count);
            float_to_half_row(floats.data(), half.data(), count);
            Image image;
            if (!tone_map_hdr_to_sdr(half.data(), width, height, width * 8,
                                     DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                                     static_cast<std::uint32_t>(sdr_white), image)) {
                return {};
            }
            image.metadata.title = "bench-hdr";
            image.metadata.date_time = "2026:11:07 00:00:00";
            return image;
        }

        // 等比例缩放后的目标尺寸（高度取给定值）。
        std::uint32_t scaled_dimension(std::uint32_t width, std::uint32_t height, std::uint32_t target_height) {
            if (target_height == 0 || target_height >= height)
                return width;
            return static_cast<std::uint32_t>(std::lround(static_cast<double>(width) * target_height / height));
        }

        // 面积平均缩放：每个目标像素取源像素块的均值（线性域，HDR/SDR 都适用）。
        template <typename T, typename Accumulate, typename Store>
        void box_downscale(T const* src, std::uint32_t src_width, std::uint32_t src_height, std::uint32_t channels,
                           T* dst, std::uint32_t dst_width, std::uint32_t dst_height, Accumulate accumulate,
                           Store store) {
            for (std::uint32_t y = 0; y < dst_height; ++y) {
                const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * src_height / dst_height);
                const std::uint32_t y1 = std::max(y0 + 1, static_cast<std::uint32_t>(
                                                              static_cast<std::uint64_t>(y + 1) * src_height / dst_height));
                for (std::uint32_t x = 0; x < dst_width; ++x) {
                    const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * src_width / dst_width);
                    const std::uint32_t x1 = std::max(x0 + 1, static_cast<std::uint32_t>(
                                                                  static_cast<std::uint64_t>(x + 1) * src_width / dst_width));
                    float sum[4]{};
                    std::uint32_t count = 0;
                    for (std::uint32_t sy = y0; sy < y1; ++sy) {
                        for (std::uint32_t sx = x0; sx < x1; ++sx) {
                            const T* pixel = src + ((static_cast<std::size_t>(sy) * src_width + sx) * channels);
                            for (std::uint32_t c = 0; c < channels; ++c)
                                sum[c] += accumulate(pixel[c]);
                            ++count;
                        }
                    }
                    T* out = dst + ((static_cast<std::size_t>(y) * dst_width + x) * channels);
                    for (std::uint32_t c = 0; c < channels; ++c)
                        out[c] = store(sum[c] / static_cast<float>(std::max(count, 1U)));
                }
            }
        }

        void downscale_scrgb(ScRgbSource& source, std::uint32_t target_height) {
            const std::uint32_t width = scaled_dimension(source.width, source.height, target_height);
            if (width == source.width)
                return;
            const std::uint32_t height = target_height;
            std::vector<float> scaled(static_cast<std::size_t>(width) * height * 4);
            box_downscale(source.rgba.data(), source.width, source.height, 4, scaled.data(), width, height, [](float v) { return v; }, [](float v) { return v; });
            source.width = width;
            source.height = height;
            source.rgba = std::move(scaled);
        }

        void downscale_bgra(Image& image, std::uint32_t target_height) {
            const std::uint32_t width = scaled_dimension(image.width, image.height, target_height);
            if (width == image.width)
                return;
            const std::uint32_t height = target_height;
            std::vector<std::uint8_t> scaled(static_cast<std::size_t>(width) * height * 4);
            box_downscale(image.pixels.data(), image.width, image.height, 4, scaled.data(), width, height, [](std::uint8_t v) { return static_cast<float>(v); }, [](float v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); });
            image.width = width;
            image.height = height;
            image.stride = width * 4;
            image.pixels.resize(scaled.size());
            std::memcpy(image.pixels.data(), scaled.data(), scaled.size());
        }

        void downscale_rgba10(U16Buffer& rgba10, std::uint32_t& width, std::uint32_t& height,
                              std::uint32_t target_height) {
            const std::uint32_t new_width = scaled_dimension(width, height, target_height);
            if (new_width == width)
                return;
            std::vector<std::uint16_t> scaled(static_cast<std::size_t>(new_width) * target_height * 4);
            box_downscale(rgba10.data(), width, height, 4, scaled.data(), new_width, target_height, [](std::uint16_t v) { return static_cast<float>(v); }, [](float v) { return static_cast<std::uint16_t>(std::clamp(std::lround(v), 0L, 65535L)); });
            rgba10.resize(scaled.size());
            std::memcpy(rgba10.data(), scaled.data(), scaled.size() * sizeof(std::uint16_t));
            width = new_width;
            height = target_height;
        }

        std::wstring lower_extension(std::filesystem::path const& path) {
            std::wstring extension = path.extension().wstring();
            std::ranges::transform(extension, extension.begin(),
                                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            return extension;
        }

        bool encode_hdr_with_config(std::filesystem::path const& path, ImageFormat format, U16Buffer const& rgba10,
                                    std::uint32_t width, std::uint32_t height, ImageMetadata const& metadata,
                                    std::string& error) {
            if (format == ImageFormat::Avif)
                return encode_avif_hdr10(path, rgba10.data(), width, height, metadata, error);
            if (format == ImageFormat::Heic)
                return encode_heic_hdr10(path, rgba10.data(), width, height, metadata, error);
            return write_hdr10(path, format, rgba10.data(), width, height, metadata, error);
        }

        // ── JXL 无损校验：把刚写出的 .jxl 解码回来，和编码输入逐像素比对 ──────
        struct JxlDecoded {
            std::vector<std::uint16_t> rgb;  // 每像素 3 通道，已还原成原始码值（8-bit 源是 0..255）
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t bits = 8;
            std::vector<std::uint8_t> narrow;  // 8-bit 源的临时缓冲
            bool use_narrow = false;
        };

        bool decode_jxl(std::filesystem::path const& path, JxlDecoded& out, std::string& error) {
            std::FILE* file = nullptr;
            if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
                error = "打不开 JXL";
                return false;
            }
            std::vector<std::uint8_t> bytes;
            std::array<std::uint8_t, 256 * 1024> chunk{};
            std::size_t read = 0;
            while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0)
                bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
            std::fclose(file);

            JxlDecoder* decoder = JxlDecoderCreate(nullptr);
            if (decoder == nullptr) {
                error = "JxlDecoderCreate 失败";
                return false;
            }
            auto guard = ScopeExit{[&] { JxlDecoderDestroy(decoder); }};
            if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
                error = "JxlDecoderSubscribeEvents 失败";
                return false;
            }
            JxlDecoderSetInput(decoder, bytes.data(), bytes.size());
            JxlDecoderCloseInput(decoder);

            bool have_info = false;
            for (;;) {
                const JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
                if (status == JXL_DEC_BASIC_INFO) {
                    JxlBasicInfo info{};
                    if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) {
                        error = "JxlDecoderGetBasicInfo 失败";
                        return false;
                    }
                    out.width = info.xsize;
                    out.height = info.ysize;
                    out.bits = info.bits_per_sample;
                    out.rgb.resize(static_cast<std::size_t>(info.xsize) * info.ysize * 3);
                    have_info = true;
                } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    if (!have_info) {
                        error = "解码顺序异常（没有 BASIC_INFO）";
                        return false;
                    }
                    // libjxl 的 UINT16 输出是“左对齐”的（8-bit 值会变成 v<<8），
                    // 所以 8-bit 源直接要 UINT8，高位深的用 UINT16 再右移回来。
                    if (out.bits <= 8) {
                        std::vector<std::uint8_t> narrow(out.rgb.size());
                        JxlPixelFormat format{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
                        if (JxlDecoderSetImageOutBuffer(decoder, &format, narrow.data(), narrow.size()) !=
                            JXL_DEC_SUCCESS) {
                            error = "JxlDecoderSetImageOutBuffer 失败";
                            return false;
                        }
                        out.narrow = std::move(narrow);
                        out.use_narrow = true;
                    } else {
                        JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
                        if (JxlDecoderSetImageOutBuffer(decoder, &format, out.rgb.data(),
                                                        out.rgb.size() * sizeof(std::uint16_t)) != JXL_DEC_SUCCESS) {
                            error = "JxlDecoderSetImageOutBuffer 失败";
                            return false;
                        }
                    }
                } else if (status == JXL_DEC_FULL_IMAGE) {
                    if (out.use_narrow) {
                        // UINT8 解码结果就是原始 8-bit 码值，直接拷进统一缓冲。
                        for (std::size_t i = 0; i < out.rgb.size(); ++i)
                            out.rgb[i] = out.narrow[i];
                        out.narrow.clear();
                        out.narrow.shrink_to_fit();
                    } else if (out.bits < 16) {
                        const auto shift = static_cast<unsigned>(16 - out.bits);
                        for (std::uint16_t& value : out.rgb)
                            value = static_cast<std::uint16_t>(value >> shift);
                    }
                    return true;
                } else if (status == JXL_DEC_SUCCESS) {
                    return have_info;
                } else if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
                    error = status == JXL_DEC_ERROR ? "JXL 解码失败" : "JXL 数据不完整";
                    return false;
                }
            }
        }

        struct PixelDiff {
            std::uint64_t pixels = 0;
            std::uint64_t differing = 0;
            std::uint32_t max_diff = 0;
        };

        // source 是编码输入（每像素 stride_channels 通道，值为原始码值），decoded 是解码结果（3 通道）。
        // order 指定源里 R/G/B 分别在第几通道（BGRA 是 {2,1,0}，RGBA 是 {0,1,2}）。
        template <typename T>
        PixelDiff compare_pixels(T const* source, std::size_t stride_channels, std::array<std::size_t, 3> order,
                                 JxlDecoded const& decoded) {
            PixelDiff diff;
            diff.pixels = static_cast<std::uint64_t>(decoded.width) * decoded.height;
            for (std::uint64_t i = 0; i < diff.pixels; ++i) {
                bool differs = false;
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto a = static_cast<std::uint16_t>(source[(i * stride_channels) + order[c]]);
                    const auto b = decoded.rgb[(i * 3) + c];
                    const std::uint32_t delta = a > b ? static_cast<std::uint32_t>(a - b)
                                                      : static_cast<std::uint32_t>(b - a);
                    diff.max_diff = std::max(diff.max_diff, delta);
                    differs = differs || delta != 0;
                }
                if (differs)
                    ++diff.differing;
            }
            return diff;
        }

    }  // namespace

    bool run_dir_bench(Config const& config, std::filesystem::path const& directory) {
        // WIC 需要 COM；基准模式在主线程上跑，这里自己初始化一次（MTA）。
        const ComApartment apartment;
        if (!apartment.ok())
            SPDLOG_WARN("COM 初始化失败，JXR 解码可能不可用: {}", describe_hresult(apartment.result()));

        SPDLOG_INFO("=== 默认配置整目录编码：{} ===", wstring_to_utf8(directory.wstring()));
        SPDLOG_INFO("SDR 格式={}，HDR 格式={}，JXL lossless={} effort={}，AVIF speed={}（编码库 {}）",
                    to_string(config.sdr_format), to_string(config.hdr_format), config.codec.jxl_lossless,
                    config.codec.jxl_effort, config.codec.avif_speed, to_string(config.codec.av1_encoder));

        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            const std::wstring extension = lower_extension(it->path());
            if (extension == L".png" || extension == L".jxr")
                files.push_back(it->path());
        }
        if (files.empty()) {
            SPDLOG_ERROR("目录里没有 .png / .jxr 文件: {}", wstring_to_utf8(directory.wstring()));
            return false;
        }
        std::ranges::sort(files);

        std::filesystem::create_directories(config.output_dir, ec);
        set_codec_options(config.codec);

        struct Row {
            std::wstring name;
            std::uint64_t source_bytes = 0;
            std::uint64_t hdr_bytes = 0;
            std::uint64_t sdr_bytes = 0;
            double ms = 0.0;
            bool hdr = false;
            bool ok = true;
            std::string error;
        };
        std::vector<Row> rows;
        rows.reserve(files.size());

        constexpr float kSdrWhiteNits = 80.0F;  // scRGB 白点；HDR10 源另有 203 nits 参考

        for (std::size_t index = 0; index < files.size(); ++index) {
            const std::filesystem::path& source_path = files[index];
            Row row;
            row.name = source_path.filename().wstring();
            row.source_bytes = std::filesystem::file_size(source_path, ec);

            U16Buffer rgba10;
            Image sdr_base;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::string error;

            const std::wstring extension = lower_extension(source_path);
            const std::uint32_t target_height = config.bench_scale_height;
            if (extension == L".jxr") {
                ScRgbSource scrgb;
                if (!load_jxr(source_path, scrgb, error)) {
                    row.ok = false;
                    row.error = error;
                    rows.push_back(std::move(row));
                    SPDLOG_ERROR("[{}/{}] {} 失败: {}", index + 1, files.size(),
                                 wstring_to_utf8(row.name), error);
                    continue;
                }
                if (target_height > 0)
                    downscale_scrgb(scrgb, target_height);
                scrgb_to_pq10(scrgb, rgba10);
                sdr_base = tone_map_scrgb_to_sdr(scrgb, static_cast<std::uint32_t>(kSdrWhiteNits));
                width = scrgb.width;
                height = scrgb.height;
                row.hdr = true;

                // 顺带报一下峰值亮度，用来确认 JXR 的 scRGB 约定（1.0 = 80 nits）没猜错。
                float peak = 0.0F;
                for (std::size_t i = 0, n = scrgb.rgba.size() / 4; i < n; ++i) {
                    peak = std::max({peak, scrgb.rgba[i * 4 + 0], scrgb.rgba[i * 4 + 1], scrgb.rgba[i * 4 + 2]});
                }
                SPDLOG_INFO("    JXR {}x{}，scRGB 峰值 {:.1f}（≈{:.0f} nits）", width, height, peak, peak * 80.0F);
            } else {
                HdrSource hdr;
                if (load_png_hdr(source_path, hdr, error)) {
                    if (target_height > 0)
                        downscale_rgba10(hdr.rgba10, hdr.width, hdr.height, target_height);
                    sdr_base = tone_map_pq10_to_sdr(hdr.rgba10.data(), hdr.width, hdr.height, 203.0F);
                    rgba10 = std::move(hdr.rgba10);
                    width = hdr.width;
                    height = hdr.height;
                    row.hdr = true;
                } else {
                    DecodedImage image8;
                    if (!load_png(source_path, image8, error)) {
                        row.ok = false;
                        row.error = error;
                        rows.push_back(std::move(row));
                        SPDLOG_ERROR("[{}/{}] {} 失败: {}", index + 1, files.size(),
                                     wstring_to_utf8(row.name), error);
                        continue;
                    }
                    sdr_base.width = image8.width;
                    sdr_base.height = image8.height;
                    sdr_base.stride = image8.width * 4;
                    sdr_base.pixels = std::move(image8.bgra);
                    sdr_base.metadata.title = "bench";
                    sdr_base.metadata.date_time = "2026:11:07 00:00:00";
                    if (target_height > 0)
                        downscale_bgra(sdr_base, target_height);
                    width = sdr_base.width;
                    height = sdr_base.height;
                    row.hdr = false;
                }
            }

            const std::wstring stem = source_path.stem().wstring();
            const auto start = SteadyClock::now();

            if (row.hdr) {
                const std::filesystem::path hdr_path =
                    config.output_dir / (stem + L"_HDR" + std::wstring(extension_of(config.hdr_format)));
                if (!encode_hdr_with_config(hdr_path, config.hdr_format, rgba10, width, height,
                                            sdr_base.metadata, error)) {
                    row.ok = false;
                    row.error = "HDR: " + error;
                } else {
                    row.hdr_bytes = std::filesystem::file_size(hdr_path, ec);
                    if (config.bench_verify && config.hdr_format == ImageFormat::Jxl && config.codec.jxl_lossless) {
                        JxlDecoded decoded;
                        std::string decode_error;
                        if (!decode_jxl(hdr_path, decoded, decode_error)) {
                            row.ok = false;
                            row.error = "HDR 校验解码失败: " + decode_error;
                        } else {
                            const PixelDiff diff = compare_pixels(rgba10.data(), 4, {0, 1, 2}, decoded);
                            SPDLOG_INFO("    无损校验 HDR: {} 通道 {} bit，像素 {}，差异 {}，最大差 {}",
                                        "RGB", decoded.bits, diff.pixels, diff.differing, diff.max_diff);
                            if (diff.differing != 0) {
                                row.ok = false;
                                row.error = fmt::format("HDR 不是无损：{} 个像素有差异", diff.differing);
                            }
                        }
                    }
                }
            }

            const std::filesystem::path sdr_path =
                config.output_dir / (stem + (row.hdr ? L"_SDR" : L"") + std::wstring(extension_of(config.sdr_format)));
            if (row.ok && !write_sdr(sdr_path, config.sdr_format, sdr_base, error)) {
                row.ok = false;
                row.error = "SDR: " + error;
            } else if (row.ok) {
                row.sdr_bytes = std::filesystem::file_size(sdr_path, ec);
                if (config.bench_verify && config.sdr_format == ImageFormat::Jxl && config.codec.jxl_lossless) {
                    JxlDecoded decoded;
                    std::string decode_error;
                    if (!decode_jxl(sdr_path, decoded, decode_error)) {
                        row.ok = false;
                        row.error = "SDR 校验解码失败: " + decode_error;
                    } else {
                        // sdr_base 是 BGRA；JXL 里存的是紧凑 RGB，所以顺序是 {2,1,0}。
                        const PixelDiff diff = compare_pixels(sdr_base.pixels.data(), 4, {2, 1, 0}, decoded);
                        SPDLOG_INFO("    无损校验 SDR: RGB {} bit，像素 {}，差异 {}，最大差 {}", decoded.bits,
                                    diff.pixels, diff.differing, diff.max_diff);
                        if (diff.differing != 0) {
                            row.ok = false;
                            row.error = fmt::format("SDR 不是无损：{} 个像素有差异", diff.differing);
                        }
                    }
                }
            }

            row.ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
            const std::uint64_t total = row.hdr_bytes + row.sdr_bytes;
            const double ratio =
                total > 0 ? static_cast<double>(row.source_bytes) / static_cast<double>(total) : 0.0;
            SPDLOG_INFO("[{}/{}] {}{:<44} 源 {:>10} -> HDR {:>9} + SDR {:>9} = {:>10}  {:5.2f}x  {:7.1f} ms{}",
                        index + 1, files.size(), row.ok ? "" : "[失败] ",
                        wstring_to_utf8(row.name), row.source_bytes, row.hdr_bytes, row.sdr_bytes, total, ratio,
                        row.ms, row.error.empty() ? "" : ("  " + row.error));
            rows.push_back(std::move(row));
        }

        // ── 汇总 ────────────────────────────────────────────────────────────
        std::uint64_t source_total = 0;
        std::uint64_t hdr_total = 0;
        std::uint64_t sdr_total = 0;
        std::size_t ok_count = 0;
        double ms_total = 0.0;
        for (const Row& row : rows) {
            if (!row.ok)
                continue;
            ++ok_count;
            source_total += row.source_bytes;
            hdr_total += row.hdr_bytes;
            sdr_total += row.sdr_bytes;
            ms_total += row.ms;
        }
        const std::uint64_t output_total = hdr_total + sdr_total;
        if (ok_count == 0) {
            SPDLOG_ERROR("全部失败，没有可汇总的数据");
            return false;
        }

        SPDLOG_INFO("──────────────────────────────────────────────────────────────");
        SPDLOG_INFO("成功 {} / {} 张", ok_count, rows.size());
        SPDLOG_INFO("源总计 {:.2f} MB（{} 字节）", static_cast<double>(source_total) / (1024.0 * 1024.0),
                    source_total);
        SPDLOG_INFO("输出总计 {:.2f} MB（HDR {:.2f} MB + SDR {:.2f} MB）",
                    static_cast<double>(output_total) / (1024.0 * 1024.0),
                    static_cast<double>(hdr_total) / (1024.0 * 1024.0),
                    static_cast<double>(sdr_total) / (1024.0 * 1024.0));
        SPDLOG_INFO("总体压缩率 {:.2f}x，体积为 {:.1f}%，平均 {:.0f} ms/张",
                    static_cast<double>(source_total) / static_cast<double>(output_total),
                    100.0 * static_cast<double>(output_total) / static_cast<double>(source_total),
                    ms_total / static_cast<double>(ok_count));

        const std::filesystem::path csv_path = config.output_dir / L"bench_dir.csv";
        std::ofstream csv(csv_path, std::ios::binary | std::ios::trunc);
        if (csv) {
            csv << "name,source_bytes,hdr_bytes,sdr_bytes,total_bytes,ratio,ms,hdr,ok,error\n";
            for (const Row& row : rows) {
                const std::uint64_t total = row.hdr_bytes + row.sdr_bytes;
                const double ratio =
                    total > 0 ? static_cast<double>(row.source_bytes) / static_cast<double>(total) : 0.0;
                csv << '"' << wstring_to_utf8(row.name) << "\"," << row.source_bytes << ',' << row.hdr_bytes << ','
                    << row.sdr_bytes << ',' << total << ',' << ratio << ',' << row.ms << ',' << (row.hdr ? 1 : 0)
                    << ',' << (row.ok ? 1 : 0) << ',' << '"' << row.error << "\"\n";
            }
            csv << "TOTAL," << source_total << ',' << hdr_total << ',' << sdr_total << ',' << output_total << ','
                << (static_cast<double>(source_total) / static_cast<double>(output_total)) << ',' << ms_total << ",,,\n";
            SPDLOG_INFO("CSV: {}", wstring_to_utf8(csv_path.wstring()));
        }
        return ok_count == rows.size();
    }

}  // namespace gamesnap

#endif  // GAMESNAP_BENCH
