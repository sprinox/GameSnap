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

    // 创建一块 CPU 可读的 staging 纹理（失败返回空）。
    std::shared_ptr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc);

    class EncodeWorkers {
    public:
        EncodeWorkers(std::filesystem::path output_dir, std::shared_ptr<EncodeQueue> jobs,
                      CodecOptions options, ToneMapBackend tone_map_backend,
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

        std::filesystem::path output_dir_;
        std::shared_ptr<EncodeQueue> jobs_;
        std::shared_ptr<UiBridge> ui_;
        CodecOptions options_;
        ToneMapBackend tone_map_backend_ = ToneMapBackend::Auto;
        std::vector<std::thread> threads_;
    };

}  // namespace gamesnap
