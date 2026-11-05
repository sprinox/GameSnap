#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 输出画像格式与 CPU 像素缓冲。编码线程和编解码实现共用。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gamesnap {

    enum class ImageFormat : std::uint8_t {
        Bmp = 1,
        Png,
        Jpeg,
        Webp,
        Avif,
        Heic,
    };

    enum class ToneMapAlgorithm : std::uint8_t {
        Spline = 1,
        Analysis,
    };

    std::string_view to_string(ImageFormat format);
    std::optional<ImageFormat> parse_image_format(std::string_view text);
    std::wstring_view image_extension(ImageFormat format);
    // HDR 容器：AVIF / HEIC；其余格式只输出 SDR。
    bool format_supports_hdr(ImageFormat format);

    // 各编码器的质量/无损/速度参数。BMP 固定无损，PNG 固定无损（compression 控制速度）。
    struct CodecOptions {
        int png_compression = 6;  // 0..9，高=更慢/更小

        int jpeg_quality = 95;  // 1..100

        int webp_quality = 90;  // 0..100
        bool webp_lossless = false;
        int webp_method = 4;  // 0..6，高=更慢/更小

        int avif_quality = 90;  // 0..100
        bool avif_lossless = false;
        int avif_speed = 10;  // 0..10，高=更快

        int heic_quality = 90;  // 0..100
        bool heic_lossless = false;
        std::string_view heic_preset = "fast";  // x265 preset
    };

    // 写入 EXIF 的截图信息；时间格式为 YYYY:MM:DD HH:MM:SS。
    struct ImageMetadata {
        std::string title;
        std::string date_time;
    };

    // 紧凑 BGRA8，行间无 padding；stride = width * 4。
    struct Image {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        ImageMetadata metadata;
    };

    // HDR 捕获的 FP16 原始帧；pq=true 表示码值是 BT.2020 PQ，否则是 scRGB 线性。
    struct HdrFrame {
        std::uint8_t const* bytes = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        bool pq = false;
        std::uint32_t sdr_white_nits = 80;
        ImageMetadata metadata;
    };

}  // namespace gamesnap
