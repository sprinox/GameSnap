#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 键盘/手柄捕获线程。
//
//   * 键盘：WH_KEYBOARD_LL 低层钩子，可捕获全屏游戏里的按键。
//     注意低层钩子的回调是在本线程的消息泵里执行的，所以这个线程必须保持
//     纯消息循环（这里用 GetMessage），否则会拖慢整台机器的键盘输入。
//   * 手柄：GameInput 读数回调（Xbox Share 键）；系统没装 GameInput 时只影响手柄。
//
// 本线程只负责把 Command::Trigger 丢进命令队列，不做任何截图工作。
// ─────────────────────────────────────────────────────────────────────────────

// 必须先包含 common.h（其中包含 windows.h），GameInput.h 依赖它。
#include "common.h"

#include <GameInput.h>

#include "messages.h"

namespace gamesnap {
    class UiBridge;

    // Microsoft.GameInput 的 API 在 GameInput::v4 命名空间里。
    namespace gi = GameInput::v4;

    class InputThread {
    public:
        InputThread(Config config, std::shared_ptr<CommandQueue> commands, std::shared_ptr<UiBridge> ui);
        ~InputThread();

        InputThread(const InputThread&) = delete;
        InputThread& operator=(const InputThread&) = delete;
        InputThread(InputThread&&) = delete;
        InputThread& operator=(InputThread&&) = delete;

        void start();
        void stop();
        void join();

    private:
        void run();
        void on_hotkey(bool down);
        void trigger(SteadyClock::time_point when);
        void on_reading(gi::IGameInputReading& reading);

        static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam);
        static void CALLBACK reading_callback(gi::GameInputCallbackToken callback_token, void* context,
                                              gi::IGameInputReading* reading);

        Config config_;
        std::shared_ptr<CommandQueue> commands_;
        std::shared_ptr<UiBridge> ui_;

        std::thread thread_;
        std::atomic<DWORD> thread_id_{0};
        std::atomic<bool> stop_{false};
        bool key_down_ = false;  // 只在 input 线程的钩子回调里访问
        HHOOK hook_ = nullptr;

        winrt::com_ptr<gi::IGameInput> game_input_;
        gi::GameInputCallbackToken reading_token_ = 0;  // 0 表示未注册
        std::atomic<bool> share_latched_{false};        // GameInput 派发线程可能并发进来
    };

}  // namespace gamesnap
