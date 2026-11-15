#include "pch.h"

#include "thumbnail.h"

#include <algorithm>
#include <cstring>

#include "thumbnail_cs.h"

namespace gamesnap {

    std::shared_ptr<const UiBridge::Thumbnail> make_capture_thumbnail(
        D3DContext& d3d, ID3D11Texture2D* source, std::uint32_t sdr_white_nits, std::string& error) {
        if (!source) {
            error = "缩略图源为空";
            return {};
        }
        D3D11_TEXTURE2D_DESC input_desc{};
        source->GetDesc(&input_desc);
        const bool hdr = input_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if ((!hdr && input_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             input_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
            input_desc.Width == 0 || input_desc.Height == 0 || input_desc.SampleDesc.Count != 1 ||
            input_desc.ArraySize != 1 || input_desc.MipLevels != 1) {
            error = "缩略图源格式/尺寸不支持";
            return {};
        }
        const auto fail = [&](HRESULT hr, char const* operation) {
            if (FAILED(hr))
                error = fmt::format("缩略图 {}: {}", operation, describe_hresult(hr));
            return FAILED(hr);
        };
        auto* device = d3d.device();
        winrt::com_ptr<ID3D11Texture2D> readable;
        winrt::com_ptr<ID3D11ShaderResourceView> source_view;
        // HDR 直接复用捕获的 DEFAULT+SRV；SDR 优先读 WGC 源，不能建 SRV 才临时复制。
        if (!(input_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            FAILED(device->CreateShaderResourceView(source, nullptr, source_view.put()))) {
            source_view = nullptr;
            auto desc = input_desc;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = desc.MiscFlags = 0;
            if (fail(device->CreateTexture2D(&desc, nullptr, readable.put()), "创建可采样源") ||
                fail(device->CreateShaderResourceView(readable.get(), nullptr, source_view.put()), "源 SRV"))
                return {};
        }
        constexpr std::uint32_t width = 320, height = 180;
        struct Parameters {
            std::uint32_t source_width, source_height;
            std::uint32_t draw_width, draw_height;
            std::uint32_t offset_x, offset_y;
            std::uint32_t hdr;
            float ui_white_scale;
        };
        static_assert(sizeof(Parameters) == 32);
        Parameters params{input_desc.Width, input_desc.Height, width, height, 0, 0, hdr ? 1U : 0U,
                          static_cast<float>(std::max(80U, sdr_white_nits)) / 80.0F};
        if (std::uint64_t(width) * input_desc.Height <= std::uint64_t(height) * input_desc.Width)
            params.draw_height = std::max(1U, static_cast<std::uint32_t>(std::uint64_t(width) * input_desc.Height / input_desc.Width));
        else
            params.draw_width = std::max(1U, static_cast<std::uint32_t>(std::uint64_t(height) * input_desc.Width / input_desc.Height));
        params.offset_x = (width - params.draw_width) / 2;
        params.offset_y = (height - params.draw_height) / 2;
        D3D11_BUFFER_DESC cb_desc{};
        cb_desc.ByteWidth = sizeof(params);
        cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{&params, 0, 0};
        winrt::com_ptr<ID3D11Buffer> constants;
        winrt::com_ptr<ID3D11ComputeShader> shader;
        if (fail(device->CreateBuffer(&cb_desc, &initial, constants.put()), "参数缓冲") ||
            fail(device->CreateComputeShader(kThumbnailCS, sizeof(kThumbnailCS), nullptr, shader.put()), "着色器"))
            return {};

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        winrt::com_ptr<ID3D11Texture2D> output, staging;
        winrt::com_ptr<ID3D11UnorderedAccessView> output_view;
        if (fail(device->CreateTexture2D(&desc, nullptr, output.put()), "输出纹理") ||
            fail(device->CreateUnorderedAccessView(output.get(), nullptr, output_view.put()), "输出 UAV"))
            return {};
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (fail(device->CreateTexture2D(&desc, nullptr, staging.put()), "小图 staging"))
            return {};

        auto* context = d3d.context();
        {
            std::scoped_lock lock(d3d.context_mutex());
            if (readable)
                context->CopyResource(readable.get(), source);
            auto* srv = source_view.get();
            auto* uav = output_view.get();
            auto* cb = constants.get();
            context->CSSetShader(shader.get(), nullptr, 0);
            context->CSSetShaderResources(0, 1, &srv);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetConstantBuffers(0, 1, &cb);
            context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            srv = nullptr;
            uav = nullptr;
            cb = nullptr;
            context->CSSetShaderResources(0, 1, &srv);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetConstantBuffers(0, 1, &cb);
            context->CSSetShader(nullptr, nullptr, 0);
            context->CopyResource(staging.get(), output.get());
            // 下面使用非阻塞 Map + 截止时间；显式提交以保证隐藏/无 Present 的设备也会前进。
            context->Flush();
        }

        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(250);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        for (;;) {
            HRESULT hr;
            {
                std::scoped_lock lock(d3d.context_mutex());
                hr = context->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            }
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                if (SteadyClock::now() >= deadline) {
                    error = "缩略图 GPU 等待超过 250ms，跳过预览";
                    return {};
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (fail(hr, "Map"))
                return {};
            break;
        }
        auto unmap = ScopeExit{[&] {
            std::scoped_lock lock(d3d.context_mutex());
            context->Unmap(staging.get(), 0);
        }};
        auto thumbnail = std::make_shared<UiBridge::Thumbnail>();
        thumbnail->width = width;
        thumbnail->height = height;
        thumbnail->hdr = hdr;
        thumbnail->sdr_white_nits = sdr_white_nits;
        const std::size_t stride = width * (hdr ? 8U : 4U);
        thumbnail->pixels.resize(stride * height);
        for (std::size_t y = 0; y < height; ++y)
            std::memcpy(thumbnail->pixels.data() + y * stride,
                        static_cast<std::uint8_t const*>(mapped.pData) + y * mapped.RowPitch, stride);
        return thumbnail;
    }

}  // namespace gamesnap
