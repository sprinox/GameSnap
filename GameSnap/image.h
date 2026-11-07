#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 输出画像格式与 CPU 像素缓冲。编码线程和编解码实现共用。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace gamesnap {

    // 未初始化缓冲：resize() 只分配、不置零。用于“随后会被整体写满”的像素和中间缓冲，
    // 省掉 4K HDR 一张图几十 MiB 的 memset。新增区域的内容是未定义的，读之前必须先写。
    template <typename T>
    class UninitBuffer {
        static_assert(std::is_trivially_copyable_v<T>, "UninitBuffer 只用于可直接 memcpy 的类型");

    public:
        UninitBuffer() = default;
        ~UninitBuffer() = default;
        UninitBuffer(UninitBuffer const&) = delete;
        UninitBuffer& operator=(UninitBuffer const&) = delete;
        UninitBuffer(UninitBuffer&&) noexcept = default;
        UninitBuffer& operator=(UninitBuffer&&) noexcept = default;

        [[nodiscard]] T* data() noexcept { return data_.get(); }
        [[nodiscard]] T const* data() const noexcept { return data_.get(); }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        T& operator[](std::size_t index) noexcept { return data_[index]; }
        T const& operator[](std::size_t index) const noexcept { return data_[index]; }

        void clear() noexcept { size_ = 0; }

        // 变大时重新分配并保留旧内容；新增区域不初始化。变小时只缩短长度。
        void resize(std::size_t count) {
            if (count <= size_) {
                size_ = count;
                return;
            }
            auto next = std::make_unique_for_overwrite<T[]>(count);
            if (size_ != 0)
                std::memcpy(next.get(), data_.get(), size_ * sizeof(T));
            data_ = std::move(next);
            size_ = count;
        }

        void swap(UninitBuffer& other) noexcept {
            data_.swap(other.data_);
            std::swap(size_, other.size_);
        }

    private:
        std::unique_ptr<T[]> data_;
        std::size_t size_ = 0;
    };

    using ByteBuffer = UninitBuffer<std::uint8_t>;
    using U16Buffer = UninitBuffer<std::uint16_t>;

    enum class ImageFormat : std::uint8_t {
        Bmp = 1,
        Png,
        Jpeg,
        Webp,
        Avif,
        Heic,
        Jxl,
    };

    // AVIF（AV1）可选的编码库：auto 交给 libavif 自己挑，其余强制指定。
    enum class Av1Encoder : std::uint8_t {
        Auto = 0,
        Aom,
        Svt,
    };

    // YUV 色度采样。auto = 各编码器自己的默认（AVIF/HEIC/JPEG 都是 4:4:4 优先）。
    // 只对走 YCbCr 的编码器有效：PNG/BMP/JXL/WebP 无损是 RGB，WebP 有损固定 4:2:0。
    enum class ChromaSubsampling : std::uint8_t {
        Auto = 0,
        Yuv444,
        Yuv422,
        Yuv420,
    };

    enum class ToneMapBackend : std::uint8_t {
        Auto = 0,
        Gpu,
        Cpu,
    };

    enum class ToneMapAlgorithm : std::uint8_t {
        Spline = 1,
        Analysis,
    };

    std::string_view to_string(ImageFormat format);
    std::optional<ImageFormat> parse_image_format(std::string_view text);
    std::string_view to_string(Av1Encoder encoder);
    std::optional<Av1Encoder> parse_av1_encoder(std::string_view text);
    std::string_view to_string(ChromaSubsampling chroma);
    std::optional<ChromaSubsampling> parse_chroma_subsampling(std::string_view text);
    std::wstring_view image_extension(ImageFormat format);
    // HDR 容器：AVIF / HEIC / JXL；其余格式只输出 SDR。
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

        // JXL（libjxl）：distance 0 = 数学无损（还要 jxl_lossless），1.0 = 视觉无损；effort 越高越慢越小。
        float jxl_distance = 1.0F;  // 0..15
        bool jxl_lossless = false;
        int jxl_effort = 7;  // 1..10

        // AVIF 的 AV1 编码库与各库专属参数；-1 表示交给 avif_speed 推导。
        Av1Encoder av1_encoder = Av1Encoder::Auto;
        int aom_cpu_used = -1;  // 0..8，aom -cpu-used
        int svt_preset = -1;    // 0..13，SVT-AV1 preset

        // YUV 色度采样（AVIF / HEIC / JPEG 生效）。默认 auto = 4:4:4，保留 UI 文字和细线。
        ChromaSubsampling chroma = ChromaSubsampling::Auto;
    };

    // 写入 EXIF 的截图信息；时间格式为 YYYY:MM:DD HH:MM:SS。
    struct ImageMetadata {
        std::string title;
        std::string date_time;
    };

    // 紧凑 BGRA8，行间无 padding；stride = width * 4。
    struct Image {
        ByteBuffer pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        ImageMetadata metadata;
    };

    // HDR 捕获的 FP16 原始帧；pq=true 表示码值是 BT.2020 PQ，否则是 scRGB 线性。
    // bytes_per_pixel 用于校验源格式：`hdr_to_rgba10` 只接受 FP16 RGBA（8 字节/像素）。
    struct HdrFrame {
        std::uint8_t const* bytes = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        std::uint32_t bytes_per_pixel = 8;
        bool pq = false;
        std::uint32_t sdr_white_nits = 80;
        ImageMetadata metadata;
    };

}  // namespace gamesnap
