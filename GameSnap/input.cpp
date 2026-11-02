#include "pch.h"

#include "input.h"

namespace gamesnap {
    namespace {

        constexpr gi::GameInputGamepadButtons kTriggerButtons = gi::GameInputGamepadLeftShoulder | gi::GameInputGamepadRightShoulder;

        // 低层钩子的回调是静态函数，通过这个指针找到当前实例。
        // 同一时刻只会有一个 InputThread（进程级单例）。
        // 用函数内静态变量而不是全局变量：便于初始化顺序，也避免 [avoid-non-const-global-variables]。
        std::atomic<InputThread*>& hook_target() {
            static std::atomic<InputThread*> target{nullptr};
            return target;
        }

    }  // namespace

    InputThread::InputThread(Config config, std::shared_ptr<CommandQueue> commands)
        : config_(std::move(config)), commands_(std::move(commands)) {}

    InputThread::~InputThread() {
        try {
            stop();
            join();
        } catch (...) {  // NOLINT(bugprone-empty-catch): 析构函数不能抛
        }
    }

    void InputThread::start() {
        thread_ = std::thread([this] {
            try {
                run();
            } catch (std::exception const& e) {
                SPDLOG_CRITICAL("input 线程异常退出: {}", e.what());
            } catch (...) {
                SPDLOG_CRITICAL("input 线程异常退出");
            }
        });
    }

    void InputThread::stop() {
        stop_.store(true, std::memory_order_release);
        const DWORD id = thread_id_.load(std::memory_order_acquire);
        if (id != 0)
            if (::PostThreadMessageW(id, WM_QUIT, 0, 0) == 0)
                SPDLOG_DEBUG("PostThreadMessageW(WM_QUIT) 失败: {}", ::GetLastError());
    }

    void InputThread::join() {
        if (thread_.joinable())
            thread_.join();
    }

    void InputThread::run() {
        ComApartment apartment;
        if (!apartment.ok())
            SPDLOG_WARN("input 线程初始化 COM 失败: {}", describe_hresult(apartment.result()));

        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        ::SetThreadDescription(::GetCurrentThread(), L"GameSnap.Input");

        thread_id_.store(::GetCurrentThreadId(), std::memory_order_release);

        // 先初始化 GameInput：它偶尔会阻塞几秒（服务不可用时），
        // 放在安装键盘钩子之前，避免这几秒里全局键盘输入被卡住。
        const HRESULT gameinput_hr = gi::GameInputCreate(game_input_.put());
        if (FAILED(gameinput_hr)) {
            SPDLOG_WARN("GameInput 初始化失败，手柄热键不可用: {}", describe_hresult(gameinput_hr));
        } else {
            // 手柄用 GameInput 读数回调；回调在 GameInput 的派发线程上执行，context 传 this。
            const HRESULT register_hr = game_input_->RegisterReadingCallback(
                nullptr, gi::GameInputKindGamepad, this, &InputThread::reading_callback, &reading_token_);
            if (FAILED(register_hr)) {
                SPDLOG_WARN("注册手柄读数回调失败: {}", describe_hresult(register_hr));
                reading_token_ = 0;
            }
        }

        // 低层键盘钩子：可以拿到全屏游戏（包括独占全屏）的按键。
        hook_target().store(this, std::memory_order_release);
        hook_ = ::SetWindowsHookExW(
            WH_KEYBOARD_LL, &InputThread::keyboard_proc, ::GetModuleHandleW(nullptr), 0);
        if (hook_ == nullptr)
            SPDLOG_ERROR("安装键盘钩子失败 (Win32 错误 {})，键盘热键不可用", ::GetLastError());

        // stop() 可能在消息队列就绪之前就被调用，这里补一次唤醒。
        if (stop_.load(std::memory_order_acquire))
            if (::PostThreadMessageW(thread_id_.load(std::memory_order_acquire), WM_QUIT, 0, 0) == 0)
                SPDLOG_DEBUG("PostThreadMessageW(WM_QUIT) 失败: {}", ::GetLastError());

        MSG message;
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        hook_target().store(nullptr, std::memory_order_release);
        if (hook_ != nullptr) {
            ::UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        if (game_input_ && reading_token_ != 0) {
            // 微软文档语义：UnregisterCallback 返回后不会再派发新回调；
            // 因此在 run() 退出前反注册，reading_callback 里的裸 this 是安全的。
            game_input_->UnregisterCallback(reading_token_);
            reading_token_ = 0;
        }
        game_input_ = nullptr;

        SPDLOG_INFO("input 线程退出");
    }

    LRESULT CALLBACK InputThread::keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
        if (code == HC_ACTION) {
            auto* self = hook_target().load(std::memory_order_acquire);
            if (self != nullptr) {
                const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
                const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
                if (down || up) {
                    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT const*>(lparam);  // NOLINT(performance-no-int-to-ptr): Win32 的 LPARAM 约定
                    if (info != nullptr && info->vkCode == self->config_.hotkey_vk) {
                        // 钩子回调里绝不能抛异常：抛出去会直接终止进程，
                        // 而且会卡住整台机器的键盘输入。
                        try {
                            self->on_hotkey(down);
                        } catch (std::exception const& e) {
                            SPDLOG_ERROR("处理热键时异常: {}", e.what());
                        } catch (...) {
                            SPDLOG_ERROR("处理热键时异常");
                        }
                        // 默认不吞热键：游戏仍然能收到这个按键。
                        if (down && self->config_.hotkey_swallow)
                            return 1;
                    }
                }
            }
        }
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    void InputThread::on_hotkey(bool down) {
        if (down) {
            // 按住不放会产生连续的 WM_KEYDOWN，只在第一次按下时触发。
            // 只会在本线程的钩子回调里访问，不需要原子量。
            if (key_down_)
                return;
            key_down_ = true;
            trigger(Clock::now());
        } else {
            key_down_ = false;
        }
    }

    void InputThread::trigger(Clock::time_point when) {
        Command command;
        command.kind = CommandKind::Trigger;
        command.reason = ReloadReason::Manual;
        command.enqueued = when;
        // 键盘钩子线程和 GameInput 回调线程都会调用；一次按键只入队一次，
        // 用不带 token 的 enqueue，避免两个线程共享同一个 ProducerToken。
        commands_->enqueue(command);
    }

    void InputThread::on_reading(gi::IGameInputReading& reading) {
        gi::GameInputGamepadState state{};
        if (!reading.GetGamepadState(&state))
            return;

        const bool combo = (state.buttons & kTriggerButtons) == kTriggerButtons;
        if (combo && !combo_latched_.exchange(true, std::memory_order_acq_rel)) {
            SPDLOG_DEBUG("手柄触发截图");
            trigger(Clock::now());
        } else if (!combo) {
            combo_latched_.store(false, std::memory_order_release);
        }
    }

    void CALLBACK InputThread::reading_callback([[maybe_unused]] gi::GameInputCallbackToken callback_token,
                                                void* context, gi::IGameInputReading* reading) {
        auto* self = static_cast<InputThread*>(context);
        if (self == nullptr || reading == nullptr)
            return;
        try {
            self->on_reading(*reading);
        } catch (...) {  // NOLINT(bugprone-empty-catch): GameInput 回调里不能抛异常
        }
    }

}  // namespace gamesnap
