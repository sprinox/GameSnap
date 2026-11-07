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
//
// 这个窗口同时是托盘图标的宿主：右键菜单可切换控制台显示 / 退出，启动就绪后
// 弹一条系统通知。控制台默认隐藏（见 main.cpp），日志仍写文件。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"
#include "messages.h"

namespace gamesnap {

    class SystemEventThread {
    public:
        SystemEventThread(std::shared_ptr<CommandQueue> commands, MonitorChoice monitor,
                          std::shared_ptr<StopSource> stop_signal);
        ~SystemEventThread();

        SystemEventThread(const SystemEventThread&) = delete;
        SystemEventThread& operator=(const SystemEventThread&) = delete;
        SystemEventThread(SystemEventThread&&) = delete;
        SystemEventThread& operator=(SystemEventThread&&) = delete;

        void start();
        void stop();
        void join();
        // 启动就绪后调用（任意线程）：托盘弹一条“已就绪”通知。
        void notify_ready();

    private:
        void run();
        void post(CommandKind kind, ReloadReason reason);
        // 合并短时间内的重复自检请求：WM_SETTINGCHANGE 在 Windows 上触发很频繁，
        // 每次自检都要枚举 DXGI 输出并查询显示器配置。
        void post_validate(ReloadReason reason);
        // 以下都只在 system-events 线程内调用。
        void add_tray_icon();
        void remove_tray_icon();
        void show_balloon();
        void show_menu();

        static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
        static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                            LONG id_object, LONG id_child, DWORD thread_id, DWORD time);
        // DXGI 适配器变化（显卡拔插 / eGPU / 驱动重启 / 显卡下线）的线程池回调。
        static void CALLBACK adapters_changed_proc(PVOID context, BOOLEAN timed_out);

        std::shared_ptr<CommandQueue> commands_;
        std::shared_ptr<StopSource> stop_signal_;
        std::thread thread_;
        std::atomic<DWORD> thread_id_{0};
        std::atomic<bool> stop_{false};
        MonitorChoice monitor_;
        HWND window_ = nullptr;
        // 给其他线程投递托盘通知用；线程退出时清空。
        std::atomic<HWND> window_handle_{nullptr};
        HWINEVENTHOOK foreground_hook_ = nullptr;
        HMONITOR foreground_monitor_ = nullptr;
        // 只在 system-events 线程读写；用来合并短时间内的重复 Validate。
        SteadyClock::time_point last_validate_{};
        NOTIFYICONDATAW tray_{};
        bool tray_added_ = false;
        UINT taskbar_created_ = 0;
        // 适配器变化事件：比 WM_DISPLAYCHANGE 更准（单纯显卡下线不一定改显示拓扑）。
        winrt::com_ptr<IDXGIFactory7> adapters_factory_;
        HANDLE adapters_event_ = nullptr;
        HANDLE adapters_wait_ = nullptr;
        DWORD adapters_cookie_ = 0;
    };

}  // namespace gamesnap
