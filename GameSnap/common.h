#pragma once

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
    void init_logging(bool verbose);
    void install_terminate_handler();

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
        SteadyClock::time_point start_capture;   // 调用 StartCapture
        SteadyClock::time_point frame_received;  // 拿到帧的时刻
        SteadyClock::time_point copied;          // CopyResource 完成
    };

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

    struct Config {
        std::filesystem::path output_dir;
        int encode_threads = 2;       // 编码线程数（同时决定并行落盘能力）
        UINT hotkey_vk = VK_F12;      // 键盘热键
        bool hotkey_swallow = false;  // 是否吞掉热键（默认不吞，游戏仍能收到）
        MonitorChoice monitor = MonitorChoice::Primary;
        bool hdr_capture = false;  // HDR 显示器以 FP16 捕获，再色调映射到 SDR 输出
        bool verbose = false;
        bool selftest = false;  // 不经过 WGC，用合成纹理验证编码链路
    };

    std::filesystem::path default_output_dir();
    std::optional<Config> parse_command_line(int argc, wchar_t const* const* argv);
    void log_config(Config const& config);
    std::string_view hotkey_name(UINT vk);

}  // namespace gamesnap
