#include "pch.h"

#include "console.h"

#include <array>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <memory>

namespace gamesnap {
    namespace {

        struct HandleCloser {
            void operator()(void* handle) const noexcept { ::CloseHandle(handle); }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;

        Handle duplicate_handle(HANDLE source) noexcept {
            HANDLE copy = nullptr;
            if (!source || source == INVALID_HANDLE_VALUE ||
                !::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &copy,
                                   0, FALSE, DUPLICATE_SAME_ACCESS))
                return {};
            return Handle(copy);
        }

        OutputDestination destination(HANDLE handle, bool input = false) noexcept {
            if (!handle || handle == INVALID_HANDLE_VALUE)
                return OutputDestination::Unavailable;
            ::SetLastError(ERROR_SUCCESS);
            const DWORD type = ::GetFileType(handle);
            if (type == FILE_TYPE_UNKNOWN && ::GetLastError() != ERROR_SUCCESS)
                return OutputDestination::Unavailable;
            DWORD mode = 0, written = 0;
            // 只可写的 CONOUT$ 不支持 GetConsoleMode，但仍可正确写 Unicode。
            if (::GetConsoleMode(handle, &mode) ||
                (!input && type == FILE_TYPE_CHAR && ::WriteConsoleW(handle, L"", 0, &written, nullptr)))
                return OutputDestination::Console;
            // NUL 也是 FILE_TYPE_CHAR，不能误判成缺失的控制台并覆盖它。
            return OutputDestination::Redirected;
        }

        Handle open_console_device(bool input) noexcept {
            const HANDLE handle = ::CreateFileW(input ? L"CONIN$" : L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            return Handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
        }

        bool bind_crt(std::FILE* stream, HANDLE handle, bool input) noexcept {
            // GUI 进程的 _fileno 可能为 -2，不能直接传给 _dup2（会触发 CRT invalid-parameter）。
            // 原 Win32 句柄已由调用者单独持有，临时打开 NUL 不会丢失用户的重定向。
            if (::_fileno(stream) < 0) {
                std::FILE* reopened = nullptr;
                if (::freopen_s(&reopened, "NUL", input ? "r" : "w", stream) != 0)
                    return false;
            }
            auto copy = duplicate_handle(handle);
            if (!copy)
                return false;
            const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(copy.get()),
                                             (input ? _O_RDONLY : _O_WRONLY) | _O_TEXT);
            if (fd < 0)
                return false;
            (void)copy.release();  // 成功后仅 CRT fd 拥有这份副本。
            const bool bound = ::_dup2(fd, ::_fileno(stream)) == 0;
            (void)::_close(fd);
            if (bound)
                ::clearerr(stream);
            return bound;
        }

        ConsoleState inspect_state(bool crt_ready = true) noexcept {
            return {destination(::GetStdHandle(STD_OUTPUT_HANDLE)),
                    destination(::GetStdHandle(STD_ERROR_HANDLE)), crt_ready};
        }

        ConsoleState initialize_impl() noexcept {
            constexpr std::array<DWORD, 3> ids{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
            const std::array<std::FILE*, 3> streams{stdin, stdout, stderr};
            std::array<Handle, 3> saved;
            // 先保全所有有效句柄，再做 AttachConsole 或 CRT 重绑定；部分失败时由 RAII 回收。
            // 也覆盖多个标准流指向同一底层句柄的情况。
            std::array<OutputDestination, 3> kinds;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const HANDLE original = ::GetStdHandle(ids[i]);
                kinds[i] = destination(original, i == 0);
                if (kinds[i] != OutputDestination::Unavailable) {
                    saved[i] = duplicate_handle(original);
                    if (!saved[i])
                        return inspect_state(false);  // 保全失败，不修改原标准流。
                }
            }
            // ERROR_ACCESS_DENIED 可能表示已连接控制台；仍以后续打开设备是否成功为准。
            const bool attached = ::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE ||
                                  ::GetLastError() == ERROR_ACCESS_DENIED;
            // SetStdHandle 不接管所有权。这些句柄由本模块持有到进程退出，CRT 另持副本。
            static std::array<Handle, 3> owned;
            bool crt_ready = true;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                Handle target;
                if (attached && kinds[i] != OutputDestination::Redirected)
                    target = open_console_device(i == 0);
                if (!target)
                    target = std::move(saved[i]);
                if (!target)
                    continue;
                crt_ready = bind_crt(streams[i], target.get(), i == 0) && crt_ready;
                // freopen/_dup2 可能改动 STD_*，因此在 CRT 操作后发布最终 Win32 句柄。
                if (::SetStdHandle(ids[i], target.get()))
                    owned[i] = std::move(target);
                else
                    crt_ready = false;
            }
            return inspect_state(crt_ready);
        }

        void write_bytes(HANDLE handle, std::string_view text) noexcept {
            while (!text.empty()) {
                const DWORD count = static_cast<DWORD>(std::min<std::size_t>(text.size(), 64 * 1024));
                DWORD written = 0;
                if (!::WriteFile(handle, text.data(), count, &written, nullptr) || written == 0)
                    return;
                text.remove_prefix(written);
            }
        }

        void write_utf8(HANDLE handle, std::string_view text) noexcept {
            const auto kind = destination(handle);
            if (kind == OutputDestination::Unavailable)
                return;
            if (kind == OutputDestination::Redirected) {
                write_bytes(handle, text);
                return;
            }
            // 固定栈缓冲分块；不要在 UTF-8 多字节字符中间切断，也不截断长帮助信息。
            wchar_t wide[1024];
            while (!text.empty()) {
                std::size_t count = std::min(text.size(), std::size(wide));
                if (count < text.size()) {
                    while (count > 0 && (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80)
                        --count;
                    if (count == 0)
                        count = std::min(text.size(), std::size(wide));
                }
                const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(count),
                                                         wide, static_cast<int>(std::size(wide)));
                if (length == 0)
                    return;
                DWORD offset = 0;
                while (offset < static_cast<DWORD>(length)) {
                    DWORD written = 0;
                    if (!::WriteConsoleW(handle, wide + offset, static_cast<DWORD>(length) - offset,
                                         &written, nullptr) ||
                        written == 0)
                        return;
                    offset += written;
                }
                text.remove_prefix(count);
            }
        }

    }  // namespace

    ConsoleState initialize_console() noexcept {
        static const ConsoleState state = initialize_impl();
        return state;
    }

    void write_console_utf8(std::string_view text, bool to_stderr) noexcept {
        write_utf8(::GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE), text);
    }

    void write_console_utf8_fixed(std::string_view text, bool to_stderr) noexcept {
        write_console_utf8(text, to_stderr);
    }

}  // namespace gamesnap
