#include "pch.h"

#include <immintrin.h>

#include "simd.h"

namespace gamesnap {

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

        std::uint16_t float_to_half_scalar(float value) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            const std::uint32_t sign = (bits >> 16) & 0x8000U;
            const std::uint32_t raw_exponent = (bits >> 23) & 0xFFU;
            std::uint32_t mantissa = bits & 0x7FFFFFU;
            if (raw_exponent == 0xFFU)  // inf / NaN
                return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa != 0 ? 0x200U : 0U));
            const int exponent = static_cast<int>(raw_exponent) - 127 + 15;
            if (exponent >= 31)  // 溢出成 inf
                return static_cast<std::uint16_t>(sign | 0x7C00U);
            if (exponent <= 0) {  // 次正规数或下溢
                if (exponent < -10)
                    return static_cast<std::uint16_t>(sign);
                mantissa |= 0x800000U;
                const auto shift = static_cast<std::uint32_t>(14 - exponent);
                return static_cast<std::uint16_t>(sign | (mantissa >> shift));
            }
            return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) |
                                              (mantissa >> 13));
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

    void float_to_half_row(float const* src, std::uint16_t* dst, std::size_t count) {
        std::size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m256 v = _mm256_loadu_ps(src + i);
            const __m128i half = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), half);
        }
        for (; i < count; ++i)
            dst[i] = float_to_half_scalar(src[i]);
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

    // ── 4→3 通道打包（丢 alpha） ───────────────────────────────────────────
    // AVX2 没有“按 3 字节紧凑”的指令，做法是：先在 128-bit 通道内用 shuffle 把
    // 每 4 像素（8-bit）或 2 像素（16-bit）的 RGB 排成 12 字节，正好 3 个 dword；
    // 再用 permutevar8x32 把两个通道的 3 个 dword 拼到低 6 个 dword，写 24 字节。
    namespace {

        // 8-bit：每个 128-bit 通道 4 像素 → 12 字节；16-bit：每个通道 2 像素 → 12 字节。
        const __m256i kPack8 = _mm256_setr_epi8(
            2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1,
            2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1);
        const __m256i kPack16 = _mm256_setr_epi8(
            0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, -1, -1, -1, -1,
            0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, -1, -1, -1, -1);
        // 两个通道各 3 个 dword → 低 6 个 dword 连续。
        const __m256i kPackPerm = _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 0, 0);

        inline void store24(std::uint8_t* dst, __m256i v) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(v));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 16), _mm256_extracti128_si256(v, 1));
        }

        void bgra_to_rgb_row_scalar(std::uint8_t const* __restrict src, std::uint8_t* __restrict dst,
                                    std::size_t pixels) {
            for (std::size_t i = 0; i < pixels; ++i, src += 4, dst += 3) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
            }
        }

        void bgra_to_rgb_row_avx2(std::uint8_t const* __restrict src, std::uint8_t* __restrict dst,
                                  std::size_t pixels) {
            std::size_t x = 0;
            for (; x + 8 <= pixels; x += 8) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + x * 4));
                const __m256i packed = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(v, kPack8), kPackPerm);
                store24(dst + x * 3, packed);
            }
            bgra_to_rgb_row_scalar(src + x * 4, dst + x * 3, pixels - x);
        }

        void rgba16_to_rgb16_row_scalar(std::uint16_t const* __restrict src, std::uint16_t* __restrict dst,
                                        std::size_t pixels) {
            for (std::size_t i = 0; i < pixels; ++i, src += 4, dst += 3) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
        }

        void rgba16_to_rgb16_row_avx2(std::uint16_t const* __restrict src, std::uint16_t* __restrict dst,
                                      std::size_t pixels) {
            std::size_t x = 0;
            for (; x + 4 <= pixels; x += 4) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + x * 4));
                const __m256i packed = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(v, kPack16), kPackPerm);
                store24(reinterpret_cast<std::uint8_t*>(dst + x * 3), packed);
            }
            rgba16_to_rgb16_row_scalar(src + x * 4, dst + x * 3, pixels - x);
        }

    }  // namespace

    void bgra_to_rgb_row(std::uint8_t const* src, std::uint8_t* dst, std::size_t pixels) {
        bgra_to_rgb_row_avx2(src, dst, pixels);
    }

    void rgba16_to_rgb16_row(std::uint16_t const* src, std::uint16_t* dst, std::size_t pixels) {
        rgba16_to_rgb16_row_avx2(src, dst, pixels);
    }

    // ── 10-bit → 8-bit（PQ 8-bit 回退用） ────────────────────────────────────
    namespace {

        // AVX2：每次 16 个 uint16 → 16 个 uint8。
        // 注意 packus_epi32 在 256 位下按 128 位 lane 打包，顺序会错，所以要 permute4x64 重排；
        // 重排后每 16 位元素只有低字节有效，再用 shuffle_epi8 抽出 16 个字节拼成一次 16B 写。
        void rgba10_to_rgba8_avx2(std::uint16_t const* __restrict src, std::uint8_t* __restrict dst,
                                  std::size_t count) {
            const __m256i kMax = _mm256_set1_epi32(1023);
            const __m256 kInv = _mm256_set1_ps(1.0F / 1023.0F);
            // 每个 16 位元素取低字节（每 lane 8 个）。
            const __m256i kBytePick = _mm256_setr_epi8(
                0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1,
                0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
            std::size_t i = 0;
            for (; i + 16 <= count; i += 16) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + i));
                const auto convert = [&](__m256i xi) -> __m256i {
                    const __m256 x = _mm256_cvtepi32_ps(_mm256_min_epu32(xi, kMax));
                    // 标量是 floor((v*255+511)/1023)。float 实际误差约 3e-5，
                    // 远小于与最近整数边界的距离 1/1023 ≈ 9.8e-4；且全部 1024 个输入
                    // 都已逐值与标量结果对齐过，所以这里取 floor 与整数除法一致。
                    const __m256 scaled =
                        _mm256_mul_ps(_mm256_add_ps(_mm256_mul_ps(x, _mm256_set1_ps(255.0F)),
                                                    _mm256_set1_ps(511.0F)),
                                      kInv);
                    return _mm256_cvttps_epi32(_mm256_floor_ps(scaled));
                };
                const __m256i lo = convert(_mm256_cvtepu16_epi32(_mm256_castsi256_si128(v)));
                const __m256i hi = convert(_mm256_cvtepu16_epi32(_mm256_extracti128_si256(v, 1)));
                __m256i words = _mm256_packus_epi32(lo, hi);
                words = _mm256_permute4x64_epi64(words, _MM_SHUFFLE(3, 1, 2, 0));
                const __m256i bytes = _mm256_shuffle_epi8(words, kBytePick);
                const __m128i packed = _mm_unpacklo_epi64(_mm256_castsi256_si128(bytes),
                                                          _mm256_extracti128_si256(bytes, 1));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), packed);
            }
            for (; i < count; ++i) {
                // 尾部标量：(v * 255 + 511) / 1023，与向量路径同一条公式（越界先钳位）。
                dst[i] = static_cast<std::uint8_t>(
                    (std::min<std::uint32_t>(src[i], 1023U) * 255U + 511U) / 1023U);
            }
        }

    }  // namespace

    void rgba10_to_rgba8(std::uint16_t const* src, std::uint8_t* dst, std::size_t count) {
        rgba10_to_rgba8_avx2(src, dst, count);
    }

    // BMP 的像素字节序是 B,G,R：不能复用 bgra_to_rgb_row，否则红蓝颠倒。
    // 和 bgra_to_rgb_row 同一套打包手法，只是掩码保留 B,G,R 原序。
    void bgra_to_bgr_row(std::uint8_t const* __restrict src, std::uint8_t* __restrict dst,
                         std::size_t pixels) {
        const __m256i pack = _mm256_setr_epi8(
            0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1,
            0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
        std::size_t x = 0;
        for (; x + 8 <= pixels; x += 8) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(src + x * 4));
            store24(dst + x * 3, _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(v, pack), kPackPerm));
        }
        for (; x < pixels; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }

}  // namespace gamesnap
