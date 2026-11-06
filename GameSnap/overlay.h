#pragma once

#include "common.h"

#include <atomic>
#include <memory>
#include <thread>

namespace gamesnap {

    class UiBridge;

    // 独立的 UI 线程：DirectComposition 透明窗口 + ImGui 自绘 + 内嵌 MP3。
    class OverlayThread {
    public:
        // monitor 必须和截图用同一个选择，否则双屏时面板会滑到另一块屏上。
        OverlayThread(std::shared_ptr<UiBridge> bridge, MonitorChoice monitor);
        ~OverlayThread();

        OverlayThread(OverlayThread const&) = delete;
        OverlayThread& operator=(OverlayThread const&) = delete;
        OverlayThread(OverlayThread&&) = delete;
        OverlayThread& operator=(OverlayThread&&) = delete;

        void start();
        void stop();
        void join();

    private:
        void run();
        std::uint64_t run_session(std::uint64_t trigger);

        std::shared_ptr<UiBridge> bridge_;
        MonitorChoice monitor_;
        std::thread thread_;
        std::atomic<bool> stop_{false};
        HANDLE stop_event_ = nullptr;
    };

}  // namespace gamesnap
