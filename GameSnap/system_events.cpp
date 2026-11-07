#include "pch.h"

#include <shellapi.h>

// 托盘图标走 shell32。
#pragma comment(lib, "shell32.lib")

#include "system_events.h"

namespace gamesnap {
    namespace {

        // 编译期字面量，不需要每次 run() 构造 std::wstring。
        constexpr wchar_t const* kWindowClassName = L"GameSnap.SystemEvents";
        constexpr wchar_t const* kWindowTitle = L"GameSnap";
        // 两次以上廉价自检之间的最小间隔。WM_SETTINGCHANGE 会随主题、输入法、显示器设置
        // 和各种应用广播频繁到来，每次自检都要枚举 DXGI 输出，必须合并。
        constexpr auto kValidateThrottle = std::chrono::milliseconds(500);

        // 托盘：回调消息 + 菜单命令 ID。
        constexpr UINT kTrayCallbackMessage = WM_APP + 1;
        constexpr UINT kTrayNotifyReady = WM_APP + 2;
        constexpr UINT kTrayIconId = 1;
        constexpr UINT kMenuExit = 2;
        constexpr wchar_t const* kTrayTip = L"GameSnap — 热键截图（右键菜单）";

        std::atomic<SystemEventThread*>& win_event_target() {
            static std::atomic<SystemEventThread*> target{nullptr};
            return target;
        }

    }  // namespace

    SystemEventThread::SystemEventThread(std::shared_ptr<CommandQueue> commands, MonitorChoice monitor,
                                         std::shared_ptr<StopSource> stop_signal)
        : commands_(std::move(commands)), stop_signal_(std::move(stop_signal)), monitor_(monitor) {}

    SystemEventThread::~SystemEventThread() {
        try {
            stop();
            join();
        } catch (...) {  // NOLINT(bugprone-empty-catch): 析构函数不能抛
        }
    }

    void SystemEventThread::start() {
        thread_ = std::thread([this] {
            try {
                run();
            } catch (std::exception const& e) {
                SPDLOG_CRITICAL("system-events 线程异常退出: {}", e.what());
            } catch (...) {
                SPDLOG_CRITICAL("system-events 线程异常退出");
            }
        });
    }

