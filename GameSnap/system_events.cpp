#include "pch.h"

#include "system_events.h"

namespace gamesnap {
    namespace {

        // 编译期字面量，不需要每次 run() 构造 std::wstring。
        constexpr wchar_t const* kWindowClassName = L"GameSnap.SystemEvents";
        constexpr wchar_t const* kWindowTitle = L"GameSnap";

        std::atomic<SystemEventThread*>& win_event_target() {
            static std::atomic<SystemEventThread*> target{nullptr};
            return target;
        }

    }  // namespace

    SystemEventThread::SystemEventThread(std::shared_ptr<CommandQueue> commands, MonitorChoice monitor)
        : commands_(std::move(commands)), monitor_(monitor) {}

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
        // 确保线程消息队列已建立，stop() 的 WM_QUIT 不会丢。
        MSG queue_probe{};
        ::PeekMessageW(&queue_probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

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

        if (window_ != nullptr) {
            ::DestroyWindow(window_);
            window_ = nullptr;
        }
        ::UnregisterClassW(kWindowClassName, instance);
        SPDLOG_INFO("system-events 线程退出");
    }

    void SystemEventThread::post(CommandKind kind, ReloadReason reason) {
        Command command;
        command.kind = kind;
        command.reason = reason;
        command.enqueued = SteadyClock::now();
        commands_->enqueue(command);
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
            case WM_DISPLAYCHANGE:
                // 切全屏 / 改分辨率 / 切 HDR 都会广播；先自检，真变了才重建。
                SPDLOG_DEBUG("收到 WM_DISPLAYCHANGE");
                self->post(CommandKind::Validate, ReloadReason::DisplayChange);
                return 0;

            case WM_SETTINGCHANGE:
                SPDLOG_DEBUG("收到 WM_SETTINGCHANGE");
                // HDR 开关等设置变化可能只走这里，做一次廉价自检即可。
                self->post(CommandKind::Validate, ReloadReason::DisplayChange);
                return 0;

            case WM_DEVICECHANGE:
                SPDLOG_DEBUG("收到 WM_DEVICECHANGE");
                // 显卡插拔 / 驱动重启：先自检，确认失效才重建，避免误重建。
                self->post(CommandKind::Validate, ReloadReason::DeviceChange);
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

        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

}  // namespace gamesnap
