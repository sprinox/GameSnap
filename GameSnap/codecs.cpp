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
#include <jxl/encode.h>
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

        // 前向声明：这三个由配置推导的辅助函数定义在文件后面，但前面的编码路径要用。
        avifCodecChoice avif_effective_choice();
        char const* heic_chroma_name();
        void jpeg_sampling_factors(int& y_h, int& y_v, int& c_h, int& c_v);

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
            case ImageFormat::Jxl:
                return "jxl";
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

        // 所有输出都是不透明图像：编码前统一丢掉 alpha（实现见 simd.cpp 的 AVX2 版）。
        bool to_rgb(Image const& image, ByteBuffer& rgb) {
            if (image.width == 0 || image.height == 0 || image.stride != image.width * 4)
                return false;
            rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3);
            for (std::size_t y = 0; y < image.height; ++y) {
                auto const* src = image.pixels.data() + y * image.stride;
                auto* dst = rgb.data() + y * image.width * 3;
                bgra_to_rgb_row(src, dst, image.width);
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
                {.tag = 0x010E, .type = 2, .count = static_cast<std::uint32_t>(title.size()) + 1, .value = title_offset},
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

        // 入参用裸指针 + 长度：调用方手里的压缩缓冲（spng/libjpeg 分配的）不必再拷一份 vector。
        std::vector<std::uint8_t> add_jpeg_exif(std::uint8_t const* jpeg, std::size_t jpeg_size,
                                                ImageMetadata const& metadata) {
            const auto exif = make_exif_payload(metadata);
            if (jpeg_size < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
                return {jpeg, jpeg + jpeg_size};
            std::vector<std::uint8_t> out(6 + exif.size() + jpeg_size - 2);
            auto* p = out.data();
            constexpr std::array<std::uint8_t, 4> app1_marker{0xFF, 0xD8, 0xFF, 0xE1};
            std::memcpy(p, app1_marker.data(), app1_marker.size());
            write_u16_be(p + 4, static_cast<std::uint16_t>(exif.size() + 2));
            std::memcpy(p + 6, exif.data(), exif.size());
            std::memcpy(p + 6 + exif.size(), jpeg + 2, jpeg_size - 2);
            return out;
        }

        std::vector<std::uint8_t> add_png_exif(std::uint8_t const* png, std::size_t png_size,
                                               ImageMetadata const& metadata) {
            constexpr std::size_t kIendSize = 12;  // length=0 + "IEND" + crc
            if (png_size < kIendSize)
                return {png, png + png_size};
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

            // IEND 固定为最后 12 字节：length=0 + "IEND" + crc。直接在 out 里拼好，
            // 全程只有这一趟拷贝（不再先构造 png_bytes 再 insert 一次）。
            const std::size_t insert_at = png_size - kIendSize;
            std::vector<std::uint8_t> out;
            out.reserve(png_size + chunk.size());
            out.insert(out.end(), png, png + insert_at);
            out.insert(out.end(), chunk.begin(), chunk.end());
            out.insert(out.end(), png + insert_at, png + png_size);
            return out;
        }

        // 成功时 out 由 WebPMuxAssemble 分配，调用方写完必须 WebPDataClear；失败时 out 为空。
        bool add_webp_exif(WebPData const& webp, ImageMetadata const& metadata, WebPData& out) {
            out = {};
            if (webp.size < 12 || std::memcmp(webp.bytes, "RIFF", 4) != 0 ||
                std::memcmp(static_cast<std::uint8_t const*>(webp.bytes) + 8, "WEBP", 4) != 0) {
                return false;
            }

            const auto exif = make_exif_payload(metadata);
            WebPData exif_data{.bytes = exif.data(), .size = exif.size()};
            WebPMux* mux = WebPMuxNew();
            if (mux == nullptr)
                return false;
            if (WebPMuxSetImage(mux, &webp, 1) != WEBP_MUX_OK ||
                WebPMuxSetChunk(mux, "EXIF", &exif_data, 1) != WEBP_MUX_OK) {
                WebPMuxDelete(mux);
                return false;
            }

            const bool ok = WebPMuxAssemble(mux, &out) == WEBP_MUX_OK;
            WebPMuxDelete(mux);
            if (!ok) {
                WebPDataClear(&out);
                out = {};
            }
            return ok;
        }

        // ── BMP ──────────────────────────────────────────────────────────────
        bool write_bmp_impl(std::filesystem::path const& path, Image const& image, std::string& error) {
            if (image.pixels.empty() || image.width == 0 || image.height == 0) {
                error = "空图像";
                return false;
            }
            // 24bpp：截图不透明，alpha 字节没有意义，写了只是徒增 1/4 体积。
            const std::uint32_t row_bytes = (image.width * 3 + 3) & ~std::uint32_t{3};
            BITMAPFILEHEADER file_header{};
            file_header.bfType = 0x4D42;
            file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            file_header.bfSize = file_header.bfOffBits + (row_bytes * image.height);

            BITMAPINFOHEADER info_header{};
            info_header.biSize = sizeof(BITMAPINFOHEADER);
            info_header.biWidth = static_cast<LONG>(image.width);
            info_header.biHeight = -static_cast<LONG>(image.height);
            info_header.biPlanes = 1;
            info_header.biBitCount = 24;
            info_header.biCompression = BI_RGB;
            info_header.biSizeImage = row_bytes * image.height;

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "无法创建文件";
                return false;
            }
            file.write(reinterpret_cast<char const*>(&file_header), sizeof(file_header));
            file.write(reinterpret_cast<char const*>(&info_header), sizeof(info_header));
            // 按块拼行再一次性写出：4K 下逐行 write 是 2160 次系统调用，攒成 ~4 MB 一块能少两个数量级。
            constexpr std::size_t kChunkTarget = 4 * 1024 * 1024;
            const std::size_t rows_per_chunk = std::max<std::size_t>(1, kChunkTarget / row_bytes);
            ByteBuffer chunk;
            chunk.resize(row_bytes * rows_per_chunk);
            for (std::size_t y = 0; y < image.height; y += rows_per_chunk) {
                const std::size_t rows = std::min(rows_per_chunk, static_cast<std::size_t>(image.height) - y);
                for (std::size_t row_index = 0; row_index < rows; ++row_index) {
                    auto const* src = image.pixels.data() + (y + row_index) * image.stride;
                    auto* dst = chunk.data() + row_index * row_bytes;
                    bgra_to_bgr_row(src, dst, image.width);
                    // 行尾 4 字节对齐的填充保持为 0（chunk 只 append 时同一块会复用，
                    // 这里每行显式清一次，避免上一轮的像素残留在 padding 里）。
                    if (row_bytes > static_cast<std::size_t>(image.width) * 3) {
                        std::memset(dst + static_cast<std::size_t>(image.width) * 3, 0,
                                    row_bytes - static_cast<std::size_t>(image.width) * 3);
                    }
                }
                file.write(reinterpret_cast<char const*>(chunk.data()),
                           static_cast<std::streamsize>(row_bytes * rows));
                if (!file.good())
                    break;
            }
            file.flush();
            if (!file.good()) {
                error = "写 BMP 失败";
                file.close();
                // 别留半张图：磁盘满/中断时删掉残file，免得看起来像一次成功截图。
                std::error_code remove_ec;
                std::filesystem::remove(path, remove_ec);
                return false;
            }
            file.close();
            if (file.fail()) {
                error = "关闭 BMP 失败";
                std::error_code remove_ec;
                std::filesystem::remove(path, remove_ec);
                return false;
            }
            return true;
        }

        // ── PNG（libspng，最高压缩） ─────────────────────────────────────────
        bool write_png_impl(std::filesystem::path const& path, Image const& image, std::string& error) {
            // 不透明图像用 TRUECOLOR（3 通道），省掉恒为 255 的 alpha 通道。
            ByteBuffer rgb;
            if (!to_rgb(image, rgb)) {
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
            ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR;
            ihdr.compression_method = 0;
            ihdr.filter_method = 0;
            ihdr.interlace_method = 0;
            if (spng_set_ihdr(ctx, &ihdr) != 0) {
                spng_ctx_free(ctx);
                error = "spng_set_ihdr 失败";
                return false;
            }
            const int encode_result =
                spng_encode_image(ctx, rgb.data(), rgb.size(), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
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
            const auto with_exif = add_png_exif(static_cast<std::uint8_t const*>(png), png_size, image.metadata);
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
            // 色度采样由配置决定：444（默认）/ 422 / 420。
            int y_h = 1, y_v = 1, c_h = 1, c_v = 1;
            jpeg_sampling_factors(y_h, y_v, c_h, c_v);
            if (scratch.cinfo.num_components == 3) {
                scratch.cinfo.comp_info[0].h_samp_factor = y_h;
                scratch.cinfo.comp_info[0].v_samp_factor = y_v;
                scratch.cinfo.comp_info[1].h_samp_factor = c_h;
                scratch.cinfo.comp_info[1].v_samp_factor = c_v;
                scratch.cinfo.comp_info[2].h_samp_factor = c_h;
                scratch.cinfo.comp_info[2].v_samp_factor = c_v;
            }
            jpeg_start_compress(&scratch.cinfo, TRUE);

            while (scratch.cinfo.next_scanline < scratch.cinfo.image_height) {
                auto const* src =
                    image.pixels.data() + static_cast<std::size_t>(scratch.cinfo.next_scanline) * image.stride;
                // 复用 AVX2 的 BGRA→RGB（和 PNG/JXL 同一条路径），别再用逐像素循环。
                bgra_to_rgb_row(src, scratch.row.data(), image.width);
                JSAMPROW row_pointer = scratch.row.data();
                jpeg_write_scanlines(&scratch.cinfo, &row_pointer, 1);
            }
            jpeg_finish_compress(&scratch.cinfo);
            const auto with_exif = add_jpeg_exif(static_cast<std::uint8_t const*>(scratch.mem),
                                                 static_cast<std::size_t>(scratch.mem_size), image.metadata);
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
            // 直接用原生 BGRA 导入（libwebp 内部转 ARGB，走 lossless 的精确 RGB 路径）；
            // alpha 全 255 时它不会写 alpha 通道，所以不会多出无用通道。
            picture.use_argb = 1;
            if (WebPPictureImportBGRA(&picture, image.pixels.data(),
                                      static_cast<int>(image.stride)) == 0) {
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
                // writer.mem 在 WebPMemoryWriterClear 之前一直有效，直接把它交给 mux，
                // 省掉"再拷一份整图到 vector"和"再拷一份装配结果"。
                const WebPData encoded{.bytes = writer.mem, .size = writer.size};
                WebPData with_exif{};
                if (add_webp_exif(encoded, image.metadata, with_exif)) {
                    result = write_bytes(path, with_exif.bytes, with_exif.size, error);
                    WebPDataClear(&with_exif);
                } else {
                    result = write_bytes(path, encoded.bytes, encoded.size, error);
                }
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

            // AV1 编码库：配置指定了就用它，但库里没编进去时回落到 AUTO，而不是整张图失败。
            encoder->codecChoice = avif_effective_choice();
            if (encoder->codecChoice == AVIF_CODEC_CHOICE_AUTO &&
                codec_options.av1_encoder != Av1Encoder::Auto) {
                SPDLOG_WARN("libavif 未内置 AV1 编码器 {}，回退 auto", to_string(codec_options.av1_encoder));
            }
            // 各库专属参数：只设给实际选中的编码器（AUTO 时两个都试，失败静默），
            // 免得把 svt 的 preset 塞给 aom 还打一条没意义的 warning。
            const auto choice = encoder->codecChoice;
            if (codec_options.aom_cpu_used >= 0 && choice != AVIF_CODEC_CHOICE_SVT) {
                const std::string value = std::to_string(codec_options.aom_cpu_used);
                if (avifEncoderSetCodecSpecificOption(encoder, "cpu-used", value.c_str()) != AVIF_RESULT_OK &&
                    choice == AVIF_CODEC_CHOICE_AOM) {
                    SPDLOG_WARN("aom 不接受 cpu-used={}", value);
                }
            }
            if (codec_options.svt_preset >= 0 && choice != AVIF_CODEC_CHOICE_AOM) {
                const std::string value = std::to_string(codec_options.svt_preset);
                if (avifEncoderSetCodecSpecificOption(encoder, "preset", value.c_str()) != AVIF_RESULT_OK &&
                    choice == AVIF_CODEC_CHOICE_SVT) {
                    SPDLOG_WARN("svt 不接受 preset={}", value);
                }
            }
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

        // 配置里选的 AV1 编码器 → 实际会用的编码器：库里没编进去就回落 AUTO。
        // 像素格式和 encoder->codecChoice 都以它为准，避免"图像按 4:2:0 建、实际跑 aom 444"这类错配。
        avifCodecChoice avif_effective_choice() {
            switch (codec_options.av1_encoder) {
            case Av1Encoder::Aom:
                if (avifCodecName(AVIF_CODEC_CHOICE_AOM, AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr)
                    return AVIF_CODEC_CHOICE_AOM;
                break;
            case Av1Encoder::Svt:
                if (avifCodecName(AVIF_CODEC_CHOICE_SVT, AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr)
                    return AVIF_CODEC_CHOICE_SVT;
                break;
            case Av1Encoder::Auto:
                return AVIF_CODEC_CHOICE_AUTO;
            }
            return AVIF_CODEC_CHOICE_AUTO;  // 配置的编码器不可用
        }

        // 色度采样：SVT 插件只接受 4:2:0，其余按配置（auto = 4:4:4，无损场景首选）。
        avifPixelFormat avif_yuv_format() {
            if (avif_effective_choice() == AVIF_CODEC_CHOICE_SVT)
                return AVIF_PIXEL_FORMAT_YUV420;
            // 无损必须 4:4:4，否则 RGB→YCbCr 抽样那一步就丢了信息（启动时已告警）。
            if (codec_options.avif_lossless)
                return AVIF_PIXEL_FORMAT_YUV444;
            switch (codec_options.chroma) {
            case ChromaSubsampling::Yuv422:
                return AVIF_PIXEL_FORMAT_YUV422;
            case ChromaSubsampling::Yuv420:
                return AVIF_PIXEL_FORMAT_YUV420;
            case ChromaSubsampling::Auto:
            case ChromaSubsampling::Yuv444:
                break;
            }
            return AVIF_PIXEL_FORMAT_YUV444;
        }

        // 只处理 8-bit SDR：Image 恒为 BGRA8，HDR 走 avif_from_rgba10()。
        // 曾经这里有个 hdr 形参会把 depth 设成 10，而缓冲仍是 8-bit 的——一用就按 uint16 越界读。
        bool avif_from_bgra(std::filesystem::path const& path, Image const& image, std::string& error) {
            constexpr std::uint32_t depth = 8;
            avifImage* avif = avifImageCreate(image.width, image.height, depth, avif_yuv_format());
            if (avif == nullptr) {
                error = "avifImageCreate 失败";
                return false;
            }
            // 同 avif_from_rgba10：identity 矩阵的平面顺序有歧义，改用标准 YCbCr 矩阵。
            // 范围统一用 full（与 HEIC/JXL 一致）：444 + full 时 RGB↔YUV 往返几乎无损失。
            avif->yuvRange = AVIF_RANGE_FULL;
            avif->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
            avif->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
            avif->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;

            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, avif);
            rgb.format = AVIF_RGB_FORMAT_BGRA;
            rgb.depth = depth;
            rgb.rowBytes = image.stride;
            // 截图恒不透明：跳过 alpha 平面的分配与逐像素填充。
            rgb.ignoreAlpha = AVIF_TRUE;
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
        // 只接受 FP16 RGBA：WGC 实际给的帧格式可能和请求的不同（驱动转换、旋转、HDR 被关掉），
        // 盲按 8 字节/像素解读 4 字节/像素的缓冲会越界读取。
        U16Buffer hdr_to_rgba10(HdrFrame const& frame, std::string& error) {
            if (frame.bytes == nullptr || frame.width == 0 || frame.height == 0 ||
                frame.bytes_per_pixel != 8 || frame.stride < static_cast<std::size_t>(frame.width) * 8) {
                error = "HDR 源不是 FP16 RGBA（bytes/pixel 或 stride 不符）";
                return {};
            }
            U16Buffer rgba;
            rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
            std::vector<float> row_floats(static_cast<std::size_t>(frame.width) * 4);
            for (std::size_t y = 0; y < frame.height; ++y) {
                auto const* row16 =
                    reinterpret_cast<std::uint16_t const*>(frame.bytes + y * frame.stride);
                half_to_float_row(row16, row_floats.data(), static_cast<std::size_t>(frame.width) * 4);
                for (std::size_t x = 0; x < frame.width; ++x) {
                    RgbLinear rgb{.r = row_floats[x * 4 + 0],
                                  .g = row_floats[x * 4 + 1],
                                  .b = row_floats[x * 4 + 2]};
                    auto* p = rgba.data() + (y * frame.width + x) * 4;
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
            avifImage* avif = avifImageCreate(width, height, 10, avif_yuv_format());
            if (avif == nullptr) {
                error = "avifImageCreate 失败";
                return false;
            }
            // 不要用 AVIF_MATRIX_COEFFICIENTS_IDENTITY：identity 的平面顺序在 CICP/AV1 里是
            // G,B,R，而 libavif 按 R,G,B 写，按规范解码的实现会把红蓝换过来。
            // 用 BT.2020 非恒定亮度矩阵；范围与其它容器统一为 full。
            avif->yuvRange = AVIF_RANGE_FULL;
            avif->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
            avif->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_PQ;
            avif->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;

            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, avif);
            rgb.format = AVIF_RGB_FORMAT_RGBA;
            rgb.depth = 10;
            rgb.rowBytes = width * 8;
            rgb.ignoreAlpha = AVIF_TRUE;                                                          // 不透明，别让 libavif 建 alpha 平面
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

            avifImage* base = avifImageCreate(width, height, 8, avif_yuv_format());
            if (base == nullptr) {
                error = "avifImageCreate(base) 失败";
                return false;
            }
            base->yuvRange = AVIF_RANGE_FULL;
            base->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
            base->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
            // identity 矩阵要求 4:4:4，且平面顺序有歧义；和 write_sdr 保持一致用 BT.709 NCL。
            base->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;

            avifRGBImage base_rgb{};
            base_rgb.width = width;
            base_rgb.height = height;
            base_rgb.depth = 8;
            base_rgb.format = AVIF_RGB_FORMAT_BGRA;
            base_rgb.rowBytes = sdr.stride;
            base_rgb.ignoreAlpha = AVIF_TRUE;                                // 不透明
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
            alt_rgb.ignoreAlpha = AVIF_TRUE;                                                           // 不透明
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
            // IDENTITY = "YUV 平面里直接放 RGB"，配合 4:4:4 + full range 才是无损的 RGB 承载；
            // 换成 BT2020_NCL 反而会再做一次 RGB→YCbCr，和独立 HDR AVIF 的编码路径就不一致了。
            // altYUVRange 必须显式写 FULL，否则读端可能按 narrow range 解释（画面发灰）。
            gain_map->altMatrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
            gain_map->altYUVRange = AVIF_RANGE_FULL;
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
                // libheif 的 EXIF 接口约定：传入 "Exif\0\0" + TIFF（即 make_exif_payload 的格式），
                // 它会自己扫描 TIFF header 定位数据。换库/换版本时这点要重新确认。
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

        // SDR 与 HDR 两条 HEIC 路径共用：把 heic_* 配置应用到编码器。
        // libheif 的这些 setter 失败不影响编码能否继续，但会让配置悄悄不生效（比如 preset 拼错），
        // 所以统一记录日志，避免"配了 slower 却没变小"这种无迹可查的情况。
        void apply_heic_encoder_options(heif_encoder* encoder) {
            if (encoder == nullptr)
                return;
            const auto check = [](heif_error const& result, char const* what) {
                if (result.code != heif_error_Ok)
                    SPDLOG_WARN("HEIC 设置 {} 失败: {}", what, result.message ? result.message : "unknown");
            };
            check(heif_encoder_set_lossy_quality(encoder, codec_options.heic_quality), "quality");
            check(heif_encoder_set_parameter_string(encoder, "chroma", heic_chroma_name()), "chroma");
            check(heif_encoder_set_parameter_string(encoder, "preset", std::string(codec_options.heic_preset).c_str()),
                  "preset");
            if (codec_options.heic_lossless)
                check(heif_encoder_set_lossless(encoder, 1), "lossless");
        }

        heif_encoder* make_heif_encoder(heif_context* ctx, std::string& error) {
            heif_encoder* encoder = nullptr;
            const heif_error result = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder);
            if (result.code != heif_error_Ok || encoder == nullptr) {
                error = fmt::format("HEVC 编码器不可用: {}", result.message ? result.message : "unknown");
                return nullptr;
            }
            apply_heic_encoder_options(encoder);
            return encoder;
        }

        // 色度采样 → libheif 的 chroma 参数名；auto 保持 4:4:4（保留 UI 文字/细线）。
        char const* heic_chroma_name() {
            // 无损必须 4:4:4（启动时已告警）。
            if (codec_options.heic_lossless)
                return "444";
            switch (codec_options.chroma) {
            case ChromaSubsampling::Yuv422:
                return "422";
            case ChromaSubsampling::Yuv420:
                return "420";
            case ChromaSubsampling::Auto:
            case ChromaSubsampling::Yuv444:
                break;
            }
            return "444";
        }

        // 色度采样 → JPEG 的采样因子（Y 与 Cb/Cr 各自）。
        void jpeg_sampling_factors(int& y_h, int& y_v, int& c_h, int& c_v) {
            y_h = 1, y_v = 1, c_h = 1, c_v = 1;
            switch (codec_options.chroma) {
            case ChromaSubsampling::Yuv422:
                y_h = 2, y_v = 1;
                break;
            case ChromaSubsampling::Yuv420:
                y_h = 2, y_v = 2;
                break;
            case ChromaSubsampling::Auto:
            case ChromaSubsampling::Yuv444:
                break;
            }
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
            for (std::size_t y = 0; y < image.height; ++y) {
                auto const* src = image.pixels.data() + y * image.stride;
                bgra_to_rgb_planes_row(src, plane_r + y * stride_r,
                                       plane_g + y * stride_g,
                                       plane_b + y * stride_b, image.width);
            }

            heif_color_profile_nclx* nclx = heif_nclx_color_profile_alloc();
            if (nclx != nullptr) {
                heif_nclx_color_profile_set_color_primaries(nclx, 1);
                heif_nclx_color_profile_set_transfer_characteristics(nclx, 13);
                heif_nclx_color_profile_set_matrix_coefficients(nclx, 1);
                const heif_error nclx_error = heif_image_set_nclx_color_profile(img, nclx);
                if (nclx_error.code != heif_error_Ok)
                    SPDLOG_WARN("HEIC 设置 NCLX 色彩配置失败: {}",
                                nclx_error.message ? nclx_error.message : "unknown");
                heif_nclx_color_profile_free(nclx);
            } else {
                SPDLOG_WARN("heif_nclx_color_profile_alloc 失败，HEIC 将不带色彩空间标记");
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
                for (std::size_t y = 0; y < height; ++y) {
                    auto* r = reinterpret_cast<std::uint16_t*>(plane_r + y * stride_r);
                    auto* g = reinterpret_cast<std::uint16_t*>(plane_g + y * stride_g);
                    auto* b = reinterpret_cast<std::uint16_t*>(plane_b + y * stride_b);
                    for (std::size_t x = 0; x < width; ++x) {
                        const auto* p = src + (y * width + x) * 4;
                        r[x] = p[0];
                        g[x] = p[1];
                        b[x] = p[2];
                    }
                }
            } else {
                auto const* src = static_cast<std::uint8_t const*>(pixels);
                for (std::size_t y = 0; y < height; ++y) {
                    rgba_to_rgb_planes_row(src + y * width * 4,
                                           plane_r + y * stride_r,
                                           plane_g + y * stride_g,
                                           plane_b + y * stride_b, width);
                }
            }

            heif_color_profile_nclx* nclx = heif_nclx_color_profile_alloc();
            if (nclx != nullptr) {
                heif_nclx_color_profile_set_color_primaries(nclx, hdr ? 9 : 1);
                heif_nclx_color_profile_set_transfer_characteristics(nclx, hdr ? 16 : 13);
                heif_nclx_color_profile_set_matrix_coefficients(nclx, hdr ? 9 : 1);
                const heif_error nclx_error = heif_image_set_nclx_color_profile(img, nclx);
                if (nclx_error.code != heif_error_Ok)
                    SPDLOG_WARN("HEIC 设置 NCLX 色彩配置失败: {}",
                                nclx_error.message ? nclx_error.message : "unknown");
                heif_nclx_color_profile_free(nclx);
            } else {
                SPDLOG_WARN("heif_nclx_color_profile_alloc 失败，HEIC 将不带色彩空间标记");
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
            // 与 SDR 路径共用参数应用逻辑（含失败日志），避免 HDR 这边静默丢配置。
            apply_heic_encoder_options(encoder);

            const bool ok = write_heic_impl(path, ctx, img, encoder, metadata, error);
            free_heif(ctx, img, encoder);
            return ok;
        }

        // ── JXL（libjxl） ────────────────────────────────────────────────────
        // pixels 是紧凑 RGB：8-bit 用 JXL_TYPE_UINT8，10-bit PQ 用 JXL_TYPE_UINT16
        // 且通过 JXL_BIT_DEPTH_FROM_CODESTREAM 声明“未缩放”（值域 0..1023）。
        // 截图不透明，所以不写 alpha extra channel。
        bool write_jxl_impl(std::filesystem::path const& path, void const* pixels,
                            std::uint32_t width, std::uint32_t height, bool hdr,
                            ImageMetadata const& metadata, std::string& error) {
            JxlEncoder* enc = JxlEncoderCreate(nullptr);
            if (enc == nullptr) {
                error = "JxlEncoderCreate 失败";
                return false;
            }
            auto guard = ScopeExit{[&] { JxlEncoderDestroy(enc); }};

            JxlBasicInfo info{};
            JxlEncoderInitBasicInfo(&info);
            info.xsize = width;
            info.ysize = height;
            info.num_color_channels = 3;
            info.bits_per_sample = hdr ? 10 : 8;
            info.exponent_bits_per_sample = 0;
            // 不透明图像：不声明 alpha，也不加 extra channel。
            info.num_extra_channels = 0;
            info.alpha_bits = 0;
            // 保留我们写入的色彩编码（PQ/BT.2020 或 sRGB），不让编码器换成 ICC。
            info.uses_original_profile = JXL_TRUE;
            if (hdr)
                info.intensity_target = 1000.0F;
            if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
                error = "JxlEncoderSetBasicInfo 失败";
                return false;
            }

            JxlColorEncoding color{};
            if (hdr) {
                color.color_space = JXL_COLOR_SPACE_RGB;
                color.white_point = JXL_WHITE_POINT_D65;
                color.primaries = JXL_PRIMARIES_2100;
                color.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
                color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
            } else {
                JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
            }
            if (JxlEncoderSetColorEncoding(enc, &color) != JXL_ENC_SUCCESS) {
                error = "JxlEncoderSetColorEncoding 失败";
                return false;
            }

            JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(enc, nullptr);
            if (frame == nullptr) {
                error = "JxlEncoderFrameSettingsCreate 失败";
                return false;
            }
            // libjxl 的这些 setter 都有返回状态：拒绝时会静默用默认值，所以逐个检查。
            const auto check_jxl = [](JxlEncoderStatus status, char const* what) {
                if (status != JXL_ENC_SUCCESS)
                    SPDLOG_WARN("JXL 设置 {} 被拒绝，编码将使用库默认值", what);
            };
            if (codec_options.jxl_lossless) {
                check_jxl(JxlEncoderSetFrameLossless(frame, JXL_TRUE), "lossless");
                check_jxl(JxlEncoderSetFrameDistance(frame, 0.0F), "distance=0");
            } else {
                check_jxl(JxlEncoderSetFrameDistance(frame, std::clamp(codec_options.jxl_distance, 0.0F, 15.0F)),
                          "distance");
            }
            check_jxl(JxlEncoderFrameSettingsSetOption(frame, JXL_ENC_FRAME_SETTING_EFFORT,
                                                       std::clamp(codec_options.jxl_effort, 1, 10)),
                      "effort");

            if (hdr) {
                JxlBitDepth depth{};
                depth.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
                depth.bits_per_sample = 10;
                depth.exponent_bits_per_sample = 0;
                check_jxl(JxlEncoderSetFrameBitDepth(frame, &depth), "bit_depth");
            }

            // EXIF 用独立 box。libjxl 要求 box 内容以 4 字节大端 TIFF 偏移开头
            // （decode.h: "Starts with a 4-byte tiff header offset"），TIFF 头紧随其后；
            // 实测偏移写 0 时 ffmpeg/djxl 都能正常解析，写 4 反而报 "invalid TIFF header"。
            if (!metadata.title.empty() || !metadata.date_time.empty()) {
                JxlEncoderUseBoxes(enc);
                const auto tiff = make_exif_tiff(metadata);
                if (!tiff.empty()) {
                    std::vector<std::uint8_t> box(4 + tiff.size(), 0);
                    std::memcpy(box.data() + 4, tiff.data(), tiff.size());
                    if (JxlEncoderAddBox(enc, "Exif", box.data(), box.size(), JXL_FALSE) != JXL_ENC_SUCCESS)
                        SPDLOG_WARN("JXL 写入 EXIF box 失败（元数据不会出现在文件里）");
                }
            }

            JxlPixelFormat format{
                .num_channels = 3,
                .data_type = hdr ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                .endianness = JXL_NATIVE_ENDIAN,
                .align = 0,
            };
            const std::size_t bytes_per_pixel = hdr ? 6 : 3;
            const std::size_t size = static_cast<std::size_t>(width) * height * bytes_per_pixel;
            if (JxlEncoderAddImageFrame(frame, &format, pixels, size) != JXL_ENC_SUCCESS) {
                error = "JxlEncoderAddImageFrame 失败";
                return false;
            }
            JxlEncoderCloseInput(enc);

            std::vector<std::uint8_t> out(1 << 16);
            std::uint8_t* next_out = out.data();
            std::size_t avail = out.size();
            for (;;) {
                const JxlEncoderStatus status = JxlEncoderProcessOutput(enc, &next_out, &avail);
                if (status == JXL_ENC_SUCCESS)
                    break;
                if (status != JXL_ENC_NEED_MORE_OUTPUT) {
                    error = "JxlEncoderProcessOutput 失败";
                    return false;
                }
                const std::size_t used = static_cast<std::size_t>(next_out - out.data());
                out.resize(out.size() * 2);
                next_out = out.data() + used;
                avail = out.size() - used;
            }
            return write_bytes(path, out.data(), static_cast<std::size_t>(next_out - out.data()), error);
        }

    }  // namespace

    void set_codec_options(CodecOptions const& options) {
        codec_options = options;
        // 启动时把 libavif 实际编进去的 AV1 编码器打出来，避免配置选了 svt 却悄悄回落。
        const auto name_of = [](avifCodecChoice choice) -> std::string_view {
            const char* name = avifCodecName(choice, AVIF_CODEC_FLAG_CAN_ENCODE);
            return name != nullptr ? std::string_view(name) : std::string_view();
        };
        const auto aom = name_of(AVIF_CODEC_CHOICE_AOM);
        const auto svt = name_of(AVIF_CODEC_CHOICE_SVT);
        SPDLOG_INFO("AV1 编码器: libavif 支持 aom={} svt={}（默认 auto={}），配置选择 {}",
                    aom.empty() ? "否" : "是", svt.empty() ? "否" : "是",
                    name_of(AVIF_CODEC_CHOICE_AUTO), to_string(options.av1_encoder));
        // 无损与色度抽样冲突时以无损为准，这里提前说清楚，免得看文件时才发现采样被强制改回 444。
        if (options.chroma != ChromaSubsampling::Auto && options.chroma != ChromaSubsampling::Yuv444) {
            if (options.avif_lossless)
                SPDLOG_WARN("AVIF 无损固定 4:4:4，忽略 chroma_subsampling={}", to_string(options.chroma));
            if (options.heic_lossless)
                SPDLOG_WARN("HEIC 无损固定 4:4:4，忽略 chroma_subsampling={}", to_string(options.chroma));
        }
    }

    std::string_view to_string(ImageFormat format) {
        return format_name(format);
    }

    std::string_view to_string(Av1Encoder encoder) {
        switch (encoder) {
        case Av1Encoder::Auto:
            return "auto";
        case Av1Encoder::Aom:
            return "aom";
        case Av1Encoder::Svt:
            return "svt";
        }
        return "unknown";
    }

    std::optional<Av1Encoder> parse_av1_encoder(std::string_view text) {
        if (text == "auto")
            return Av1Encoder::Auto;
        if (text == "aom")
            return Av1Encoder::Aom;
        if (text == "svt" || text == "svt-av1" || text == "svtav1")
            return Av1Encoder::Svt;
        return std::nullopt;
    }

    std::string_view to_string(ChromaSubsampling chroma) {
        switch (chroma) {
        case ChromaSubsampling::Auto:
            return "auto";
        case ChromaSubsampling::Yuv444:
            return "444";
        case ChromaSubsampling::Yuv422:
            return "422";
        case ChromaSubsampling::Yuv420:
            return "420";
        }
        return "unknown";
    }

    std::optional<ChromaSubsampling> parse_chroma_subsampling(std::string_view text) {
        if (text == "auto")
            return ChromaSubsampling::Auto;
        if (text == "444" || text == "4:4:4")
            return ChromaSubsampling::Yuv444;
        if (text == "422" || text == "4:2:2")
            return ChromaSubsampling::Yuv422;
        if (text == "420" || text == "4:2:0")
            return ChromaSubsampling::Yuv420;
        return std::nullopt;
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
        if (text == "jxl" || text == "jpegxl" || text == "jpeg-xl")
            return ImageFormat::Jxl;
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
        case ImageFormat::Jxl:
            return L".jxl";
        }
        return L".bin";
    }

    bool format_supports_hdr(ImageFormat format) {
        return format == ImageFormat::Avif || format == ImageFormat::Heic || format == ImageFormat::Jxl;
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
        // 10-bit → 8-bit 按比例四舍五入（1023→255）；AVX2 版，比逐元素整数除法快 3 倍多。
        rgba10_to_rgba8(rgba10, rgba8.data(), count);
        return heif_encode_rgba(path, rgba8.data(), width, height, 8,
                                heif_compression_HEVC, true, metadata, error);
    }

    bool encode_avif_gainmap(std::filesystem::path const& path, Image const& sdr,
                             std::uint16_t const* hdr10,
                             std::uint32_t width, std::uint32_t height, std::string& error) {
        // SVT 只接受 4:2:0，gain map 的 base/alt 需要 4:4:4 语义，这里退化成普通 SDR AVIF。
        if (codec_options.av1_encoder == Av1Encoder::Svt) {
            SPDLOG_WARN("SVT-AV1 只支持 4:2:0，SDR AVIF 跳过 gain map");
            return write_sdr(path, ImageFormat::Avif, sdr, error);
        }
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
            return avif_from_bgra(path, image, error);
        case ImageFormat::Heic:
            return heic_from_bgra(path, image, error);
        case ImageFormat::Jxl: {
            ByteBuffer rgb;
            if (!to_rgb(image, rgb)) {
                error = "空图像或步幅不匹配";
                return false;
            }
            return write_jxl_impl(path, rgb.data(), image.width, image.height, false, image.metadata, error);
        }
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
            *rgba10_out = hdr_to_rgba10(frame, error);
            if (rgba10_out->empty())
                return false;  // error 已由 hdr_to_rgba10 写入
            return write_hdr10(path, format, rgba10_out->data(), frame.width, frame.height,
                               frame.metadata, error);
        }
        auto rgba10 = hdr_to_rgba10(frame, error);
        if (rgba10.empty())
            return false;
        return write_hdr10(path, format, rgba10.data(), frame.width, frame.height, frame.metadata, error);
    }

    bool write_hdr10(std::filesystem::path const& path, ImageFormat format, std::uint16_t const* rgba10,
                     std::uint32_t width, std::uint32_t height, ImageMetadata const& metadata, std::string& error) {
        switch (format) {
        case ImageFormat::Avif:
            return encode_avif_hdr10(path, rgba10, width, height, metadata, error);
        case ImageFormat::Heic:
            return encode_heic_hdr10(path, rgba10, width, height, metadata, error);
        case ImageFormat::Jxl: {
            // 丢掉 alpha：10-bit RGB 交错缓冲。
            U16Buffer rgb;
            rgb.resize(static_cast<std::size_t>(width) * height * 3);
            for (std::uint32_t y = 0; y < height; ++y) {
                rgba16_to_rgb16_row(rgba10 + static_cast<std::size_t>(y) * width * 4,
                                    rgb.data() + static_cast<std::size_t>(y) * width * 3, width);
            }
            return write_jxl_impl(path, rgb.data(), width, height, true, metadata, error);
        }
        default:
            error = "该格式不支持 HDR";
            return false;
        }
    }

}  // namespace gamesnap
