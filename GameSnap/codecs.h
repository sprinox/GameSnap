#pragma once

#include <string>
#include <vector>

#include "image.h"

namespace gamesnap {

    // 每个编码线程在进入编码前设置；底层编码器只读取本线程的选项。
    void set_codec_options(CodecOptions const& options);

    // 所有编码函数都走内存缓冲（大图写盘也集中一次），失败时 error 带原因。


    // GPU 已经算好的 10-bit PQ / BT.2020 RGBA（每通道 0..1023），直接封装。
    bool encode_avif_hdr10(std::filesystem::path const& path, std::uint16_t const* rgba10,
                           std::uint32_t width, std::uint32_t height, ImageMetadata const& metadata,
                           std::string& error);
    bool encode_heic_hdr10(std::filesystem::path const& path, std::uint16_t const* rgba10,
                           std::uint32_t width, std::uint32_t height, ImageMetadata const& metadata,
                           std::string& error);

    // SDR AVIF 内嵌 gain map，alt 是同一帧的 10-bit PQ / BT.2020 HDR 像素。
    bool encode_avif_gainmap(std::filesystem::path const& path, Image const& sdr,
                             std::uint16_t const* hdr10,
                             std::uint32_t width, std::uint32_t height, std::string& error);

    bool write_sdr(std::filesystem::path const& path, ImageFormat format, Image const& image, std::string& error);
    // rgba10_out 非空时同时返回 CPU 生成的 10-bit PQ 数据，供 SDR AVIF gain map 使用。
    bool write_hdr(std::filesystem::path const& path, ImageFormat format, HdrFrame const& frame, std::string& error,
                   U16Buffer* rgba10_out = nullptr);
    bool write_hdr10(std::filesystem::path const& path, ImageFormat format, std::uint16_t const* rgba10,
                     std::uint32_t width, std::uint32_t height, ImageMetadata const& metadata,
                     std::string& error);

}  // namespace gamesnap
