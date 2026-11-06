#pragma once

#include <memory>
#include <string>

#include "common.h"
#include <d3d11_4.h>
#include "image.h"

namespace gamesnap {

    class D3DContext;

    std::string_view to_string(ToneMapAlgorithm algorithm);
    std::optional<ToneMapAlgorithm> parse_tone_map_algorithm(std::string_view text);

    // 在 D3D11 compute shader 里完成色调映射。
    // 每个 HDR job 临时创建一个实例（编码线程之间不共享），因此内部不需要锁；
    // ensure_size() 的缓存只在同一个实例内有效。
    class ToneMapper {
    public:
        explicit ToneMapper(std::shared_ptr<D3DContext> d3d);
        ~ToneMapper();
        ToneMapper(ToneMapper const&) = delete;
        ToneMapper& operator=(ToneMapper const&) = delete;
        ToneMapper(ToneMapper&&) = delete;
        ToneMapper& operator=(ToneMapper&&) = delete;

        [[nodiscard]] bool ok() const noexcept { return ok_; }
        [[nodiscard]] std::string const& init_error() const noexcept { return error_; }

        bool map(ID3D11Texture2D* hdr_texture,
                 DXGI_COLOR_SPACE_TYPE color_space,
                 std::uint32_t sdr_white_nits,
                 ToneMapAlgorithm algorithm,
                 Image& out,
                 std::vector<std::uint16_t>& hdr10,
                 std::string& error);

    private:
        bool ensure_size(std::uint32_t width, std::uint32_t height);
        void release_resources();

        std::shared_ptr<D3DContext> d3d_;
        winrt::com_ptr<ID3D11ComputeShader> shader_;
        winrt::com_ptr<ID3D11Buffer> constants_;
        winrt::com_ptr<ID3D11Fence> fence_;
        winrt::com_ptr<ID3D11DeviceContext4> context4_;
        std::uint64_t fence_value_ = 0;
        winrt::com_ptr<ID3D11Texture2D> output_;
        winrt::com_ptr<ID3D11UnorderedAccessView> output_view_;
        winrt::com_ptr<ID3D11Texture2D> readback_;
        winrt::com_ptr<ID3D11Texture2D> hdr_output_;
        winrt::com_ptr<ID3D11UnorderedAccessView> hdr_output_view_;
        winrt::com_ptr<ID3D11Texture2D> hdr_readback_;
        std::uint32_t width_ = 0;
        std::uint32_t height_ = 0;
        bool ok_ = false;
        std::string error_;
    };

}  // namespace gamesnap
