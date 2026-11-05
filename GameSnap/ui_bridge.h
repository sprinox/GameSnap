#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "image.h"

namespace gamesnap {

    // capture/input → overlay 的轻量通知桥。
    // 不能把 Image / D3D 资源直接交给 UI 线程；这里只复制文件名和小缩略图。
    class UiBridge {
    public:
        struct Snapshot {
            std::uint64_t trigger_serial = 0;
            std::uint64_t info_serial = 0;
            int remaining = 0;
            std::wstring filename;
            std::vector<std::uint8_t> thumbnail;
            std::uint32_t thumb_width = 0;
            std::uint32_t thumb_height = 0;
        };

        void on_trigger() noexcept;
        void job_enqueued() noexcept;
        void job_finished() noexcept;
        void publish(std::wstring filename, Image const& image);

        [[nodiscard]] Snapshot snapshot() const;

    private:
        mutable std::mutex mutex_;
        std::wstring filename_;
        std::vector<std::uint8_t> thumbnail_;
        std::uint32_t thumb_width_ = 0;
        std::uint32_t thumb_height_ = 0;
        std::uint64_t info_serial_ = 0;
        std::atomic<std::uint64_t> trigger_serial_{0};
        std::atomic<int> remaining_{0};
    };

}  // namespace gamesnap
