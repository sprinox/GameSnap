#include "pch.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>

#include <avif/avif.h>
#include <zlib.h>
#include <jpeglib.h>
#include <libheif/heif.h>
#include <spng.h>
#include <webp/encode.h>
#include <x265.h>

#pragma comment(lib, "x265-static.lib")
#include <webp/mux.h>

#include "codecs.h"
#include "common.h"
#include "simd.h"

#pragma comment(lib, "libwebpmux.lib")

namespace gamesnap {

    namespace {

        CodecOptions codec_options;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once before encoder threads start

        constexpr std::uint32_t kMaxChannel10 = 1023;

        std::string_view format_name(ImageFormat format) {
            switch (format) {
            case ImageFormat::Bmp:
                return "bmp";
            case ImageFormat::Png:
                return "png";
            case ImageFormat::Jpeg:
                return "jpeg";
            case ImageFormat::Webp:
                return "webp";
            case ImageFormat::Avif:
                return "avif";
            case ImageFormat::Heic:
                return "heic";
            }
            return "unknown";
        }

        bool write_bytes(std::filesystem::path const& path, void const* data, std::size_t size, std::string& error) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "无法创建文件";
                return false;
            }
            file.write(static_cast<char const*>(data), static_cast<std::streamsize>(size));
            file.close();
            if (file.fail()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
                error = "写文件失败";
                return false;
            }
            return true;
        }

        bool to_rgba(Image const& image, ByteBuffer& rgba) {
            if (image.width == 0 || image.height == 0 || image.stride != image.width * 4)
                return false;
            rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
            for (std::uint32_t y = 0; y < image.height; ++y) {
                auto const* src = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
                auto* dst = rgba.data() + static_cast<std::size_t>(y) * image.width * 4;
                bgra_to_rgba_row(src, dst, image.width);
            }
            return true;
        }

        // ── PQ ───────────────────────────────────────────────────────────────
        float pq_to_nits(float pq) {
            if (!(pq > 0.0F))
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

        float nits_to_pq(float nits) {
            constexpr float m1 = 2610.0F / 16384.0F;
            constexpr float m2 = 2523.0F / 4096.0F * 128.0F;
            constexpr float c1 = 3424.0F / 4096.0F;
            constexpr float c2 = 2413.0F / 4096.0F * 32.0F;
            constexpr float c3 = 2392.0F / 4096.0F * 32.0F;
            const float y = std::pow(std::clamp(nits / 10000.0F, 0.0F, 1.0F), m1);
            const float num = c1 + (c2 * y);
            const float den = 1.0F + (c3 * y);
            return std::pow(num / den, m2);
        }

        struct RgbLinear {
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
        };

        RgbLinear srgb_to_bt2020(RgbLinear c) {
            return {.r = 0.6274F * c.r + 0.3293F * c.g + 0.0433F * c.b,
                    .g = 0.0691F * c.r + 0.9195F * c.g + 0.0114F * c.b,
                    .b = 0.0164F * c.r + 0.0880F * c.g + 0.8956F * c.b};
        }

        // ── EXIF（仅 little-endian，Windows x64）─────────────────────────────
        static_assert(std::endian::native == std::endian::little,
                      "EXIF writer relies on little-endian host layout");

        void write_u16_be(std::uint8_t* out, std::uint16_t value) {
            const std::uint16_t encoded = std::byteswap(value);
            std::memcpy(out, &encoded, sizeof(encoded));
        }

        void write_u32_be(std::uint8_t* out, std::uint32_t value) {
            const std::uint32_t encoded = std::byteswap(value);
            std::memcpy(out, &encoded, sizeof(encoded));
        }

#pragma pack(push, 1)
        struct TiffHeader {
            std::uint16_t byte_order = 0x4949;  // "II"
            std::uint16_t magic = 42;
            std::uint32_t ifd0_offset = 8;
        };

        struct ExifEntry {
            std::uint16_t tag = 0;
            std::uint16_t type = 0;
            std::uint32_t count = 0;
            std::uint32_t value = 0;
        };
#pragma pack(pop)
        static_assert(sizeof(TiffHeader) == 8);
        static_assert(sizeof(ExifEntry) == 12);

        std::string exif_text(std::string value, std::size_t limit) {
            std::erase(value, '\0');
            if (value.size() > limit)
                value.resize(limit);
            return value;
        }

        // 最小 TIFF/EXIF：ImageDescription=标题，DateTimeOriginal/Digitized=截图时间。
        // 直接按 little-endian 结构体写入，不做逐字节 append/字节序转换。
        // PNG eXIf 和 AVIF 使用纯 TIFF；JPEG/WebP/HEIC 使用 "Exif\0\0"+TIFF。
        std::vector<std::uint8_t> make_exif(ImageMetadata const& metadata, bool with_prefix) {
            const std::string title = exif_text(metadata.title.empty() ? "GameSnap" : metadata.title, 512);
            std::string date_time = metadata.date_time;
            if (date_time.size() != 19)
                date_time = "0000:00:00 00:00:00";

            constexpr std::uint32_t ifd0_offset = 8;
            constexpr std::uint32_t ifd0_count = 3;
            constexpr std::uint32_t ifd0_size = 2 + ifd0_count * 12 + 4;
            constexpr std::uint32_t strings_offset = ifd0_offset + ifd0_size;
            const std::uint32_t title_offset = strings_offset;
            const std::uint32_t software_offset = title_offset + static_cast<std::uint32_t>(title.size()) + 1;
            constexpr std::uint32_t software_text_size = 9;  // "GameSnap\0"
            const std::uint32_t exif_ifd_offset = (software_offset + software_text_size + 1U) & ~1U;
            constexpr std::uint32_t exif_ifd_count = 2;
            constexpr std::uint32_t exif_ifd_size = 2 + exif_ifd_count * 12 + 4;
            const std::uint32_t date_offset = exif_ifd_offset + exif_ifd_size;

            const std::size_t prefix_size = with_prefix ? 6 : 0;
            std::vector<std::uint8_t> tiff(prefix_size + date_offset + 20);
            auto* base = tiff.data() + prefix_size;
            if (with_prefix)
                std::memcpy(tiff.data(), "Exif\0\0", 6);
            const TiffHeader header{};
            std::memcpy(base, &header, sizeof(header));

            const std::array<ExifEntry, 3> ifd0_entries{{
                {.tag = 0x010E, .type = 2,
                 .count = static_cast<std::uint32_t>(title.size()) + 1, .value = title_offset},
                {.tag = 0x0131, .type = 2, .count = software_text_size, .value = software_offset},
                {.tag = 0x8769, .type = 4, .count = 1, .value = exif_ifd_offset},
            }};
            const auto ifd0_next = static_cast<std::uint32_t>(0);
            auto* ifd0 = base + ifd0_offset;
            std::memcpy(ifd0, &ifd0_count, sizeof(ifd0_count));
            std::memcpy(ifd0 + 2, ifd0_entries.data(), sizeof(ifd0_entries));
            std::memcpy(ifd0 + 2 + sizeof(ifd0_entries), &ifd0_next, sizeof(ifd0_next));

            std::memcpy(base + title_offset, title.data(), title.size() + 1);
            constexpr std::string_view software = "GameSnap";
            std::memcpy(base + software_offset, software.data(), software.size() + 1);

            const std::array<ExifEntry, 2> exif_entries{{
                {.tag = 0x9003, .type = 2, .count = 20, .value = date_offset},
                {.tag = 0x9004, .type = 2, .count = 20, .value = date_offset},
            }};
            const auto exif_next = static_cast<std::uint32_t>(0);
            auto* exif_ifd = base + exif_ifd_offset;
            std::memcpy(exif_ifd, &exif_ifd_count, sizeof(exif_ifd_count));
            std::memcpy(exif_ifd + 2, exif_entries.data(), sizeof(exif_entries));
            std::memcpy(exif_ifd + 2 + sizeof(exif_entries), &exif_next, sizeof(exif_next));
            std::memcpy(base + date_offset, date_time.data(), 20);
            return tiff;
        }

        std::vector<std::uint8_t> make_exif_tiff(ImageMetadata const& metadata) {
            return make_exif(metadata, false);
        }

        std::vector<std::uint8_t> make_exif_payload(ImageMetadata const& metadata) {
            return make_exif(metadata, true);
        }

        std::vector<std::uint8_t> add_jpeg_exif(std::vector<std::uint8_t> jpeg,
                                                ImageMetadata const& metadata) {
            const auto exif = make_exif_payload(metadata);
            if (jpeg.size() < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
                return jpeg;
            std::vector<std::uint8_t> out(6 + exif.size() + jpeg.size() - 2);
            auto* p = out.data();
            constexpr std::array<std::uint8_t, 4> app1_marker{0xFF, 0xD8, 0xFF, 0xE1};
            std::memcpy(p, app1_marker.data(), app1_marker.size());
            write_u16_be(p + 4, static_cast<std::uint16_t>(exif.size() + 2));
            std::memcpy(p + 6, exif.data(), exif.size());
            std::memcpy(p + 6 + exif.size(), jpeg.data() + 2, jpeg.size() - 2);
            return out;
        }

        std::vector<std::uint8_t> add_png_exif(std::vector<std::uint8_t> png,
                                               ImageMetadata const& metadata) {
            if (png.size() < 12)
                return png;
            const auto tiff = make_exif_tiff(metadata);
            std::vector<std::uint8_t> chunk(12 + tiff.size());
            auto* chunk_data = chunk.data();
            write_u32_be(chunk_data, static_cast<std::uint32_t>(tiff.size()));
            constexpr std::array<std::uint8_t, 4> exif_type{'e', 'X', 'I', 'f'};
            std::memcpy(chunk_data + 4, exif_type.data(), exif_type.size());
            std::memcpy(chunk_data + 8, tiff.data(), tiff.size());
            uLong crc = ::crc32(0L, Z_NULL, 0);
            crc = ::crc32(crc, reinterpret_cast<const Bytef*>(chunk_data + 4),
                          static_cast<uInt>(4 + tiff.size()));
            write_u32_be(chunk_data + 8 + tiff.size(), static_cast<std::uint32_t>(crc));

            // IEND 固定为最后 12 字节：length=0 + "IEND" + crc。
            const std::size_t insert_at = png.size() - 12;
            png.insert(png.begin() + static_cast<std::ptrdiff_t>(insert_at), chunk.begin(), chunk.end());
            return png;
        }

        std::vector<std::uint8_t> add_webp_exif(std::vector<std::uint8_t> webp,
                                                ImageMetadata const& metadata) {
            if (webp.size() < 12 || std::memcmp(webp.data(), "RIFF", 4) != 0 ||
                std::memcmp(webp.data() + 8, "WEBP", 4) != 0) {
                return webp;
            }

            const auto exif = make_exif_payload(metadata);
            WebPData image_data{.bytes = webp.data(), .size = webp.size()};
            WebPData exif_data{.bytes = exif.data(), .size = exif.size()};
            WebPMux* mux = WebPMuxNew();
            if (mux == nullptr)
                return webp;
            if (WebPMuxSetImage(mux, &image_data, 1) != WEBP_MUX_OK ||
                WebPMuxSetChunk(mux, "EXIF", &exif_data, 1) != WEBP_MUX_OK) {
                WebPMuxDelete(mux);
                return webp;
            }

            WebPData assembled{};
            if (WebPMuxAssemble(mux, &assembled) != WEBP_MUX_OK) {
                WebPMuxDelete(mux);
                return webp;
            }
            std::vector<std::uint8_t> out(assembled.bytes, assembled.bytes + assembled.size);
            WebPDataClear(&assembled);
            WebPMuxDelete(mux);
            return out;
        }

        // ── BMP ──────────────────────────────────────────────────────────────
        bool write_bmp_impl(std::filesystem::path const& path, Image const& image, std::string& error) {
            if (image.pixels.empty() || image.width == 0 || image.height == 0) {
                error = "空图像";
                return false;
            }
            const std::uint32_t row_bytes = image.width * 4;
            BITMAPFILEHEADER file_header{};
            file_header.bfType = 0x4D42;
            file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            file_header.bfSize = file_header.bfOffBits + (row_bytes * image.height);

            BITMAPINFOHEADER info_header{};
            info_header.biSize = sizeof(BITMAPINFOHEADER);
            info_header.biWidth = static_cast<LONG>(image.width);
            info_header.biHeight = -static_cast<LONG>(image.height);
            info_header.biPlanes = 1;
            info_header.biBitCount = 32;
            info_header.biCompression = BI_RGB;
            info_header.biSizeImage = row_bytes * image.height;

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "无法创建文件";
                return false;
            }
            file.write(reinterpret_cast<char const*>(&file_header), sizeof(file_header));
            file.write(reinterpret_cast<char const*>(&info_header), sizeof(info_header));
            file.write(reinterpret_cast<char const*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
            file.flush();
            if (!file.good()) {
                error = "写 BMP 失败";
                return false;
            }
            file.close();
            if (file.fail()) {
                error = "关闭 BMP 失败";
                return false;
            }
            return true;
        }

        // ── PNG（libspng，最高压缩） ─────────────────────────────────────────
        bool write_png_impl(std::filesystem::path const& path, Image const& image, std::string& error) {
            // PNG 转换缓冲只在本次编码期间存在；空闲时不占内存。
            ByteBuffer rgba;
            if (!to_rgba(image, rgba)) {
                error = "PNG 需要 BGRA8 紧凑图像";
                return false;
            }
            spng_ctx* ctx = spng_ctx_new(SPNG_CTX_ENCODER);
            if (ctx == nullptr) {
                error = "spng_ctx_new 失败";
                return false;
            }
            spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1);
            spng_set_option(ctx, SPNG_IMG_COMPRESSION_LEVEL, codec_options.png_compression);

            spng_ihdr ihdr{};
            ihdr.width = image.width;
            ihdr.height = image.height;
            ihdr.bit_depth = 8;
            ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
            ihdr.compression_method = 0;
            ihdr.filter_method = 0;
            ihdr.interlace_method = 0;
            if (spng_set_ihdr(ctx, &ihdr) != 0) {
                spng_ctx_free(ctx);
                error = "spng_set_ihdr 失败";
                return false;
            }
            const int encode_result =
                spng_encode_image(ctx, rgba.data(), rgba.size(), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
            if (encode_result != 0) {
                error = fmt::format("spng_encode_image 失败: {} ({})", spng_strerror(encode_result), encode_result);
                spng_ctx_free(ctx);
                return false;
            }
            size_t png_size = 0;
            int png_error = 0;
            void* png = spng_get_png_buffer(ctx, &png_size, &png_error);
            if (png == nullptr) {
                spng_ctx_free(ctx);
                error = "spng_get_png_buffer 失败";
                return false;
            }
            const std::vector<std::uint8_t> png_bytes(static_cast<std::uint8_t*>(png),
                                                       static_cast<std::uint8_t*>(png) + png_size);
            const auto with_exif = add_png_exif(png_bytes, image.metadata);
            const bool ok = write_bytes(path, with_exif.data(), with_exif.size(), error);
            free(png);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
            spng_ctx_free(ctx);
            return ok;
        }

        // ── JPEG（需要 RGB 扫描线） ─────────────────────────────────────────
        struct JpegErrorMgr {
            jpeg_error_mgr base{};
            jmp_buf jump{};
        };

        void jpeg_error_exit(j_common_ptr cinfo) {
            longjmp(reinterpret_cast<JpegErrorMgr*>(cinfo->err)->jump, 1);  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        }

        // libjpeg 用 longjmp 报错：longjmp 之后普通自动变量的值不确定（C11 7.13.2.1），
        // 因此所有跨越 setjmp/longjmp 仍然要使用的状态都放在线程局部对象里。
        struct JpegScratch {
            jpeg_compress_struct cinfo{};
            JpegErrorMgr error{};
            unsigned char* mem = nullptr;
            unsigned long mem_size = 0;
            bool created = false;  // 跨 setjmp/longjmp，必须放在 thread_local 里
            std::vector<std::uint8_t> row;
        };

        JpegScratch& jpeg_scratch() {
            static thread_local JpegScratch scratch;
            return scratch;
        }

        bool write_jpeg_impl(std::filesystem::path const& path, Image const& image, std::string& error) {
            if (image.width == 0 || image.height == 0) {
                error = "空图像";
                return false;
            }
            JpegScratch& scratch = jpeg_scratch();
            scratch.cinfo = {};
            scratch.error = {};
            scratch.mem = nullptr;
            scratch.mem_size = 0;
            scratch.created = false;
            scratch.cinfo.err = jpeg_std_error(&scratch.error.base);
            scratch.error.base.error_exit = jpeg_error_exit;
            scratch.row.resize(static_cast<std::size_t>(image.width) * 3);

            if (setjmp(scratch.error.jump) != 0) {  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                if (scratch.created) {
                    jpeg_destroy_compress(&scratch.cinfo);
                    scratch.created = false;
                }
                free(scratch.mem);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
                scratch.mem = nullptr;
                error = "JPEG 编码失败";
                return false;
            }

            jpeg_create_compress(&scratch.cinfo);
            scratch.created = true;
            jpeg_mem_dest(&scratch.cinfo, &scratch.mem, &scratch.mem_size);

            scratch.cinfo.image_width = image.width;
            scratch.cinfo.image_height = image.height;
            scratch.cinfo.input_components = 3;
            scratch.cinfo.in_color_space = JCS_RGB;
            jpeg_set_defaults(&scratch.cinfo);
            jpeg_set_quality(&scratch.cinfo, codec_options.jpeg_quality, TRUE);
            for (int i = 0; i < scratch.cinfo.num_components; ++i) {
                scratch.cinfo.comp_info[i].h_samp_factor = 1;
                scratch.cinfo.comp_info[i].v_samp_factor = 1;
            }
            jpeg_start_compress(&scratch.cinfo, TRUE);

            while (scratch.cinfo.next_scanline < scratch.cinfo.image_height) {
                auto const* src =
                    image.pixels.data() + static_cast<std::size_t>(scratch.cinfo.next_scanline) * image.stride;
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    scratch.row[x * 3 + 0] = src[x * 4 + 2];
                    scratch.row[x * 3 + 1] = src[x * 4 + 1];
                    scratch.row[x * 3 + 2] = src[x * 4 + 0];
                }
                JSAMPROW row_pointer = scratch.row.data();
                jpeg_write_scanlines(&scratch.cinfo, &row_pointer, 1);
            }
            jpeg_finish_compress(&scratch.cinfo);
            const std::vector<std::uint8_t> jpeg(scratch.mem, scratch.mem + scratch.mem_size);
            const auto with_exif = add_jpeg_exif(jpeg, image.metadata);
            const bool ok = write_bytes(path, with_exif.data(), with_exif.size(), error);
            jpeg_destroy_compress(&scratch.cinfo);
            scratch.created = false;
            free(scratch.mem);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
            scratch.mem = nullptr;
            return ok;
        }

        // ── WebP（无损、method 4） ──────────────────────────────────────────
        bool write_webp_impl(std::filesystem::path const& path, Image const& image, std::string& error) {
            WebPConfig config;
            if (WebPConfigInit(&config) == 0) {
                error = "WebPConfigInit 失败";
                return false;
            }
            config.lossless = codec_options.webp_lossless ? 1 : 0;
            config.quality = codec_options.webp_lossless ? 100.0F
                                                         : static_cast<float>(codec_options.webp_quality);
            config.method = codec_options.webp_method;
            config.exact = 1;

            WebPPicture picture;
            if (WebPPictureInit(&picture) == 0) {
                error = "WebPPictureInit 失败";
                return false;
            }
            picture.width = static_cast<int>(image.width);
            picture.height = static_cast<int>(image.height);
            picture.use_argb = 1;
            if (WebPPictureImportBGRA(&picture, image.pixels.data(), static_cast<int>(image.stride)) == 0) {
                WebPPictureFree(&picture);
                error = "WebPPictureImportBGRA 失败";
                return false;
            }

            WebPMemoryWriter writer;
            WebPMemoryWriterInit(&writer);
            picture.writer = WebPMemoryWrite;
            picture.custom_ptr = &writer;
            const int ok = WebPEncode(&config, &picture);
            bool result = false;
            if (ok != 0) {
                const std::vector<std::uint8_t> webp(writer.mem, writer.mem + writer.size);
                const auto with_exif = add_webp_exif(webp, image.metadata);
                result = write_bytes(path, with_exif.data(), with_exif.size(), error);
            }

            WebPMemoryWriterClear(&writer);
            WebPPictureFree(&picture);
            if (ok == 0)
                error = "WebPEncode 失败";
            return result;
        }

        // ── libavif ─────────────────────────────────────────────────────────
        bool write_avif_impl(std::filesystem::path const& path, avifImage* image,
                             ImageMetadata const& metadata, std::string& error) {
            const auto exif = make_exif_payload(metadata);
            if (!exif.empty()) {
                const avifResult exif_result =
                    avifImageSetMetadataExif(image, exif.data(), exif.size());
                if (exif_result != AVIF_RESULT_OK) {
                    error = fmt::format("avifImageSetMetadataExif 失败: {}", avifResultToString(exif_result));
                    return false;
                }
            }

            avifEncoder* encoder = avifEncoderCreate();
            if (encoder == nullptr) {
                error = "avifEncoderCreate 失败";
                return false;
            }
            const int avif_speed = std::clamp(codec_options.avif_speed, 0, 10);
            const int avif_quality = codec_options.avif_lossless ? 100 : codec_options.avif_quality;
            encoder->speed = avif_speed;
            encoder->maxThreads = 1;  // 并行度由 encode_threads 提供，避免 N×8 超订
            encoder->quality = avif_quality;
            encoder->qualityAlpha = avif_quality;
            encoder->qualityGainMap = avif_quality;
            if (codec_options.avif_lossless) {
                encoder->minQuantizer = AVIF_QUANTIZER_LOSSLESS;
                encoder->maxQuantizer = AVIF_QUANTIZER_LOSSLESS;
                encoder->minQuantizerAlpha = AVIF_QUANTIZER_LOSSLESS;
                encoder->maxQuantizerAlpha = AVIF_QUANTIZER_LOSSLESS;
                (void)avifEncoderSetCodecSpecificOption(encoder, "lossless", "1");
            }

            avifRWData output = AVIF_DATA_EMPTY;
            const avifResult result = avifEncoderWrite(encoder, image, &output);
            bool ok = false;
            if (result == AVIF_RESULT_OK) {
                ok = write_bytes(path, output.data, output.size, error);
            } else {
                error = fmt::format("AVIF 编码失败: {}", avifResultToString(result));
            }
            avifRWDataFree(&output);
            avifEncoderDestroy(encoder);
            return ok;
        }

        bool avif_from_bgra(std::filesystem::path const& path, Image const& image, bool hdr, std::string& error) {
            const std::uint32_t depth = hdr ? 10 : 8;
            avifImage* avif = avifImageCreate(image.width, image.height, depth, AVIF_PIXEL_FORMAT_YUV444);
            if (avif == nullptr) {
                error = "avifImageCreate 失败";
                return false;
            }
            avif->yuvRange = AVIF_RANGE_FULL;
            avif->colorPrimaries = hdr ? AVIF_COLOR_PRIMARIES_BT2020 : AVIF_COLOR_PRIMARIES_BT709;
            avif->transferCharacteristics = hdr ? AVIF_TRANSFER_CHARACTERISTICS_PQ : AVIF_TRANSFER_CHARACTERISTICS_SRGB;
            avif->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;

            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, avif);
            rgb.format = AVIF_RGB_FORMAT_BGRA;
            rgb.depth = depth;
            rgb.rowBytes = image.stride;
            rgb.pixels = const_cast<std::uint8_t*>(image.pixels.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)

            bool ok = false;
            if (avifImageRGBToYUV(avif, &rgb) == AVIF_RESULT_OK)
                ok = write_avif_impl(path, avif, image.metadata, error);
            else
                error = "avifImageRGBToYUV 失败";
            avifImageDestroy(avif);
            return ok;
        }

        // HDR FP16 → 10-bit BT.2020 PQ RGBA 缓冲。
        U16Buffer hdr_to_rgba10(HdrFrame const& frame) {
            U16Buffer rgba;
            rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
            std::vector<float> row_floats(static_cast<std::size_t>(frame.width) * 4);
            for (std::uint32_t y = 0; y < frame.height; ++y) {
                auto const* row16 =
                    reinterpret_cast<std::uint16_t const*>(frame.bytes + static_cast<std::size_t>(y) * frame.stride);
                half_to_float_row(row16, row_floats.data(), static_cast<std::size_t>(frame.width) * 4);
                for (std::uint32_t x = 0; x < frame.width; ++x) {
                    RgbLinear rgb{.r = row_floats[static_cast<std::size_t>(x) * 4 + 0],
                                  .g = row_floats[static_cast<std::size_t>(x) * 4 + 1],
                                  .b = row_floats[static_cast<std::size_t>(x) * 4 + 2]};
                    auto* p = rgba.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4;
                    if (frame.pq) {
                        // 输入本身就是 PQ 码值，直接量化，避免 nits→PQ 的恒等往返。
                        const auto quantize = [](float value) {
                            return static_cast<std::uint16_t>(
                                std::lround(std::clamp(value, 0.0F, 1.0F) * kMaxChannel10));
                        };
                        p[0] = quantize(rgb.r);
                        p[1] = quantize(rgb.g);
                        p[2] = quantize(rgb.b);
                    } else {
                        const RgbLinear bt2020 = srgb_to_bt2020(
                            {.r = rgb.r * 80.0F, .g = rgb.g * 80.0F, .b = rgb.b * 80.0F});
                        p[0] = static_cast<std::uint16_t>(std::lround(nits_to_pq(bt2020.r) * kMaxChannel10));
                        p[1] = static_cast<std::uint16_t>(std::lround(nits_to_pq(bt2020.g) * kMaxChannel10));
                        p[2] = static_cast<std::uint16_t>(std::lround(nits_to_pq(bt2020.b) * kMaxChannel10));
                    }
                    p[3] = kMaxChannel10;
                }
            }
            return rgba;
        }

        bool avif_from_rgba10(std::filesystem::path const& path, std::uint16_t const* rgba,
                              std::uint32_t width, std::uint32_t height,
                              ImageMetadata const& metadata, std::string& error) {
            avifImage* avif = avifImageCreate(width, height, 10, AVIF_PIXEL_FORMAT_YUV444);
            if (avif == nullptr) {
                error = "avifImageCreate 失败";
                return false;
            }
            avif->yuvRange = AVIF_RANGE_FULL;
            avif->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
            avif->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_PQ;
            avif->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;

            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, avif);
            rgb.format = AVIF_RGB_FORMAT_RGBA;
            rgb.depth = 10;
            rgb.rowBytes = width * 8;
            rgb.pixels = const_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t const*>(rgba));  // NOLINT(cppcoreguidelines-pro-type-const-cast)

            bool ok = false;
            if (avifImageRGBToYUV(avif, &rgb) == AVIF_RESULT_OK)
                ok = write_avif_impl(path, avif, metadata, error);
            else
                error = "avifImageRGBToYUV 失败";
            avifImageDestroy(avif);
            return ok;
        }

        bool avif_gainmap(std::filesystem::path const& path, Image const& sdr, std::uint16_t const* hdr10,
                          std::uint32_t width, std::uint32_t height, std::string& error) {
            if (sdr.width != width || sdr.height != height || sdr.stride != width * 4) {
                error = "gain map 的 SDR/HDR 尺寸不一致";
                return false;
            }

            avifImage* base = avifImageCreate(width, height, 8, AVIF_PIXEL_FORMAT_YUV444);
            if (base == nullptr) {
                error = "avifImageCreate(base) 失败";
                return false;
            }
            base->yuvRange = AVIF_RANGE_FULL;
            base->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
            base->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
            base->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;

            avifRGBImage base_rgb{};
            base_rgb.width = width;
            base_rgb.height = height;
            base_rgb.depth = 8;
            base_rgb.format = AVIF_RGB_FORMAT_BGRA;
            base_rgb.rowBytes = sdr.stride;
            base_rgb.pixels = const_cast<std::uint8_t*>(sdr.pixels.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
            if (avifImageRGBToYUV(base, &base_rgb) != AVIF_RESULT_OK) {
                avifImageDestroy(base);
                error = "base avifImageRGBToYUV 失败";
                return false;
            }

            avifRGBImage alt_rgb{};
            alt_rgb.width = width;
            alt_rgb.height = height;
            alt_rgb.depth = 10;
            alt_rgb.format = AVIF_RGB_FORMAT_RGBA;
            alt_rgb.rowBytes = width * 8;
            alt_rgb.pixels = const_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t const*>(hdr10));  // NOLINT(cppcoreguidelines-pro-type-const-cast)

            avifGainMap* gain_map = avifGainMapCreate();
            if (gain_map == nullptr) {
                avifImageDestroy(base);
                error = "avifGainMapCreate 失败";
                return false;
            }
            gain_map->image = avifImageCreate(width, height, 8, AVIF_PIXEL_FORMAT_YUV444);
            if (gain_map->image == nullptr) {
                avifGainMapDestroy(gain_map);
                avifImageDestroy(base);
                error = "avifImageCreate(gain map) 失败";
                return false;
            }
            gain_map->image->yuvRange = AVIF_RANGE_FULL;

            avifDiagnostics diag{};
            const avifResult computed = avifRGBImageComputeGainMap(
                &base_rgb, AVIF_COLOR_PRIMARIES_BT709, AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                &alt_rgb, AVIF_COLOR_PRIMARIES_BT2020, AVIF_TRANSFER_CHARACTERISTICS_PQ,
                gain_map, &diag);
            if (computed != AVIF_RESULT_OK) {
                error = fmt::format("avifRGBImageComputeGainMap 失败: {}", avifResultToString(computed));
                avifGainMapDestroy(gain_map);
                avifImageDestroy(base);
                return false;
            }
            gain_map->altColorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
            gain_map->altTransferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_PQ;
            gain_map->altMatrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
            gain_map->altDepth = 10;
            gain_map->altPlaneCount = 3;

            base->gainMap = gain_map;
            const bool ok = write_avif_impl(path, base, sdr.metadata, error);
            base->gainMap = nullptr;
            avifGainMapDestroy(gain_map);
            avifImageDestroy(base);
            return ok;
        }

        void free_heif(heif_context* ctx, heif_image* image, heif_encoder* encoder) {
            if (image)
                heif_image_release(image);
            if (encoder)
                heif_encoder_release(encoder);
            if (ctx)
                heif_context_free(ctx);
        }

        bool write_heic_impl(std::filesystem::path const& path, heif_context* ctx, heif_image* image,
                             heif_encoder* encoder, ImageMetadata const& metadata, std::string& error) {
            heif_image_handle* handle = nullptr;
            const heif_error encoded = heif_context_encode_image(ctx, image, encoder, nullptr, &handle);
            if (encoded.code != heif_error_Ok) {
                error = fmt::format("HEIC 编码失败: {}", encoded.message ? encoded.message : "unknown");
                if (handle != nullptr)
                    heif_image_handle_release(handle);
                return false;
            }
            if (handle != nullptr) {
                const auto exif = make_exif_payload(metadata);
                const heif_error add_exif = heif_context_add_exif_metadata(
                    ctx, handle, exif.data(), static_cast<int>(exif.size()));
                if (add_exif.code != heif_error_Ok)
                    SPDLOG_WARN("HEIC 写入 EXIF 失败: {}", add_exif.message ? add_exif.message : "unknown");
                heif_image_handle_release(handle);
            }
            struct MemoryWriter {
                std::vector<std::uint8_t> bytes;
            };
            heif_writer writer{
                .writer_api_version = 1,
                .write = [](heif_context*, void const* data, std::size_t size,
                            void* userdata) -> heif_error {
                    auto* output = static_cast<MemoryWriter*>(userdata);
                    const auto* begin = static_cast<const std::uint8_t*>(data);
                    output->bytes.insert(output->bytes.end(), begin, begin + size);
                    return heif_error{.code = heif_error_Ok,
                                      .subcode = heif_suberror_Unspecified,
                                      .message = nullptr};
                }};
            MemoryWriter output;
            const heif_error written = heif_context_write(ctx, &writer, &output);
            if (written.code != heif_error_Ok) {
                error = fmt::format("HEIC 序列化失败: {}", written.message ? written.message : "unknown");
                return false;
            }
            return write_bytes(path, output.bytes.data(), output.bytes.size(), error);
        }

        heif_encoder* make_heif_encoder(heif_context* ctx, std::string& error) {
            heif_encoder* encoder = nullptr;
            const heif_error result = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder);
            if (result.code != heif_error_Ok || encoder == nullptr) {
                error = fmt::format("HEVC 编码器不可用: {}", result.message ? result.message : "unknown");
                return nullptr;
            }
            heif_encoder_set_lossy_quality(encoder, codec_options.heic_quality);
            heif_encoder_set_parameter_string(encoder, "chroma", "444");
            heif_encoder_set_parameter_string(encoder, "preset", std::string(codec_options.heic_preset).c_str());
            if (codec_options.heic_lossless)
                heif_encoder_set_lossless(encoder, 1);
            return encoder;
        }

        bool heic_from_bgra(std::filesystem::path const& path, Image const& image, std::string& error) {
            heif_context* ctx = heif_context_alloc();
            if (ctx == nullptr) {
                error = "heif_context_alloc 失败";
                return false;
            }
            heif_image* img = nullptr;
            if (heif_image_create(image.width, image.height, heif_colorspace_RGB, heif_chroma_444, &img).code != heif_error_Ok) {
                heif_context_free(ctx);
                error = "heif_image_create 失败";
                return false;
            }
            for (heif_channel channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
                if (heif_image_add_plane(img, channel, image.width, image.height, 8).code != heif_error_Ok) {
                    free_heif(ctx, img, nullptr);
                    error = "heif_image_add_plane 失败";
                    return false;
                }
            }

            std::size_t stride_r = 0;
            std::size_t stride_g = 0;
            std::size_t stride_b = 0;
            auto* plane_r = heif_image_get_plane2(img, heif_channel_R, &stride_r);
            auto* plane_g = heif_image_get_plane2(img, heif_channel_G, &stride_g);
            auto* plane_b = heif_image_get_plane2(img, heif_channel_B, &stride_b);
            if (plane_r == nullptr || plane_g == nullptr || plane_b == nullptr) {
                free_heif(ctx, img, nullptr);
                error = "heif_image_get_plane2 失败";
                return false;
            }
            for (std::uint32_t y = 0; y < image.height; ++y) {
                auto const* src = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
                bgra_to_rgb_planes_row(src, plane_r + static_cast<std::size_t>(y) * stride_r,
                                       plane_g + static_cast<std::size_t>(y) * stride_g,
                                       plane_b + static_cast<std::size_t>(y) * stride_b, image.width);
            }

            heif_color_profile_nclx* nclx = heif_nclx_color_profile_alloc();
            if (nclx != nullptr) {
                heif_nclx_color_profile_set_color_primaries(nclx, 1);
                heif_nclx_color_profile_set_transfer_characteristics(nclx, 13);
                heif_nclx_color_profile_set_matrix_coefficients(nclx, 1);
                heif_image_set_nclx_color_profile(img, nclx);
                heif_nclx_color_profile_free(nclx);
            }

            heif_encoder* encoder = make_heif_encoder(ctx, error);
            if (encoder == nullptr) {
                free_heif(ctx, img, nullptr);
                return false;
            }
            const bool ok = write_heic_impl(path, ctx, img, encoder, image.metadata, error);
            free_heif(ctx, img, encoder);
            return ok;
        }

        // HEIC 的 x265 插件只支持 8-bit 时，HDR HEIC 会退化为 8-bit PQ。
        bool heif_encode_rgba(std::filesystem::path const& path,
                              void const* pixels,
                              std::uint32_t width,
                              std::uint32_t height,
                              int bit_depth,
                              heif_compression_format compression,
                              bool hdr,
                              ImageMetadata const& metadata,
                              std::string& error) {
            heif_context* ctx = heif_context_alloc();
            if (ctx == nullptr) {
                error = "heif_context_alloc 失败";
                return false;
            }
            heif_image* img = nullptr;
            if (heif_image_create(static_cast<int>(width), static_cast<int>(height), heif_colorspace_RGB,
                                  heif_chroma_444, &img)
                    .code != heif_error_Ok) {
                heif_context_free(ctx);
                error = "heif_image_create 失败";
                return false;
            }
            for (heif_channel channel : {heif_channel_R, heif_channel_G, heif_channel_B}) {
                if (heif_image_add_plane(img, channel, static_cast<int>(width), static_cast<int>(height), bit_depth)
                        .code != heif_error_Ok) {
                    free_heif(ctx, img, nullptr);
                    error = "heif_image_add_plane 失败";
                    return false;
                }
            }

            std::size_t stride_r = 0;
            std::size_t stride_g = 0;
            std::size_t stride_b = 0;
            auto* plane_r = heif_image_get_plane2(img, heif_channel_R, &stride_r);
            auto* plane_g = heif_image_get_plane2(img, heif_channel_G, &stride_g);
            auto* plane_b = heif_image_get_plane2(img, heif_channel_B, &stride_b);
            if (plane_r == nullptr || plane_g == nullptr || plane_b == nullptr) {
                free_heif(ctx, img, nullptr);
                error = "heif_image_get_plane2 失败";
                return false;
            }
            if (bit_depth > 8) {
                auto const* src = static_cast<std::uint16_t const*>(pixels);
                for (std::uint32_t y = 0; y < height; ++y) {
                    auto* r = reinterpret_cast<std::uint16_t*>(plane_r + static_cast<std::size_t>(y) * stride_r);
                    auto* g = reinterpret_cast<std::uint16_t*>(plane_g + static_cast<std::size_t>(y) * stride_g);
                    auto* b = reinterpret_cast<std::uint16_t*>(plane_b + static_cast<std::size_t>(y) * stride_b);
                    for (std::uint32_t x = 0; x < width; ++x) {
                        const auto* p = src + (static_cast<std::size_t>(y) * width + x) * 4;
                        r[x] = p[0];
                        g[x] = p[1];
                        b[x] = p[2];
                    }
                }
            } else {
                auto const* src = static_cast<std::uint8_t const*>(pixels);
                for (std::uint32_t y = 0; y < height; ++y) {
                    rgba_to_rgb_planes_row(src + static_cast<std::size_t>(y) * width * 4,
                                           plane_r + static_cast<std::size_t>(y) * stride_r,
                                           plane_g + static_cast<std::size_t>(y) * stride_g,
                                           plane_b + static_cast<std::size_t>(y) * stride_b, width);
                }
            }

            heif_color_profile_nclx* nclx = heif_nclx_color_profile_alloc();
            if (nclx != nullptr) {
                heif_nclx_color_profile_set_color_primaries(nclx, hdr ? 9 : 1);
                heif_nclx_color_profile_set_transfer_characteristics(nclx, hdr ? 16 : 13);
                heif_nclx_color_profile_set_matrix_coefficients(nclx, hdr ? 9 : 1);
                heif_image_set_nclx_color_profile(img, nclx);
                heif_nclx_color_profile_free(nclx);
            }

            heif_encoder* encoder = nullptr;
            const heif_error got_encoder = heif_context_get_encoder_for_format(ctx, compression, &encoder);
            if (got_encoder.code != heif_error_Ok || encoder == nullptr) {
                free_heif(ctx, img, nullptr);
                error = fmt::format("找不到编码器 ({}): {}",
                                    compression == heif_compression_AV1 ? "AV1" : "HEVC",
                                    got_encoder.message ? got_encoder.message : "unknown");
                return false;
            }
            heif_encoder_set_lossy_quality(encoder, codec_options.heic_quality);
            heif_encoder_set_parameter_string(encoder, "chroma", "444");
            heif_encoder_set_parameter_string(encoder, "preset", std::string(codec_options.heic_preset).c_str());
            if (codec_options.heic_lossless)
                heif_encoder_set_lossless(encoder, 1);

            const bool ok = write_heic_impl(path, ctx, img, encoder, metadata, error);
            free_heif(ctx, img, encoder);
            return ok;
        }

    }  // namespace

    void set_codec_options(CodecOptions const& options) {
        codec_options = options;
    }

    std::string_view to_string(ImageFormat format) {
        return format_name(format);
    }

    std::optional<ImageFormat> parse_image_format(std::string_view text) {
        if (text == "bmp")
            return ImageFormat::Bmp;
        if (text == "png")
            return ImageFormat::Png;
        if (text == "jpeg" || text == "jpg")
            return ImageFormat::Jpeg;
        if (text == "webp")
            return ImageFormat::Webp;
        if (text == "avif")
            return ImageFormat::Avif;
        if (text == "heic" || text == "heif")
            return ImageFormat::Heic;
        return std::nullopt;
    }

    std::wstring_view image_extension(ImageFormat format) {
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
        }
        return L".bin";
    }

    bool format_supports_hdr(ImageFormat format) {
        return format == ImageFormat::Avif || format == ImageFormat::Heic;
    }

    bool encode_avif_hdr10(std::filesystem::path const& path, std::uint16_t const* rgba10,
                           std::uint32_t width, std::uint32_t height,
                           ImageMetadata const& metadata, std::string& error) {
        return avif_from_rgba10(path, rgba10, width, height, metadata, error);
    }

    bool encode_heic_hdr10(std::filesystem::path const& path, std::uint16_t const* rgba10,
                           std::uint32_t width, std::uint32_t height,
                           ImageMetadata const& metadata, std::string& error) {
        static const bool kSupports10Bit = [] {
            const bool available = x265_max_bit_depth >= 10;
            if (!available)
                SPDLOG_INFO("x265 不支持 10-bit，HEIC 直接使用 8-bit PQ");
            return available;
        }();
        if (kSupports10Bit) {
            if (heif_encode_rgba(path, rgba10, width, height, 10,
                                 heif_compression_HEVC, true, metadata, error))
                return true;
            SPDLOG_WARN("HEIC 10-bit 编码失败，回退 8-bit: {}", error);
        }
        const std::size_t count = static_cast<std::size_t>(width) * height * 4;
        ByteBuffer rgba8;
        rgba8.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            rgba8[i] = static_cast<std::uint8_t>(rgba10[i] >> 2);
        return heif_encode_rgba(path, rgba8.data(), width, height, 8,
                                heif_compression_HEVC, true, metadata, error);
    }

    bool encode_avif_gainmap(std::filesystem::path const& path, Image const& sdr,
                             std::uint16_t const* hdr10,
                             std::uint32_t width, std::uint32_t height, std::string& error) {
        return avif_gainmap(path, sdr, hdr10, width, height, error);
    }

    bool write_sdr(std::filesystem::path const& path, ImageFormat format, Image const& image, std::string& error) {
        switch (format) {
        case ImageFormat::Bmp:
            return write_bmp_impl(path, image, error);
        case ImageFormat::Png:
            return write_png_impl(path, image, error);
        case ImageFormat::Jpeg:
            return write_jpeg_impl(path, image, error);
        case ImageFormat::Webp:
            return write_webp_impl(path, image, error);
        case ImageFormat::Avif:
            return avif_from_bgra(path, image, false, error);
        case ImageFormat::Heic:
            return heic_from_bgra(path, image, error);
        }
        return false;
    }

    bool write_hdr(std::filesystem::path const& path, ImageFormat format, HdrFrame const& frame, std::string& error,
                   U16Buffer* rgba10_out) {
        if (!format_supports_hdr(format)) {
            error = "该格式不支持 HDR";
            return false;
        }
        if (rgba10_out != nullptr) {
            *rgba10_out = hdr_to_rgba10(frame);
            return write_hdr10(path, format, rgba10_out->data(), frame.width, frame.height,
                               frame.metadata, error);
        }
        auto rgba10 = hdr_to_rgba10(frame);
        return write_hdr10(path, format, rgba10.data(), frame.width, frame.height, frame.metadata, error);
    }

    bool write_hdr10(std::filesystem::path const& path, ImageFormat format, std::uint16_t const* rgba10,
                     std::uint32_t width, std::uint32_t height, ImageMetadata const& metadata, std::string& error) {
        switch (format) {
        case ImageFormat::Avif:
            return encode_avif_hdr10(path, rgba10, width, height, metadata, error);
        case ImageFormat::Heic:
            return encode_heic_hdr10(path, rgba10, width, height, metadata, error);
        default:
            error = "该格式不支持 HDR";
            return false;
        }
    }

}  // namespace gamesnap
