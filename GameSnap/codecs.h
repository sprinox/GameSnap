#pragma once

#include <string>
#include <vector>

#include "image.h"

namespace gamesnap {

    // 所有编码函数都走内存缓冲（大图写盘也集中一次），失败时 error 带原因。
    bool encode_bmp(std::filesystem::path const& path, Image const& image, std::string& error);
    bool encode_png(std::filesystem::path const& path, Image const& image, std::string& error);
    bool encode_jpeg(std::filesystem::path const& path, Image const& image, std::string& error);
    bool encode_webp(std::filesystem::path const& path, Image const& image, std::string& error);
    bool encode_avif(std::filesystem::path const& path, Image const& image, std::string& error);
    bool encode_heic(std::filesystem::path const& path, Image const& image, std::string& error);

    // HDR：10-bit PQ / BT.2020，AVIF 走无损，HEIC 走最高质量有损。
    bool encode_avif_hdr(std::filesystem::path const& path, HdrFrame const& frame, std::string& error);
    bool encode_heic_hdr(std::filesystem::path const& path, HdrFrame const& frame, std::string& error);

    // GPU 已经算好的 10-bit PQ / BT.2020 RGBA（每通道 0..1023），直接封装。
    bool encode_avif_hdr10(std::filesystem::path const& path, std::uint16_t const* rgba10,
                           std::uint32_t width, std::uint32_t height, std::string& error);
    bool encode_heic_hdr10(std::filesystem::path const& path, std::uint16_t const* rgba10,
                           std::uint32_t width, std::uint32_t height, std::string& error);

    // SDR AVIF 内嵌 gain map，alt 是同一帧的 10-bit PQ / BT.2020 HDR 像素。
    bool encode_avif_gainmap(std::filesystem::path const& path, Image const& sdr,
                             std::uint16_t const* hdr10,
                             std::uint32_t width, std::uint32_t height, std::string& error);

    bool write_sdr(std::filesystem::path const& path, ImageFormat format, Image const& image, std::string& error);
    bool write_hdr(std::filesystem::path const& path, ImageFormat format, HdrFrame const& frame, std::string& error);
    bool write_hdr10(std::filesystem::path const& path, ImageFormat format, std::uint16_t const* rgba10,
                     std::uint32_t width, std::uint32_t height, std::string& error);

}  // namespace gamesnap
