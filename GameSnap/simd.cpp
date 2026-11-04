#include "pch.h"

#include <immintrin.h>

#include "simd.h"

namespace gamesnap {

    void bgra_to_rgba_row(std::uint8_t const* src, std::uint8_t* dst, std::size_t pixels) {
        const __m256i shuffle = _mm256_setr_epi8(
            2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
            2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
        std::size_t x = 0;
        for (; x + 8 <= pixels; x += 8) {
            const __m256i bgra = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + x * 4));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x * 4), _mm256_shuffle_epi8(bgra, shuffle));
        }
        for (; x < pixels; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }

    // F16C 半精度转换；辅助标量只用于尾部。
    namespace {

        float half_to_float_scalar(std::uint16_t value) {
            const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16;
            std::uint32_t exponent = (value >> 10) & 0x1FU;
            std::uint32_t mantissa = value & 0x3FFU;
            std::uint32_t bits = 0;
            if (exponent == 0x1F) {
                bits = sign | 0x7F800000U | (mantissa << 13);
            } else if (exponent != 0) {
                bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
            } else if (mantissa == 0) {
                bits = sign;
            } else {
                exponent = 127 - 15 + 1;
                while ((mantissa & 0x400U) == 0) {
                    mantissa <<= 1;
                    --exponent;
                }
                mantissa &= 0x3FFU;
                bits = sign | (exponent << 23) | (mantissa << 13);
            }
            float result = 0.0F;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }

    }  // namespace

    void half_to_float_row(std::uint16_t const* src, float* dst, std::size_t count) {
        std::size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m128i half = _mm_loadu_si128(reinterpret_cast<__m128i const*>(src + i));
            _mm256_storeu_ps(dst + i, _mm256_cvtph_ps(half));
        }
        for (; i < count; ++i)
            dst[i] = half_to_float_scalar(src[i]);
    }

    void bgra_set_alpha_row(std::uint8_t* pixels, std::size_t count) {
        const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xFF000000U));
        std::size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            auto* p = reinterpret_cast<__m256i*>(pixels + i * 4);
            _mm256_storeu_si256(p, _mm256_or_si256(_mm256_loadu_si256(p), alpha));
        }
        for (; i < count; ++i)
            pixels[i * 4 + 3] = 255;
    }

    void bgra_copy_set_alpha_row(std::uint8_t const* src, std::uint8_t* dst, std::size_t count) {
        const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xFF000000U));
        std::size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + i * 4));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * 4), _mm256_or_si256(v, alpha));
        }
        for (; i < count; ++i) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, src + i * 4, sizeof(pixel));
            pixel |= 0xFF000000U;
            std::memcpy(dst + i * 4, &pixel, sizeof(pixel));
        }
    }

    namespace {

        void rgb_planes_row(std::uint8_t const* src, std::uint8_t* r, std::uint8_t* g,
                            std::uint8_t* b, std::size_t pixels, int r_index, int g_index, int b_index) {
            const __m256i mr = _mm256_setr_epi8(
                r_index, r_index + 4, r_index + 8, r_index + 12, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1,
                r_index, r_index + 4, r_index + 8, r_index + 12, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1);
            const __m256i mg = _mm256_setr_epi8(
                g_index, g_index + 4, g_index + 8, g_index + 12, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1,
                g_index, g_index + 4, g_index + 8, g_index + 12, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1);
            const __m256i mb = _mm256_setr_epi8(
                b_index, b_index + 4, b_index + 8, b_index + 12, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1,
                b_index, b_index + 4, b_index + 8, b_index + 12, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1);

            std::size_t x = 0;
            for (; x + 8 <= pixels; x += 8) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + x * 4));
                const auto store = [&](__m256i mask, std::uint8_t* dst) {
                    const __m256i shuffled = _mm256_shuffle_epi8(v, mask);
                    const auto lo = static_cast<std::uint32_t>(_mm256_extract_epi32(shuffled, 0));
                    const auto hi = static_cast<std::uint32_t>(_mm256_extract_epi32(shuffled, 4));
                    const std::uint64_t packed = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
                    std::memcpy(dst + x, &packed, sizeof(packed));
                };
                store(mr, r);
                store(mg, g);
                store(mb, b);
            }
            for (; x < pixels; ++x) {
                r[x] = src[x * 4 + static_cast<std::size_t>(r_index)];
                g[x] = src[x * 4 + static_cast<std::size_t>(g_index)];
                b[x] = src[x * 4 + static_cast<std::size_t>(b_index)];
            }
        }

    }  // namespace

    void bgra_to_rgb_planes_row(std::uint8_t const* src, std::uint8_t* r, std::uint8_t* g,
                                std::uint8_t* b, std::size_t pixels) {
        rgb_planes_row(src, r, g, b, pixels, 2, 1, 0);
    }

    void rgba_to_rgb_planes_row(std::uint8_t const* src, std::uint8_t* r, std::uint8_t* g,
                                std::uint8_t* b, std::size_t pixels) {
        rgb_planes_row(src, r, g, b, pixels, 0, 1, 2);
    }

}  // namespace gamesnap
