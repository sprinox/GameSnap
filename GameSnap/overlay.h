#pragma once

#include "common.h"

#include <atomic>
#include <memory>
#include <thread>

namespace gamesnap {

    class UiBridge;
    struct OverlayState;

    // 独立的 UI 线程：DirectComposition 透明窗口 + ImGui 自绘 + 内嵌 MP3。
    //
    // 资源策略（B 方案）：一次触发建好窗口/设备/交换链/ImGui/字体/播放器，面板收起后
    // 继续保留 kOverlayLinger（30s）；这段时间内再按热键直接复用，闲置超时才整体释放，
    // 回到"闲置不占显存/内存"。提示音在 UI 初始化之前播放。
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
        // 先出声：首次触发时初始化 MediaPlayer 并立刻播放，排在 UI 初始化之前。
        void play_audio();
        // 建好窗口 / 设备 / 交换链 / ImGui / 字体；已存在则直接复用。失败返回 false
        // （此时面板不可用，但提示音已经响了）。
        [[nodiscard]] bool ensure_ui();
        void release_ui();
        // 释放 UI 与播放器（linger 到期或线程退出）。
        void release_all();
        // 播放一次面板动画，返回这段时间内处理到的最新 trigger 序号。
        std::uint64_t run_panel(std::uint64_t trigger);

        std::shared_ptr<UiBridge> bridge_;
        MonitorChoice monitor_;
        std::thread thread_;
        std::atomic<bool> stop_{false};
        HANDLE stop_event_ = nullptr;
        // 常驻的窗口/设备/字体/播放器；为空表示已经整体释放。
        std::unique_ptr<OverlayState> state_;
    };

}  // namespace gamesnap
