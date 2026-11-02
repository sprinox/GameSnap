#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 编码侧：多线程 BMP 写盘（目前只支持 BMP，PNG/JPEG 留作 TODO）。
//
// GPU 拷贝（CopyResource）在回调里做；Map/Unmap 这种 GPU→CPU 传输在编码线程里做。
// ─────────────────────────────────────────────────────────────────────────────

#include <filesystem>
#include <thread>

#include "common.h"
#include "messages.h"

namespace gamesnap {

    // 创建一块 CPU 可读的 staging 纹理（失败返回空）。
    std::shared_ptr<ID3D11Texture2D> create_staging_texture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc);

    class EncodeWorkers {
    public:
        EncodeWorkers(std::filesystem::path output_dir, std::shared_ptr<EncodeQueue> jobs);
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
        void encode(EncodeJob& job) const;

        std::filesystem::path output_dir_;
        std::shared_ptr<EncodeQueue> jobs_;
        std::vector<std::thread> threads_;
    };

}  // namespace gamesnap
