#pragma once

#include <string_view>

namespace gamesnap {

    enum class OutputDestination { Unavailable,
                                   Console,
                                   Redirected };

    struct ConsoleState {
        OutputDestination out = OutputDestination::Unavailable;
        OutputDestination err = OutputDestination::Unavailable;
        bool crt_ready = true;

        [[nodiscard]] bool has_output() const noexcept {
            return out != OutputDestination::Unavailable || err != OutputDestination::Unavailable;
        }
    };

    // 启动时、创建日志 sink/工作线程前调用一次；重复调用不再修改句柄。
    // 仅连接父控制台，不分配新窗口、不修改代码页；独立保留 stdin/stdout/stderr 的重定向。
    ConsoleState initialize_console() noexcept;

    // 控制台写 UTF-16，文件/管道/NUL 写原始 UTF-8。两者均不依赖 CRT 绑定和缓冲。
    void write_console_utf8(std::string_view text, bool to_stderr = false) noexcept;
    // 同一实现的无堆分配/无 CRT 锁入口，供控制处理器和现有崩溃处理路径使用。
    void write_console_utf8_fixed(std::string_view text, bool to_stderr) noexcept;

}  // namespace gamesnap
