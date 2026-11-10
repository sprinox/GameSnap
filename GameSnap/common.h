#pragma once

#include "image.h"

// ─────────────────────────────────────────────────────────────────────────────
// 公共基础设施：Windows / D3D / WinRT 头文件、日志、COM 驻留、时间工具、配置。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#    define NOMINMAX
#endif

// fmt 默认用异常报格式化错误；项目不依赖这条路径，关掉后出错直接断言/abort。
// 必须在任何 fmt 头之前定义（spdlog 会间接包含 fmt）。
#define FMT_USE_EXCEPTIONS 0  // NOLINT(cppcoreguidelines-macro-usage): fmt 的配置开关

#include <windows.h>
#include <unknwn.h>
// CreateDirect3D11DeviceFromDXGIDevice / IDirect3DDxgiInterfaceAccess 在这里。
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")

// 注意：spdlog 的宏配置必须在所有翻译单元里保持一致，否则会产生 ODR 冲突。
#define SPDLOG_NO_EXCEPTIONS
// 用 vcpkg 装的 fmt，而不是 spdlog 自带的 bundled fmt；必须和 spdlog.lib 的构建方式一致。
#define SPDLOG_FMT_EXTERNAL
// header-only 后由本工程实例化，避免和 vcpkg 窄字符 spdlog.lib 的 ABI 不一致。
#define SPDLOG_HEADER_ONLY
#define SPDLOG_WCHAR_FILENAMES
// 让 SPDLOG_DEBUG/TRACE 宏在 Release 下也保留（运行时可再用日志级别过滤）
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
// __func__ 比 __FUNCTION__ 干净：只给函数名，不带命名空间/签名
#define SPDLOG_FUNCTION static_cast<const char*>(__func__)
#include <spdlog/spdlog.h>

#define CPPTRACE_STATIC_DEFINE
#include <cpptrace/cpptrace.hpp>
#pragma comment(lib, "dbghelp.lib")

// 让 winrt 命名空间下可以直接使用 Graphics::Capture / DirectX 的类型。
namespace winrt {
    using namespace ::winrt::Windows::Graphics::Capture;
    using namespace ::winrt::Windows::Graphics::DirectX;
    using namespace ::winrt::Windows::Graphics::DirectX::Direct3D11;
}
// IDirect3DDxgiInterfaceAccess 在 ABI 命名空间里，这里别名到 Windows::。
namespace Windows {
    using namespace ::Windows::Graphics::DirectX::Direct3D11;
}

namespace gamesnap {

    // 文件名用墙上时间；阶段耗时用单调时钟，避免系统时间校准/修改导致负值。
    using Clock = std::chrono::system_clock;
    using SteadyClock = std::chrono::steady_clock;

    // ── 文本 ─────────────────────────────────────────────────────────────────
    std::string wstring_to_utf8(std::wstring_view str);
    std::wstring utf8_to_wstring(std::string_view str);

    // ── 日志 / 崩溃 ──────────────────────────────────────────────────────────
    // 文件名模板里出现了哪些占位符。采集线程据此只取需要的信息：
    // 没用到 {exe}/{process} 就不必 OpenProcess，没用到 {process} 就不必读版本资源。
    struct FilenamePlaceholders {
        bool title = false;
        bool exe = false;
        bool process = false;
        bool time = false;
        bool seq = false;
    };
    FilenamePlaceholders parse_filename_placeholders(std::wstring_view pattern);

    // 交给控制台（或重定向目标）的一段 UTF-8 文本。
    // 控制台用 WriteConsoleW，绕开会话代码页（cmd 默认 936 时中文会乱码）；
    // 句柄不是控制台（重定向到文件/管道）时退回 fwrite 原始 UTF-8 字节。
    void write_console_utf8(std::string_view text, bool to_stderr = false);

    void init_logging();
    void install_terminate_handler();
    void install_new_handler();
    // 启动时检查 AVX2 / F16C；不满足时记录原因并返回 false。
    bool check_cpu_features();

