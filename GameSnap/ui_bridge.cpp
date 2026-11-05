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

    void UiBridge::on_trigger() noexcept {
        trigger_serial_.fetch_add(1, std::memory_order_release);
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

    void UiBridge::publish(std::wstring filename, Image const& image) {
        std::vector<std::uint8_t> thumbnail;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        make_thumbnail(image, thumbnail, width, height);

        std::scoped_lock lock(mutex_);
        filename_ = std::move(filename);
        thumbnail_ = std::move(thumbnail);
        thumb_width_ = width;
        thumb_height_ = height;
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
            result.thumb_width = thumb_width_;
            result.thumb_height = thumb_height_;
        }
        return result;
    }

}  // namespace gamesnap
