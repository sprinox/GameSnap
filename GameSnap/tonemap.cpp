#include "pch.h"

#include "tonemap.h"
#include "tonemap_cs.h"
#include "tonemap_peak_cs.h"
#include "d3d.h"
#include <immintrin.h>
#include <cstring>

namespace gamesnap {

    namespace {

        // 着色器槽位号，必须和 tonemap.hlsl 里的 register(...) 对齐。
        // 曾经峰值 pass 在 HLSL 里写 u2、主机却绑 u0，写入被丢弃 → 峰值恒为 0 →
        // 曲线退化成"SDR 白点硬裁剪"。用常量把两边钉在一起，改的时候起码要改名字。
        constexpr UINT kPeakUavSlot = 2;  // RWStructuredBuffer<uint> PeakOut : register(u2)

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
        case ToneMapAlgorithm::Analysis:
            return "analysis";
        }
        return "unknown";
    }

    std::optional<ToneMapAlgorithm> parse_tone_map_algorithm(std::string_view text) {
        if (text == "spline")
            return ToneMapAlgorithm::Spline;
        if (text == "analysis")
            return ToneMapAlgorithm::Analysis;
        return std::nullopt;
    }

    namespace {

        void unpack_r10_scalar(std::uint32_t packed, std::uint16_t* dst) {
            dst[0] = static_cast<std::uint16_t>(packed & 0x3FFU);
            dst[1] = static_cast<std::uint16_t>((packed >> 10) & 0x3FFU);
            dst[2] = static_cast<std::uint16_t>((packed >> 20) & 0x3FFU);
            dst[3] = 0x3FF;
        }

        void unpack_r10_4(__m128i packed, std::uint16_t* dst) {
            const __m128i mask = _mm_set1_epi32(0x3FF);
            const __m128i zero = _mm_setzero_si128();
            const __m128i alpha = _mm_set1_epi16(0x3FF);
            const __m128i r32 = _mm_and_si128(packed, mask);
            const __m128i g32 = _mm_and_si128(_mm_srli_epi32(packed, 10), mask);
            const __m128i b32 = _mm_and_si128(_mm_srli_epi32(packed, 20), mask);
            const __m128i r16 = _mm_packus_epi32(r32, zero);
            const __m128i g16 = _mm_packus_epi32(g32, zero);
            const __m128i b16 = _mm_packus_epi32(b32, zero);
            const __m128i rg = _mm_unpacklo_epi16(r16, g16);
            const __m128i ba = _mm_unpacklo_epi16(b16, alpha);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_unpacklo_epi32(rg, ba));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8), _mm_unpackhi_epi32(rg, ba));
        }

        void unpack_r10_avx2(std::uint32_t const* src, std::uint16_t* dst, std::uint32_t width) {
            std::uint32_t x = 0;
            for (; x + 8 <= width; x += 8) {
                const __m256i packed = _mm256_loadu_si256(
                    reinterpret_cast<__m256i const*>(src + x));
                unpack_r10_4(_mm256_castsi256_si128(packed), dst + static_cast<std::size_t>(x) * 4);
                unpack_r10_4(_mm256_extracti128_si256(packed, 1), dst + static_cast<std::size_t>(x + 4) * 4);
            }
            for (; x < width; ++x)
                unpack_r10_scalar(src[x], dst + static_cast<std::size_t>(x) * 4);
        }

    }  // namespace

    ToneMapper::ToneMapper(std::shared_ptr<D3DContext> d3d) : d3d_(std::move(d3d)) {
        if (!d3d_ || d3d_->device() == nullptr) {
            error_ = "D3D 设备为空";
            return;
        }

        // 先把格式能力查清楚：R10G10B10A2 要能当 UAV 写（HDR 输出）且能当 SRV 采样（SDR 输出），
        // 否则后面的纹理创建会失败，只能回退 CPU 色调映射。这里给出明确原因，别让它静默降级。
        // 注意不要要求 RENDER_TARGET：两张输出纹理都只绑 UAV（BindFlags 里没有 RENDER_TARGET），
        // 多要一项会把本来能做 GPU 色调映射的显卡误判成不支持。
        UINT hdr_support = 0;
        const HRESULT support_hr =
            d3d_->device()->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &hdr_support);
        constexpr UINT kNeeded = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                                 D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
        if (FAILED(support_hr) || (hdr_support & kNeeded) != kNeeded) {
            error_ = fmt::format("该显卡不支持 10-bit UAV，HDR 走 CPU 色调映射（R10G10B10A2 支持位=0x{:X}）",
                                 hdr_support);
            return;
        }

        if (FAILED(d3d_->device()->CreateComputeShader(kTonemapCS, sizeof(kTonemapCS), nullptr, shader_.put()))) {  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            error_ = "CreateComputeShader 失败";
            return;
        }
        if (FAILED(d3d_->device()->CreateComputeShader(kPeakCS, sizeof(kPeakCS), nullptr, peak_shader_.put()))) {  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            error_ = "CreateComputeShader(peak) 失败";
            return;
        }
        if (!ensure_peak_buffer()) {
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

        winrt::com_ptr<ID3D11Device5> device5;
        winrt::com_ptr<ID3D11DeviceContext4> context4;
        if (FAILED(d3d_->device()->QueryInterface(__uuidof(ID3D11Device5), device5.put_void())) ||
            FAILED(d3d_->context()->QueryInterface(__uuidof(ID3D11DeviceContext4), context4.put_void()))) {
            error_ = "D3D11.4 fence 接口不可用";
            return;
        }
        if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), fence_.put_void()))) {
            error_ = "CreateFence 失败";
            return;
        }
        context4_ = context4;
        ok_ = true;
    }

    ToneMapper::~ToneMapper() = default;

    // 1 个 uint 的峰值缓冲：UAV 写入、SRV 读取，全程留在 GPU 上，不回读 CPU。
    bool ToneMapper::ensure_peak_buffer() {
        if (peak_buffer_)
            return true;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(std::uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(std::uint32_t);
        if (FAILED(d3d_->device()->CreateBuffer(&desc, nullptr, peak_buffer_.put()))) {
            error_ = "CreateBuffer(peak) 失败";
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.NumElements = 1;
        if (FAILED(d3d_->device()->CreateUnorderedAccessView(peak_buffer_.get(), &uav_desc, peak_uav_.put()))) {
            error_ = "CreateUnorderedAccessView(peak) 失败";
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv_desc.Buffer.NumElements = 1;
        if (FAILED(d3d_->device()->CreateShaderResourceView(peak_buffer_.get(), &srv_desc, peak_srv_.put()))) {
            error_ = "CreateShaderResourceView(peak) 失败";
            return false;
        }
        return true;
    }

    void ToneMapper::release_resources() {
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
        if (width_ == width && height_ == height && output_ && readback_ && hdr_output_ && hdr_readback_)
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
        hdr_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
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
                         U16Buffer& hdr10,
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

        winrt::com_ptr<ID3D11ShaderResourceView> input_view;
        if (FAILED(d3d_->device()->CreateShaderResourceView(hdr_texture, nullptr, input_view.put()))) {
            error = "CreateShaderResourceView(HDR 输入) 失败";
            return false;
        }

        Constants constants{};
        constants.width = desc.Width;
        constants.height = desc.Height;
        constants.sdr_white = static_cast<float>(std::max<std::uint32_t>(sdr_white_nits, 1));
        constants.peak_nits = 10000.0F;  // 实测峰值只是被它夹住的上限
        constants.pq = (color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                        color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020)
                           ? 1U
                           : 0U;
        constants.algorithm = algorithm == ToneMapAlgorithm::Analysis ? 1U : 0U;

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
            context->UpdateSubresource(constants_.get(), 0, nullptr, &constants, 0, 0);
            ID3D11Buffer* cbuf = constants_.get();
            context->CSSetConstantBuffers(0, 1, &cbuf);

            // 1) 峰值归约：先清 0，再按 8x8 线程组做两级 max，结果留在 peak_buffer_。
            //    槽位必须和 HLSL 的 register(u2) 一致，否则写入被丢弃、峰值恒为 0，
            //    色调映射会退化成"SDR 白点硬裁剪"。
            //    analysis（false color 调试）不需要峰值，省掉这一次全帧 dispatch。
            if (constants.algorithm != 0) {
                context->CSSetShader(shader_.get(), nullptr, 0);
                ID3D11ShaderResourceView* srv_only = input_view.get();
                context->CSSetShaderResources(0, 1, &srv_only);
                ID3D11UnorderedAccessView* uav_only = output_view_.get();
                context->CSSetUnorderedAccessViews(0, 1, &uav_only, nullptr);
                ID3D11UnorderedAccessView* hdr_uav_only = hdr_output_view_.get();
                context->CSSetUnorderedAccessViews(1, 1, &hdr_uav_only, nullptr);
                context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
            } else {
                const UINT clear[4]{};
                context->ClearUnorderedAccessViewUint(peak_uav_.get(), clear);
                context->CSSetShader(peak_shader_.get(), nullptr, 0);
                ID3D11ShaderResourceView* peak_in = input_view.get();
                context->CSSetShaderResources(0, 1, &peak_in);
                ID3D11UnorderedAccessView* peak_uav = peak_uav_.get();
                context->CSSetUnorderedAccessViews(kPeakUavSlot, 1, &peak_uav, nullptr);
                context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
                ID3D11UnorderedAccessView* no_uav = nullptr;
                context->CSSetUnorderedAccessViews(kPeakUavSlot, 1, &no_uav, nullptr);

                // 2) 色调映射：t1 提供本帧峰值；spline 曲线参数由 shader 在每个
                //    8x8 线程组里算一次（groupshared），不再逐像素重算支点/斜率。
                context->CSSetShader(shader_.get(), nullptr, 0);
                ID3D11ShaderResourceView* srv = input_view.get();
                context->CSSetShaderResources(0, 1, &srv);
                ID3D11ShaderResourceView* peak_srv = peak_srv_.get();
                context->CSSetShaderResources(1, 1, &peak_srv);
                ID3D11UnorderedAccessView* uav = output_view_.get();
                context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                ID3D11UnorderedAccessView* hdr_uav = hdr_output_view_.get();
                context->CSSetUnorderedAccessViews(1, 1, &hdr_uav, nullptr);
                context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
            }

            std::array<ID3D11ShaderResourceView*, 2> null_srvs{};
            std::array<ID3D11UnorderedAccessView*, 3> null_uavs{};
            context->CSSetShaderResources(0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
            context->CSSetUnorderedAccessViews(0, static_cast<UINT>(null_uavs.size()), null_uavs.data(), nullptr);
            context->CopyResource(readback_.get(), output_.get());
            context->CopyResource(hdr_readback_.get(), hdr_output_.get());
            const HRESULT signaled = context4_->Signal(fence_.get(), ++fence_value_);
            if (FAILED(signaled)) {
                error = "Signal tone map fence 失败";
                return false;
            }
        }

        // 等 GPU 完成，但绝不持着 context 锁等：Map 是同步阻塞的，持锁会把
        // capture 回调和其他编码线程一起卡住。设备挂起但未 TDR 时必须超时回退。
        const HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            error = "CreateEvent(tone map fence) 失败";
            return false;
        }
        const HRESULT registered = fence_->SetEventOnCompletion(fence_value_, fence_event);
        if (FAILED(registered)) {
            // 注册就没成功：事件上没有任何挂起引用，可以安全关闭，也不要误报成超时。
            ::CloseHandle(fence_event);
            error = fmt::format("注册 tone map fence 事件失败: {}", describe_hresult(registered));
            return false;
        }
        const DWORD waited = ::WaitForSingleObject(fence_event, 1000);
        if (waited == WAIT_TIMEOUT) {
            // 同 d3d.cpp::wait_fence：超时说明设备已挂起，fence 仍注册在这个事件上，
            // 这时 CloseHandle 会让之后 GPU 完成的 Signal 落到可能被复用的句柄值上。
            // 故意不关（job 已失败，进程很快会走 CPU 回退或退出）。
            error = "等待 tone map GPU 超时";
            return false;
        }
        ::CloseHandle(fence_event);
        if (waited != WAIT_OBJECT_0 || fence_->GetCompletedValue() < fence_value_) {
            error = fmt::format("等待 tone map GPU 失败: Win32 {}", ::GetLastError());
            return false;
        }

        {
            // 这里用阻塞 Map，但**前面已经在锁外等到 fence 完成**，所以正常情况下它立即返回。
            // 试过 D3D11_MAP_FLAG_DO_NOT_WAIT：在 NVIDIA 驱动上即使 fence 已完成也持续返回
            // DXGI_ERROR_WAS_STILL_DRAWING，导致 GPU 色调映射每次都失败回退 CPU——比它要防的
            // 问题严重得多，所以退回阻塞 Map。真正的卡死风险由上面的 fence 超时兜住。
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

        for (std::size_t y = 0; y < desc.Height; ++y) {
            auto const* src = static_cast<std::uint8_t const*>(mapped.pData) + y * mapped.RowPitch;
            auto* dst = out.pixels.data() + y * out.stride;
            // shader 已经按 BGRA 写出且 alpha=255，这里只需整行复制。
            std::memcpy(dst, src, static_cast<std::size_t>(desc.Width) * 4);
            auto const* src_hdr = reinterpret_cast<std::uint32_t const*>(
                static_cast<std::uint8_t const*>(hdr_mapped.pData) + y * hdr_mapped.RowPitch);
            auto* dst_hdr = hdr10.data() + y * desc.Width * 4;
            unpack_r10_avx2(src_hdr, dst_hdr, desc.Width);
        }

        {
            std::scoped_lock lock(d3d_->context_mutex());
            d3d_->context()->Unmap(readback_.get(), 0);
            d3d_->context()->Unmap(hdr_readback_.get(), 0);
        }
        return true;
    }

}  // namespace gamesnap
