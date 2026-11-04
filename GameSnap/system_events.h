#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 系统事件线程：一个隐藏的 top-level 窗口 + 消息循环，负责把
//   WM_DISPLAYCHANGE  → 廉价自检（分辨率 / 拓扑 / HDR 真变了才重建）
//   WM_SETTINGCHANGE  → 廉价自检
//   WM_DEVICECHANGE   → 廉价自检（显卡插拔 / 驱动重启）
//   WM_POWERBROADCAST → 强制重建（睡眠唤醒后设备常常已经失效）
//   EVENT_SYSTEM_FOREGROUND → monitor=foreground 时目标屏变化才自检
// 转换成命令投递到截图线程。注意必须用 top-level 窗口而不是 HWND_MESSAGE，
// 否则收不到这些广播消息。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"
#include "messages.h"

namespace gamesnap {

    class SystemEventThread {
    public:
        SystemEventThread(std::shared_ptr<CommandQueue> commands, MonitorChoice monitor);
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
        static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                            LONG id_object, LONG id_child, DWORD thread_id, DWORD time);

        std::shared_ptr<CommandQueue> commands_;
        std::thread thread_;
        std::atomic<DWORD> thread_id_{0};
        std::atomic<bool> stop_{false};
        MonitorChoice monitor_;
        HWND window_ = nullptr;
        HWINEVENTHOOK foreground_hook_ = nullptr;
        HMONITOR foreground_monitor_ = nullptr;
    };

}  // namespace gamesnap
