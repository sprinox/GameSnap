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
        // sequence 是截图序号：多编码线程下完成顺序不固定，只接受不比已发布结果更旧的作业，
        // 避免旧截图的面板信息覆盖新截图。
        // image 非空时刷新缩略图（编码一开始就调，让面板立刻显示这一张）；
        // filename 留空表示"文件名还没确定"，等写盘成功后再调一次补上。
        void publish(std::uint64_t sequence, std::wstring filename, Image const* image);

        // 非 const：会在里面惰性丢弃上一张的文件名/缩略图（on_trigger() 不拿锁）。
        [[nodiscard]] Snapshot snapshot();
        [[nodiscard]] HANDLE trigger_event() const noexcept { return trigger_event_; }

    private:
        mutable std::mutex mutex_;
        std::wstring filename_;
        std::shared_ptr<const Thumbnail> thumbnail_;
        std::uint64_t info_serial_ = 0;
        std::uint64_t published_sequence_ = 0;  // 已发布的最新截图序号
        // 已经做过"惰性清空"的触发序号：on_trigger() 不拿锁，清空改在 snapshot() 里做。
        std::uint64_t cleared_trigger_ = 0;
        std::atomic<std::uint64_t> trigger_serial_{0};
        HANDLE trigger_event_ = nullptr;
        std::atomic<int> remaining_{0};
    };

}  // namespace gamesnap
