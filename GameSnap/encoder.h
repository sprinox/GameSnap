#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 编码侧：多线程编码 AVIF / HEIC / WebP / PNG / JPEG / BMP。
// HDR 使用 capture 的 DEFAULT+SRV 纹理直接做 GPU tone map；CPU fallback 才回读到 staging。
// SDR 直接 Map capture 的 staging 纹理。
// ─────────────────────────────────────────────────────────────────────────────

#include <filesystem>
#include <thread>

#include "common.h"
#include "messages.h"

namespace gamesnap {

    class D3DContext;
    class UiBridge;

    // 创建一块 CPU 可读的 staging 纹理（失败返回空）。com_ptr：独占，不可拷贝。
    winrt::com_ptr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc);

    // FP16 像素 → SDR BGRA8，走和 tonemap.hlsl 完全相同的 spline + IPT 链路。
    // color_space 决定输入是 PQ 还是 scRGB（非 PQ 一律按 scRGB，1.0 = 80 nits 处理）。
    // 编码线程内部用它做 GPU 失败时的回退，基准工具也用它生成 SDR 底图。
    bool tone_map_hdr_to_sdr(std::uint16_t const* fp16, std::uint32_t width, std::uint32_t height,
                             std::uint32_t stride_bytes, DXGI_COLOR_SPACE_TYPE color_space,
                             std::uint32_t sdr_white_nits, Image& out);

    class EncodeWorkers {
    public:
        EncodeWorkers(std::filesystem::path output_dir, std::shared_ptr<EncodeQueue> jobs,
                      CodecOptions options, ToneMapBackend tone_map_backend, std::wstring filename_pattern,
                      std::shared_ptr<UiBridge> ui);
        ~EncodeWorkers();

        EncodeWorkers(const EncodeWorkers&) = delete;
        EncodeWorkers& operator=(const EncodeWorkers&) = delete;
        EncodeWorkers(EncodeWorkers&&) = delete;
        EncodeWorkers& operator=(EncodeWorkers&&) = delete;

        void start(std::size_t thread_count);
        // 给每个编码线程投递停止哨兵并等待它们把在途任务落盘（必须最后调用）。
        void stop();

    private:
        void worker_main(std::size_t index);
        void encode(EncodeJob& job);
        // 包一层 try/catch：单张失败不能让整个编码线程退出。
        void encode_one(std::size_t index, EncodeJob& job);

        std::filesystem::path output_dir_;
        std::shared_ptr<EncodeQueue> jobs_;
        std::shared_ptr<UiBridge> ui_;
        CodecOptions options_;
        ToneMapBackend tone_map_backend_ = ToneMapBackend::Auto;
        std::wstring filename_pattern_;  // 文件名模板，见 Config::filename_pattern
        std::vector<std::thread> threads_;
        std::size_t worker_count_ = 0;
        std::atomic<std::size_t> stopped_{0};  // 已消费 stop 哨兵的线程数
    };

}  // namespace gamesnap