    void SystemEventThread::stop() {
        stop_.store(true, std::memory_order_release);
        const DWORD id = thread_id_.load(std::memory_order_acquire);
        if (id != 0 && ::PostThreadMessageW(id, WM_QUIT, 0, 0) == 0) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_INVALID_THREAD_ID)
                SPDLOG_DEBUG("PostThreadMessageW(WM_QUIT) 失败: {}", error);
        }
    }

    void SystemEventThread::join() {
        if (thread_.joinable())
            thread_.join();
        thread_id_.store(0, std::memory_order_release);
    }

    void SystemEventThread::run() {
        ::SetThreadDescription(::GetCurrentThread(), L"GameSnap.SystemEvents");
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        // explorer 重启后托盘图标会丢，需要靠这条广播消息重新添加。
        taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &SystemEventThread::window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClassName;

        if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            SPDLOG_ERROR("注册系统事件窗口类失败: {}", ::GetLastError());
            return;
        }

        window_ = ::CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kWindowClassName, kWindowTitle,
            WS_POPUP,
            0, 0, 0, 0,
            nullptr, nullptr, instance, this);

        if (window_ == nullptr) {
            SPDLOG_ERROR("创建系统事件窗口失败: {}", ::GetLastError());
            ::UnregisterClassW(kWindowClassName, instance);
            return;
        }

        thread_id_.store(::GetCurrentThreadId(), std::memory_order_release);
        window_handle_.store(window_, std::memory_order_release);
        // 确保线程消息队列已建立，stop() 的 WM_QUIT 不会丢。
        MSG queue_probe{};
        ::PeekMessageW(&queue_probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        add_tray_icon();

        win_event_target().store(this, std::memory_order_release);
        foreground_hook_ = ::SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            &SystemEventThread::win_event_proc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (foreground_hook_ == nullptr)
            SPDLOG_DEBUG("SetWinEventHook(EVENT_SYSTEM_FOREGROUND) 失败: {}", ::GetLastError());
        // stop() 可能在本线程拿到消息队列之前就已经被调用。
        if (stop_.load(std::memory_order_acquire))
            if (::PostThreadMessageW(thread_id_.load(std::memory_order_acquire), WM_QUIT, 0, 0) == 0)
                SPDLOG_DEBUG("PostThreadMessageW(WM_QUIT) 失败: {}", ::GetLastError());

        MSG message;
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        if (foreground_hook_ != nullptr) {
            ::UnhookWinEvent(foreground_hook_);
            foreground_hook_ = nullptr;
        }
        win_event_target().store(nullptr, std::memory_order_release);
        remove_tray_icon();
        window_handle_.store(nullptr, std::memory_order_release);

        if (window_ != nullptr) {
            ::DestroyWindow(window_);
            window_ = nullptr;
        }
        ::UnregisterClassW(kWindowClassName, instance);
        SPDLOG_INFO("system-events 线程退出");
    }

    void SystemEventThread::notify_ready() {
        const HWND hwnd = window_handle_.load(std::memory_order_acquire);
        if (hwnd != nullptr)
            (void)::PostMessageW(hwnd, kTrayNotifyReady, 0, 0);
    }

    void SystemEventThread::add_tray_icon() {
        if (tray_added_ || window_ == nullptr)
            return;
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = kTrayIconId;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_.uCallbackMessage = kTrayCallbackMessage;
        tray_.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray_.szTip, kTrayTip);
        if (::Shell_NotifyIconW(NIM_ADD, &tray_) == FALSE) {
            SPDLOG_WARN("添加托盘图标失败: {}", ::GetLastError());
            return;
        }
        tray_added_ = true;
    }

    void SystemEventThread::remove_tray_icon() {
        if (!tray_added_)
            return;
        ::Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_added_ = false;
    }

    void SystemEventThread::show_balloon() {
        if (!tray_added_)
            return;
        tray_.uFlags = NIF_INFO;
        wcscpy_s(tray_.szInfoTitle, L"GameSnap");
        wcscpy_s(tray_.szInfo, L"已就绪：按热键截图。右键托盘图标可切换控制台或退出。");
        tray_.dwInfoFlags = NIIF_INFO;
        tray_.uTimeout = 5000;
        if (::Shell_NotifyIconW(NIM_MODIFY, &tray_) == FALSE)
            SPDLOG_DEBUG("系统通知发送失败: {}", ::GetLastError());
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }

    void SystemEventThread::show_menu() {
        HMENU menu = ::CreatePopupMenu();
        if (menu == nullptr)
            return;
        ::AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        POINT point{};
        ::GetCursorPos(&point);
        // 必须先把窗口拿到前台，点菜单外部时菜单才会正常消失。
        ::SetForegroundWindow(window_);
        const UINT command = ::TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, window_, nullptr);
        ::DestroyMenu(menu);
        (void)::PostMessageW(window_, WM_NULL, 0, 0);

        if (command == kMenuExit) {
            SPDLOG_INFO("托盘菜单请求退出");
            if (stop_signal_)
                stop_signal_->request_stop();
        }
    }

    void SystemEventThread::post(CommandKind kind, ReloadReason reason) {
        Command command;
        command.kind = kind;
        command.reason = reason;
        command.enqueued = SteadyClock::now();
        commands_->enqueue(command);
    }

    void SystemEventThread::post_validate(ReloadReason reason) {
        const auto now = SteadyClock::now();
        if (last_validate_ != SteadyClock::time_point{} && now - last_validate_ < kValidateThrottle) {
            SPDLOG_DEBUG("自检请求过于频繁，合并本次（{}ms 节流）", kValidateThrottle.count());
            return;
        }
        last_validate_ = now;
        post(CommandKind::Validate, reason);
    }

    void CALLBACK SystemEventThread::win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                                    LONG id_object, LONG id_child, DWORD thread_id, DWORD time) {
        (void)hook;
        (void)id_object;
        (void)id_child;
        (void)thread_id;
        (void)time;
        if (event != EVENT_SYSTEM_FOREGROUND || hwnd == nullptr)
            return;
        auto* self = win_event_target().load(std::memory_order_acquire);
        if (self == nullptr || self->monitor_ != MonitorChoice::Foreground)
            return;

        HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        if (monitor == nullptr || monitor == self->foreground_monitor_)
            return;
        self->foreground_monitor_ = monitor;
        self->post(CommandKind::Validate, ReloadReason::DisplayChange);
    }

    LRESULT CALLBACK SystemEventThread::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<SystemEventThread*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));  // NOLINT(performance-no-int-to-ptr): Win32 的 GWLP_USERDATA 约定
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);  // NOLINT(performance-no-int-to-ptr): Win32 的 LPARAM 约定
            self = static_cast<SystemEventThread*>(create->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self == nullptr)
            return ::DefWindowProcW(hwnd, message, wparam, lparam);

        try {
            switch (message) {
            case kTrayCallbackMessage:
                switch (LOWORD(lparam)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    self->show_menu();
                    break;
                case WM_LBUTTONDBLCLK:
                    self->show_menu();
                    break;
                default:
                    break;
                }
                return 0;

            case kTrayNotifyReady:
                self->show_balloon();
                return 0;

            case WM_DISPLAYCHANGE:
                // 切全屏 / 改分辨率 / 切 HDR 都会广播；先自检，真变了才重建。
                SPDLOG_DEBUG("收到 WM_DISPLAYCHANGE");
                self->post_validate(ReloadReason::DisplayChange);
                return 0;

            case WM_SETTINGCHANGE:
                SPDLOG_DEBUG("收到 WM_SETTINGCHANGE");
                // HDR 开关等设置变化可能只走这里，做一次廉价自检即可（已节流）。
                self->post_validate(ReloadReason::DisplayChange);
                return 0;

            case WM_DEVICECHANGE:
                SPDLOG_DEBUG("收到 WM_DEVICECHANGE");
                // 显卡插拔 / 驱动重启：先自检，确认失效才重建，避免误重建。
                self->post_validate(ReloadReason::DeviceChange);
                return TRUE;

            case WM_POWERBROADCAST:
                SPDLOG_DEBUG("收到 WM_POWERBROADCAST");
                if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
                    SPDLOG_INFO("从睡眠恢复，准备重建捕获资源");
                    self->post(CommandKind::Reload, ReloadReason::PowerResume);
                }
                return TRUE;

            default:
                break;
            }
        } catch (std::exception const& e) {
            SPDLOG_ERROR("处理系统消息 0x{:04X} 时异常: {}", message, e.what());
        } catch (...) {
            SPDLOG_ERROR("处理系统消息 0x{:04X} 时异常", message);
        }

        // explorer 重启（TaskbarCreated）后补回托盘图标。
        if (self->taskbar_created_ != 0 && message == self->taskbar_created_) {
            self->tray_added_ = false;
            self->add_tray_icon();
            return 0;
        }

        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

}  // namespace gamesnap
