#include "pch.h"

#include "ui_bridge.h"

namespace gamesnap {
    UiBridge::UiBridge() : trigger_event_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    UiBridge::~UiBridge() {
        if (trigger_event_ != nullptr)
            ::CloseHandle(trigger_event_);
    }

    void UiBridge::notify() noexcept {
        if (trigger_event_ != nullptr)
            ::SetEvent(trigger_event_);
    }

    std::uint64_t UiBridge::on_trigger() noexcept {
        // 这个函数在 WH_KEYBOARD_LL 钩子回调里被调用：超过 LowLevelHooksTimeout（默认 1s）
        // 系统会静默摘掉钩子。所以这里只做递增 + SetEvent，不拿互斥量、不释放堆内存。
        // 丢弃上一张文件名/缩略图的工作挪到 overlay 线程的 snapshot() 里惰性完成。
        const auto sequence = trigger_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
        notify();
        return sequence;
    }

    void UiBridge::job_enqueued() noexcept {
        remaining_.fetch_add(1, std::memory_order_acq_rel);
        notify();
    }

    void UiBridge::job_finished() noexcept {
        int value = remaining_.load(std::memory_order_acquire);
        while (value > 0 && !remaining_.compare_exchange_weak(value, value - 1,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
        }
        notify();
    }

    bool UiBridge::is_current(std::uint64_t sequence) const noexcept {
        return sequence == trigger_serial_.load(std::memory_order_acquire);
    }

    bool UiBridge::prepare_publish_locked(std::uint64_t sequence) {
        if (sequence < published_sequence_ || !is_current(sequence))
            return false;
        if (sequence > published_sequence_) {
            filename_.clear();
            thumbnail_.reset();
        }
        published_sequence_ = sequence;
        return true;
    }

    void UiBridge::publish_filename(std::uint64_t sequence, std::wstring filename) {
        std::scoped_lock lock(mutex_);
        if (!prepare_publish_locked(sequence))
            return;
        filename_ = std::move(filename);
        ++info_serial_;
        notify();
    }

    void UiBridge::publish_thumbnail(std::uint64_t sequence, std::shared_ptr<const Thumbnail> thumbnail) {
        if (!thumbnail)
            return;
        std::scoped_lock lock(mutex_);
        if (!prepare_publish_locked(sequence))
            return;
        thumbnail_ = std::move(thumbnail);
        ++info_serial_;
        notify();
    }

    UiBridge::Snapshot UiBridge::snapshot() {
        Snapshot result;
        result.remaining = remaining_.load(std::memory_order_acquire);
        {
            std::scoped_lock lock(mutex_);
            result.trigger_serial = trigger_serial_.load(std::memory_order_acquire);
            // 惰性丢弃上一张：新触发已经发生、但还没有比它更新的结果发布出来时，
            // 清掉旧文件名/缩略图，避免面板在 publish() 之前继续显示上一张。
            // 每次触发只清一次（cleared_trigger_），否则每帧都会把 info_serial_ 顶上去。
            if (result.trigger_serial > cleared_trigger_ && published_sequence_ < result.trigger_serial) {
                cleared_trigger_ = result.trigger_serial;
                if (!filename_.empty() || thumbnail_) {
                    filename_.clear();
                    thumbnail_.reset();
                    ++info_serial_;
                }
            }
            result.info_serial = info_serial_;
            result.filename = filename_;
            result.thumbnail = thumbnail_;
        }
        return result;
    }

}  // namespace gamesnap
