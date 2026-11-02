#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 系统事件线程：一个隐藏的 top-level 窗口 + 消息循环，负责把
//   WM_DISPLAYCHANGE  → 廉价自检（分辨率 / 拓扑 / HDR 真变了才重建）
//   WM_SETTINGCHANGE  → 廉价自检
//   WM_DEVICECHANGE   → 廉价自检（显卡插拔 / 驱动重启）
//   WM_POWERBROADCAST → 强制重建（睡眠唤醒后设备常常已经失效）
// 转换成命令投递到截图线程。注意必须用 top-level 窗口而不是 HWND_MESSAGE，
// 否则收不到这些广播消息。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"
#include "messages.h"

namespace gamesnap {

    class SystemEventThread {
    public:
        explicit SystemEventThread(std::shared_ptr<CommandQueue> commands);
        ~SystemEventThread();

        SystemEventThread(const SystemEventThread&) = delete;
        SystemEventThread& operator=(const SystemEventThread&) = delete;
        SystemEventThread(SystemEventThread&&) = delete;
        SystemEventThread& operator=(SystemEventThread&&) = delete;

        void start();
        void stop();
        void join();

    private:
        void run();
        void post(CommandKind kind, ReloadReason reason);

        static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

        std::shared_ptr<CommandQueue> commands_;
        moodycamel::ProducerToken command_token_;
        std::thread thread_;
        std::atomic<DWORD> thread_id_{0};
        std::atomic<bool> stop_{false};
        HWND window_ = nullptr;
    };

}  // namespace gamesnap