    // ── 时间 ─────────────────────────────────────────────────────────────────
    template <typename Duration>
    inline double duration_ms(Duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    // HRESULT → "0xXXXXXXXX 系统消息（已知原因提示）"，便于日志排查。
    std::string describe_hresult(HRESULT hr);

    // 一次截图在各阶段的时刻，用来统计“按键 → 落盘”的端到端延迟。
    struct ShotTimings {
        SteadyClock::time_point input;           // 热键事件进入命令队列
        SteadyClock::time_point dequeued;        // 截图线程取出命令
        SteadyClock::time_point title_done;      // 前台窗口标题读取完成
        SteadyClock::time_point prepared;        // frame pool / session 准备完成
        SteadyClock::time_point start_capture;   // 调用 StartCapture
        SteadyClock::time_point frame_received;  // 拿到帧的时刻
        SteadyClock::time_point copied;          // CopyResource 完成
    };

    // 通用 scope guard；析构时执行，dismiss() 取消。
    template <typename F>
    class ScopeExit {
    public:
        explicit ScopeExit(F f) : f_(std::move(f)) {}
        ~ScopeExit() noexcept {
            if (!active_)
                return;
            try {
                f_();
            } catch (...) {  // NOLINT(bugprone-empty-catch): scope guard 不能抛
            }
        }
        void dismiss() noexcept { active_ = false; }
        ScopeExit(ScopeExit const&) = delete;
        ScopeExit& operator=(ScopeExit const&) = delete;
        ScopeExit(ScopeExit&&) = delete;
        ScopeExit& operator=(ScopeExit&&) = delete;

    private:
        F f_;
        bool active_ = true;
    };

    template <typename F>
    ScopeExit(F) -> ScopeExit<F>;

    // ── COM 驻留 ─────────────────────────────────────────────────────────────
    // 每个使用 WinRT/D3D/GameInput 的线程都需要自己的 COM 公寓。
    // RoInitialize 是 WinRT 的初始化方式（MTA），S_FALSE 表示本线程已经在 MTA 中。
    class ComApartment {
    public:
        ComApartment() noexcept;
        ~ComApartment();
        ComApartment(const ComApartment&) = delete;
        ComApartment& operator=(const ComApartment&) = delete;
        ComApartment(ComApartment&&) = delete;
        ComApartment& operator=(ComApartment&&) = delete;

        [[nodiscard]] bool ok() const noexcept { return initialized_; }
        [[nodiscard]] HRESULT result() const noexcept { return result_; }

    private:
        HRESULT result_ = E_FAIL;
        bool initialized_ = false;
    };

    // ── 停止信号 ─────────────────────────────────────────────────────────────
    // 用 Win32 事件而不是条件变量：Ctrl+C 处理函数里只能做最简单的操作。
    class StopSource {
    public:
        StopSource() : event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
        ~StopSource() {
            if (event_ != nullptr)
                ::CloseHandle(event_);
        }
        StopSource(const StopSource&) = delete;
        StopSource& operator=(const StopSource&) = delete;
        StopSource(StopSource&&) = delete;
        StopSource& operator=(StopSource&&) = delete;

        void request_stop() {
            if (event_ != nullptr)
                ::SetEvent(event_);
        }
        [[nodiscard]] bool stop_requested() const {
            return event_ != nullptr && ::WaitForSingleObject(event_, 0) == WAIT_OBJECT_0;
        }
        void wait() const {
            if (event_ != nullptr)
                ::WaitForSingleObject(event_, INFINITE);
        }

    private:
        HANDLE event_ = nullptr;
    };

    // ── 配置 ─────────────────────────────────────────────────────────────────
    enum class MonitorChoice : std::uint8_t {
        Primary,
        Foreground,
    };

    std::string_view to_string(MonitorChoice choice);

    struct Hotkey {
        UINT vk = VK_F12;
        UINT modifiers = 0;  // MOD_CONTROL / MOD_SHIFT / MOD_ALT / MOD_WIN
    };

    struct Config {
        std::filesystem::path output_dir;
        // 文件名模板，支持占位符：
        //   {title}   截图时前台窗口的标题
        //   {exe}     前台进程的 exe 名（不含扩展名）
        //   {process} 前台进程的显示名（exe 版本信息里的 FileDescription，取不到时等于 {exe}）
        //   {time}    时间戳 YYYYMMDD_HHMMSS_mmm
        //   {seq}     本次运行的截图序号（4 位）
        // 未知的 {…} 原样保留；替换完会再做一次文件名清洗（非法字符换成 _，裁掉首尾空格/点）。
        std::wstring filename_pattern = L"{title}_{time}_{seq}";
        int encode_threads = 2;       // 编码线程数（同时决定并行落盘能力）
        UINT hotkey_vk = VK_F12;      // 键盘热键
        UINT hotkey_modifiers = 0;    // 修饰键组合
        bool hotkey_swallow = false;  // 是否吞掉热键（默认不吞，游戏仍能收到）
        MonitorChoice monitor = MonitorChoice::Primary;
        ImageFormat hdr_format = ImageFormat::Avif;  // HDR 输出容器（仅 avif/heic/jxl）
        // SDR 默认走 JXL 无损：实测（docs/benchmark.md）比 WebP 无损快 10~20 倍、体积更小，
        // 而且是逐字节无损；effort 5 在 4K 约 5 s，要更快可降到 3。
        ImageFormat sdr_format = ImageFormat::Jxl;
        ToneMapAlgorithm tone_map = ToneMapAlgorithm::Spline;
        ToneMapBackend tone_map_backend = ToneMapBackend::Auto;
        CodecOptions codec;
        bool verbose = false;
        bool selftest = false;  // 不经过 WGC，用合成纹理验证编码链路
    // 编码基准：给定一张 PNG，用各种格式/参数重新编码，量体积与耗时。
    std::optional<std::filesystem::path> bench_source;
    };

    std::filesystem::path default_output_dir();
    std::optional<Config> parse_command_line(int argc, wchar_t const* const* argv);
    void log_config(Config const& config);
    std::string hotkey_name(Hotkey const& hotkey);

}  // namespace gamesnap
