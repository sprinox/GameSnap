#pragma once

#include <windows.h>

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
        UiBridge();
        ~UiBridge();
        UiBridge(UiBridge const&) = delete;
        UiBridge& operator=(UiBridge const&) = delete;
        UiBridge(UiBridge&&) = delete;
        UiBridge& operator=(UiBridge&&) = delete;

        struct Thumbnail {
            std::vector<std::uint8_t> pixels;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
        };

        struct Snapshot {
            std::uint64_t trigger_serial = 0;
            std::uint64_t info_serial = 0;
            int remaining = 0;
            std::wstring filename;
            std::shared_ptr<const Thumbnail> thumbnail;
        };

        void on_trigger() noexcept;
        void job_enqueued() noexcept;
        void job_finished() noexcept;
        void publish(std::wstring filename, Image const& image);

        [[nodiscard]] Snapshot snapshot() const;
        [[nodiscard]] HANDLE trigger_event() const noexcept { return trigger_event_; }

    private:
        mutable std::mutex mutex_;
        std::wstring filename_;
        std::shared_ptr<const Thumbnail> thumbnail_;
        std::uint64_t info_serial_ = 0;
        std::atomic<std::uint64_t> trigger_serial_{0};
        HANDLE trigger_event_ = nullptr;
        std::atomic<int> remaining_{0};
    };

}  // namespace gamesnap
