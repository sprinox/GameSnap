#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gamesnap {

    // capture/input → overlay 的轻量通知桥。
    // 不把整帧 Image / 捕获设备的 D3D 资源交给 UI；小缩略图以不可变 shared_ptr 传递。
    class UiBridge {
    public:
        UiBridge();
        ~UiBridge();
        UiBridge(UiBridge const&) = delete;
        UiBridge& operator=(UiBridge const&) = delete;
        UiBridge(UiBridge&&) = delete;
        UiBridge& operator=(UiBridge&&) = delete;

        struct Thumbnail {
            // SDR: RGBA8/sRGB；HDR: RGBA16F/线性 scRGB（1.0 = 80 nit），不经过 tone map。
            std::vector<std::uint8_t> pixels;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            bool hdr = false;
            std::uint32_t sdr_white_nits = 80;  // HDR 表面上的普通 UI 白点，不改变缩略图亮度
        };

        struct Snapshot {
            std::uint64_t trigger_serial = 0;
            std::uint64_t info_serial = 0;
            int remaining = 0;
            std::wstring filename;
            std::shared_ptr<const Thumbnail> thumbnail;
        };

        [[nodiscard]] std::uint64_t on_trigger() noexcept;
        void job_enqueued() noexcept;
        void job_finished() noexcept;
        // 回调与编码线程分别发布，任何顺序都不能清掉同一请求的另一半信息。
        [[nodiscard]] bool is_current(std::uint64_t sequence) const noexcept;
        void publish_filename(std::uint64_t sequence, std::wstring filename);
        void publish_thumbnail(std::uint64_t sequence, std::shared_ptr<const Thumbnail> thumbnail);

        // 非 const：会在里面惰性丢弃上一张的文件名/缩略图（on_trigger() 不拿锁）。
        [[nodiscard]] Snapshot snapshot();
        // 触发、发布、剩余数变化都会唤醒同一个 overlay 消费者。
        [[nodiscard]] HANDLE trigger_event() const noexcept { return trigger_event_; }

    private:
        bool prepare_publish_locked(std::uint64_t sequence);
        void notify() noexcept;
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
