#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>

namespace gamesnap {

    class UiBridge;

    // 独立的 UI 线程：DirectComposition 透明窗口 + ImGui 自绘 + 内嵌 MP3。
    class OverlayThread {
    public:
        explicit OverlayThread(std::shared_ptr<UiBridge> bridge);
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
        void run_session();

        std::shared_ptr<UiBridge> bridge_;
        std::thread thread_;
        std::atomic<bool> stop_{false};
        HANDLE stop_event_ = nullptr;
    };

}  // namespace gamesnap
