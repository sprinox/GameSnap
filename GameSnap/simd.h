#pragma once

#include <cstddef>
#include <cstdint>

namespace gamesnap {

    // FP16 → FP32。F16C（AVX2 平台默认可用）一次转换 8 个 half。
    void half_to_float_row(std::uint16_t const* src, float* dst, std::size_t count);

    // FP32 → FP16（就近舍入，溢出按 half 的 inf 处理）。
    void float_to_half_row(float const* src, std::uint16_t* dst, std::size_t count);

    // BGRA8 行内把 alpha 置为 255。
    void bgra_set_alpha_row(std::uint8_t* pixels, std::size_t count);

    // 复制 BGRA 行并把目标 alpha 置为 255（源读、目标写各一趟）。
    void bgra_copy_set_alpha_row(std::uint8_t const* src, std::uint8_t* dst, std::size_t count);

    // BGRA8 / RGBA8 行 → R/G/B 三个平面行（HEIC 8-bit）。
    void bgra_to_rgb_planes_row(std::uint8_t const* src, std::uint8_t* r, std::uint8_t* g,
                                std::uint8_t* b, std::size_t pixels);
    void rgba_to_rgb_planes_row(std::uint8_t const* src, std::uint8_t* r, std::uint8_t* g,
                                std::uint8_t* b, std::size_t pixels);

    // BGRA8 → 紧凑 RGB8（丢掉 alpha）。AVX2 一次 8 像素，尾部走标量。
    void bgra_to_rgb_row(std::uint8_t const* src, std::uint8_t* dst, std::size_t pixels);
    // BGRA8 → 紧凑 BGR8（丢掉 alpha，保持 BMP 需要的字节序）。
    void bgra_to_bgr_row(std::uint8_t const* src, std::uint8_t* dst, std::size_t pixels);
    // RGBA16 → 紧凑 RGB16（丢掉 alpha），每像素 6 字节。AVX2 一次 4 像素。
    void rgba16_to_rgb16_row(std::uint16_t const* src, std::uint16_t* dst, std::size_t pixels);

    // 10-bit（0..1023，越界会被钳位）→ 8-bit 的按比例缩放。
    void rgba10_to_rgba8(std::uint16_t const* src, std::uint8_t* dst, std::size_t count);

}  // namespace gamesnap
