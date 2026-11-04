#include "pch.h"

#include "tonemap.h"
#include "tonemap_cs.h"
#include "d3d.h"
#include <cstring>

namespace gamesnap {

    namespace {

        struct Constants {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            float sdr_white = 80.0F;
            float peak_nits = 1000.0F;
            std::uint32_t pq = 0;
            std::uint32_t algorithm = 0;
            float pad0 = 0.0F;
            float pad1 = 0.0F;
        };
        static_assert(sizeof(Constants) % 16 == 0);

    }  // namespace

    std::string_view to_string(ToneMapAlgorithm algorithm) {
        switch (algorithm) {
        case ToneMapAlgorithm::Spline:
            return "spline";
        case ToneMapAlgorithm::Lilium:
            return "lilium";
        }
        return "unknown";
    }

    std::optional<ToneMapAlgorithm> parse_tone_map_algorithm(std::string_view text) {
        if (text == "spline")
            return ToneMapAlgorithm::Spline;
        if (text == "lilium")
            return ToneMapAlgorithm::Lilium;
        return std::nullopt;
    }

    ToneMapper::ToneMapper(std::shared_ptr<D3DContext> d3d) : d3d_(std::move(d3d)) {
        if (!d3d_ || d3d_->device() == nullptr) {
            error_ = "D3D 设备为空";
            return;
        }

        if (FAILED(d3d_->device()->CreateComputeShader(kTonemapCS, sizeof(kTonemapCS), nullptr, shader_.put()))) {  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            error_ = "CreateComputeShader 失败";
            return;
        }

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Constants);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(d3d_->device()->CreateBuffer(&desc, nullptr, constants_.put()))) {
            error_ = "CreateBuffer(constants) 失败";
            return;
        }

        D3D11_QUERY_DESC query_desc{};
        query_desc.Query = D3D11_QUERY_EVENT;
        if (FAILED(d3d_->device()->CreateQuery(&query_desc, event_query_.put()))) {
            error_ = "CreateQuery(EVENT) 失败";
            return;
        }
        ok_ = true;
    }

    ToneMapper::~ToneMapper() = default;

    void ToneMapper::release_resources() {
        input_view_ = nullptr;
        input_ = nullptr;
        output_view_ = nullptr;
        output_ = nullptr;
        readback_ = nullptr;
        hdr_output_view_ = nullptr;
        hdr_output_ = nullptr;
        hdr_readback_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

    bool ToneMapper::ensure_size(std::uint32_t width, std::uint32_t height) {
        if (width_ == width && height_ == height && input_ && output_ && readback_ && hdr_output_ && hdr_readback_)
            return true;
        if (width == 0 || height == 0) {
            error_ = "空纹理";
            return false;
        }
        release_resources();

        D3D11_TEXTURE2D_DESC input_desc{};
        input_desc.Width = width;
        input_desc.Height = height;
        input_desc.MipLevels = 1;
        input_desc.ArraySize = 1;
        input_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        input_desc.SampleDesc.Count = 1;
        input_desc.Usage = D3D11_USAGE_DEFAULT;
        input_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d3d_->device()->CreateTexture2D(&input_desc, nullptr, input_.put()))) {
            error_ = "CreateTexture2D(input) 失败";
            return false;
        }
        if (FAILED(d3d_->device()->CreateShaderResourceView(input_.get(), nullptr, input_view_.put()))) {
            error_ = "CreateShaderResourceView 失败";
            return false;
        }

        D3D11_TEXTURE2D_DESC output_desc = input_desc;
        output_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        output_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(d3d_->device()->CreateTexture2D(&output_desc, nullptr, output_.put()))) {
            error_ = "CreateTexture2D(output) 失败";
            return false;
        }
        if (FAILED(d3d_->device()->CreateUnorderedAccessView(output_.get(), nullptr, output_view_.put()))) {
            error_ = "CreateUnorderedAccessView 失败";
            return false;
        }

        D3D11_TEXTURE2D_DESC read_desc = input_desc;
        read_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        read_desc.Usage = D3D11_USAGE_STAGING;
        read_desc.BindFlags = 0;
        read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d_->device()->CreateTexture2D(&read_desc, nullptr, readback_.put()))) {
            error_ = "CreateTexture2D(readback) 失败";
            return false;
        }

        D3D11_TEXTURE2D_DESC hdr_desc = input_desc;
        hdr_desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
        hdr_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(d3d_->device()->CreateTexture2D(&hdr_desc, nullptr, hdr_output_.put()))) {
            error_ = "CreateTexture2D(hdr output) 失败";
            return false;
        }
        if (FAILED(d3d_->device()->CreateUnorderedAccessView(hdr_output_.get(), nullptr, hdr_output_view_.put()))) {
            error_ = "CreateUnorderedAccessView(hdr) 失败";
            return false;
        }
        D3D11_TEXTURE2D_DESC hdr_read_desc = hdr_desc;
        hdr_read_desc.Usage = D3D11_USAGE_STAGING;
        hdr_read_desc.BindFlags = 0;
        hdr_read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d_->device()->CreateTexture2D(&hdr_read_desc, nullptr, hdr_readback_.put()))) {
            error_ = "CreateTexture2D(hdr readback) 失败";
            return false;
        }

        width_ = width;
        height_ = height;
        return true;
    }

    bool ToneMapper::map(ID3D11Texture2D* hdr_texture,
                         DXGI_COLOR_SPACE_TYPE color_space,
                         std::uint32_t sdr_white_nits,
                         ToneMapAlgorithm algorithm,
                         Image& out,
                         std::vector<std::uint16_t>& hdr10,
                         std::string& error) {
        if (!ok_ || !hdr_texture || !d3d_) {
            error = error_.empty() ? "ToneMapper 未初始化" : error_;
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        hdr_texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            error = "色调映射需要 FP16 纹理";
            return false;
        }
        if (!ensure_size(desc.Width, desc.Height)) {
            error = error_;
            return false;
        }

        Constants constants{};
        constants.width = desc.Width;
        constants.height = desc.Height;
        constants.sdr_white = static_cast<float>(std::max<std::uint32_t>(sdr_white_nits, 1));
        constants.peak_nits = 1000.0F;
        constants.pq = (color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                        color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020)
                           ? 1U
                           : 0U;
        constants.algorithm = algorithm == ToneMapAlgorithm::Lilium ? 1U : 0U;

        // 先分配好 CPU 缓冲，再做 Map；避免 Map 成功后 resize 失败留下 mapped 纹理。
        out.width = desc.Width;
        out.height = desc.Height;
        out.stride = desc.Width * 4;
        out.pixels.resize(static_cast<std::size_t>(out.stride) * desc.Height);
        hdr10.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        D3D11_MAPPED_SUBRESOURCE hdr_mapped{};
        {
            std::scoped_lock lock(d3d_->context_mutex());
            auto* context = d3d_->context();
            context->CopyResource(input_.get(), hdr_texture);
            context->UpdateSubresource(constants_.get(), 0, nullptr, &constants, 0, 0);
            context->CSSetShader(shader_.get(), nullptr, 0);
            ID3D11Buffer* cbuf = constants_.get();
            context->CSSetConstantBuffers(0, 1, &cbuf);
            ID3D11ShaderResourceView* srv = input_view_.get();
            context->CSSetShaderResources(0, 1, &srv);
            ID3D11UnorderedAccessView* uav = output_view_.get();
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ID3D11UnorderedAccessView* hdr_uav = hdr_output_view_.get();
            context->CSSetUnorderedAccessViews(1, 1, &hdr_uav, nullptr);
            context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);

            ID3D11ShaderResourceView* null_srv = nullptr;
            std::array<ID3D11UnorderedAccessView*, 2> null_uavs{};
            context->CSSetShaderResources(0, 1, &null_srv);
            context->CSSetUnorderedAccessViews(0, 2, null_uavs.data(), nullptr);
            context->CopyResource(readback_.get(), output_.get());
            context->CopyResource(hdr_readback_.get(), hdr_output_.get());
            context->End(event_query_.get());
        }

        // 等 GPU 完成，但绝不持着 context 锁等：Map 是同步阻塞的，持锁会把
        // capture 回调和其他编码线程一起卡住。设备挂起但未 TDR 时必须超时回退。
        const auto wait_deadline = SteadyClock::now() + std::chrono::seconds(5);
        for (;;) {
            HRESULT ready = E_FAIL;
            {
                std::scoped_lock lock(d3d_->context_mutex());
                ready = d3d_->context()->GetData(event_query_.get(), nullptr, 0, 0);
            }
            if (ready == S_OK)
                break;
            if (ready != S_FALSE) {
                error = "等待 tone map GPU 完成失败";
                return false;
            }
            if (SteadyClock::now() >= wait_deadline) {
                error = "等待 tone map GPU 超时";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        {
            std::scoped_lock lock(d3d_->context_mutex());
            auto* context = d3d_->context();
            if (FAILED(context->Map(readback_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                error = "Map tone map 输出失败";
                return false;
            }
            if (FAILED(context->Map(hdr_readback_.get(), 0, D3D11_MAP_READ, 0, &hdr_mapped))) {
                context->Unmap(readback_.get(), 0);
                error = "Map tone map HDR 输出失败";
                return false;
            }
        }

        for (std::uint32_t y = 0; y < desc.Height; ++y) {
            auto const* src = static_cast<std::uint8_t const*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            auto* dst = out.pixels.data() + static_cast<std::size_t>(y) * out.stride;
            // shader 已经按 BGRA 写出且 alpha=255，这里只需整行复制。
            std::memcpy(dst, src, static_cast<std::size_t>(desc.Width) * 4);
            auto const* src_hdr =
                static_cast<std::uint16_t const*>(hdr_mapped.pData) + static_cast<std::size_t>(y) * (hdr_mapped.RowPitch / 2);
            auto* dst_hdr = hdr10.data() + static_cast<std::size_t>(y) * desc.Width * 4;
            std::memcpy(dst_hdr, src_hdr, static_cast<std::size_t>(desc.Width) * 4 * sizeof(std::uint16_t));
        }

        {
            std::scoped_lock lock(d3d_->context_mutex());
            d3d_->context()->Unmap(readback_.get(), 0);
            d3d_->context()->Unmap(hdr_readback_.get(), 0);
        }
        return true;
    }

}  // namespace gamesnap
