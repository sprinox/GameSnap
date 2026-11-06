#include "pch.h"

#include "ui_bridge.h"

#include <algorithm>

namespace gamesnap {
    namespace {

        constexpr std::uint32_t kThumbWidth = 320;
        constexpr std::uint32_t kThumbHeight = 180;

        void make_thumbnail(Image const& image, std::vector<std::uint8_t>& out,
                            std::uint32_t& width, std::uint32_t& height) {
            out.clear();
            width = 0;
            height = 0;
            if (image.width == 0 || image.height == 0 || image.stride < image.width * 4 ||
                image.pixels.size() < static_cast<std::size_t>(image.stride) * image.height) {
                return;
            }

            width = kThumbWidth;
            height = kThumbHeight;
            out.resize(static_cast<std::size_t>(width) * height * 4);
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint32_t sy = std::min(image.height - 1, y * image.height / height);
                auto const* src = image.pixels.data() + static_cast<std::size_t>(sy) * image.stride;
                auto* dst = out.data() + static_cast<std::size_t>(y) * width * 4;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t sx = std::min(image.width - 1, x * image.width / width);
                    const auto* pixel = src + static_cast<std::size_t>(sx) * 4;
                    dst[x * 4 + 0] = pixel[0];
                    dst[x * 4 + 1] = pixel[1];
                    dst[x * 4 + 2] = pixel[2];
                    dst[x * 4 + 3] = 255;
                }
            }
        }

    }  // namespace

    UiBridge::UiBridge() : trigger_event_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    UiBridge::~UiBridge() {
        if (trigger_event_ != nullptr)
            ::CloseHandle(trigger_event_);
    }

    void UiBridge::on_trigger() noexcept {
        {
            // 新截图还没有文件：立刻丢弃上一张的名字和缩略图，避免面板在 publish() 之前
            // 继续显示旧文件名。trigger_serial 随后自增，overlay 靠它判断是新的一次触发。
            std::scoped_lock lock(mutex_);
            filename_.clear();
            thumbnail_.reset();
            ++info_serial_;
        }
        trigger_serial_.fetch_add(1, std::memory_order_release);
        if (trigger_event_ != nullptr)
            ::SetEvent(trigger_event_);
    }

    void UiBridge::job_enqueued() noexcept {
        remaining_.fetch_add(1, std::memory_order_acq_rel);
    }

    void UiBridge::job_finished() noexcept {
        int value = remaining_.load(std::memory_order_acquire);
        while (value > 0 && !remaining_.compare_exchange_weak(value, value - 1,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
        }
    }

    void UiBridge::publish(std::uint64_t sequence, std::wstring filename, Image const& image) {
        // 缩略图在锁外生成；只有序号不比已发布结果旧的作业才会落进面板，
        // 这样多编码线程乱序完成时，旧截图不会覆盖新截图的显示。
        auto thumbnail = std::make_shared<Thumbnail>();
        make_thumbnail(image, thumbnail->pixels, thumbnail->width, thumbnail->height);

        std::scoped_lock lock(mutex_);
        if (sequence < published_sequence_)
            return;
        published_sequence_ = sequence;
        filename_ = std::move(filename);
        thumbnail_ = std::move(thumbnail);
        ++info_serial_;
    }

    UiBridge::Snapshot UiBridge::snapshot() const {
        Snapshot result;
        result.trigger_serial = trigger_serial_.load(std::memory_order_acquire);
        result.remaining = remaining_.load(std::memory_order_acquire);
        {
            std::scoped_lock lock(mutex_);
            result.info_serial = info_serial_;
            result.filename = filename_;
            result.thumbnail = thumbnail_;
        }
        return result;
    }

}  // namespace gamesnap
