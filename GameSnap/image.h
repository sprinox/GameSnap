#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 输出画像格式与 CPU 像素缓冲。编码线程和编解码实现共用。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace gamesnap {

    enum class ImageFormat : std::uint8_t {
        Bmp,
        Png,
        Jpeg,
        Webp,
        Avif,
        Heic,
    };

    enum class ToneMapAlgorithm : std::uint8_t {
        Spline,
        Lilium,
    };

    std::string_view to_string(ImageFormat format);
    std::optional<ImageFormat> parse_image_format(std::string_view text);
    std::string_view image_extension(ImageFormat format);
    // HDR 容器：AVIF / HEIC；其余格式只输出 SDR。
    bool format_supports_hdr(ImageFormat format);

    // 紧凑 BGRA8，行间无 padding；stride = width * 4。
    struct Image {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
    };

    // HDR 捕获的 FP16 原始帧；pq=true 表示码值是 BT.2020 PQ，否则是 scRGB 线性。
    struct HdrFrame {
        std::uint8_t const* bytes = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        bool pq = false;
        std::uint32_t sdr_white_nits = 80;
    };

}  // namespace gamesnap
