#include "pch.h"

#include <cstdio>
#include <exception>
#include <process.h>

#include "capture.h"
#include "overlay.h"
#include "ui_bridge.h"
#include "common.h"
#include "encoder.h"
#include "input.h"
#include "messages.h"
#include "selftest.h"
#include "system_events.h"

namespace {

    constexpr std::string_view kVersion = "0.2";

    // Ctrl+C 处理函数是 C 回调，只能通过一个静态位置找到 StopSource。
    // 用函数内静态变量避免非 const 全局变量。
    std::atomic<gamesnap::StopSource*>& stop_source_slot() {
        static std::atomic<gamesnap::StopSource*> slot{nullptr};
        return slot;
    }

    std::atomic<int>& ctrl_c_count() {
        static std::atomic<int> count{0};
        return count;
    }

    BOOL WINAPI console_ctrl_handler(DWORD type) {
        switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            if (auto* source = stop_source_slot().load(std::memory_order_acquire))
                source->request_stop();
            // 第一次正常收尾；再按一次说明收尾卡住，直接强制结束。
            // 这条路径不能经过日志框架：收尾卡住时可能有线程正持着日志锁，
            // 走 std::terminate() 再写日志会直接死锁。
            if (ctrl_c_count().fetch_add(1, std::memory_order_acq_rel) >= 1) {
                constexpr std::string_view kMessage = "GameSnap: 收尾卡住，强制退出\n";
                (void)std::fwrite(kMessage.data(), 1, kMessage.size(), stderr);
                (void)std::fflush(stderr);
                ::_exit(1);
            }
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (auto* source = stop_source_slot().load(std::memory_order_acquire))
                source->request_stop();
            return TRUE;
        default:
            return FALSE;
        }
    }

    int run(gamesnap::Config const& config) {

        gamesnap::ComApartment apartment;
        if (!apartment.ok()) {
            SPDLOG_CRITICAL("初始化 COM apartment 失败: {}", gamesnap::describe_hresult(apartment.result()));
            return 1;
        }

        // 控制台处理函数在独立线程回调；让 StopSource 活到进程结束，避免
        // handler 拿到指针后对象被析构的竞态（进程退出时由 OS 回收）。
        auto stop = std::shared_ptr<gamesnap::StopSource>(new gamesnap::StopSource(), [](gamesnap::StopSource*) {});
        auto slot_guard = gamesnap::ScopeExit{
            [&] { stop_source_slot().store(nullptr, std::memory_order_release); }};
        stop_source_slot().store(stop.get(), std::memory_order_release);
        ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

        // 输入线程 / 系统事件线程 → 截图线程；截图线程 → 编码线程。
        auto commands = std::make_shared<gamesnap::CommandQueue>();
        auto jobs = std::make_shared<gamesnap::EncodeQueue>();
        auto ui = std::make_shared<gamesnap::UiBridge>();
        gamesnap::OverlayThread overlay(ui, config.monitor);
        overlay.start();

        gamesnap::EncodeWorkers encoders(config.output_dir, jobs, config.codec, config.tone_map_backend, ui);
        encoders.start(config.encode_threads);

        gamesnap::InputThread input(config, commands, ui);
        input.start();

        gamesnap::SystemEventThread system_events(commands, config.monitor, stop);
        system_events.start();

        gamesnap::CaptureWorker capture(config, commands, jobs, stop, ui);
        std::atomic<bool> capture_crashed{false};
        std::jthread capture_thread([&capture, &stop, &capture_crashed] {
            // 线程入口不能抛：逃逸的异常会直接 std::terminate。
            try {
                capture.run();
            } catch (winrt::hresult_error const& e) {
                capture_crashed.store(true, std::memory_order_relaxed);
                SPDLOG_CRITICAL("capture 线程异常退出: {}", gamesnap::describe_hresult(e.code()));
            } catch (std::exception const& e) {
                capture_crashed.store(true, std::memory_order_relaxed);
                SPDLOG_CRITICAL("capture 线程异常退出: {}", e.what());
            } catch (...) {
                capture_crashed.store(true, std::memory_order_relaxed);
                SPDLOG_CRITICAL("capture 线程异常退出（未知异常）");
            }
            stop->request_stop();  // 捕获线程没了就整体退出，避免半死状态
        });

        // 等启动阶段的捕获资源初始化结束，避免先打印“就绪”再报失败。
        capture.wait_ready();
        if (!capture.fatal_error()) {
            if (capture.no_display()) {
                SPDLOG_WARN("GameSnap {} 已启动，但当前没有可用显示器；接入后会自动开始工作。", kVersion);
            } else {
                SPDLOG_INFO("GameSnap {} 就绪：按 {} 截图（手柄 Share 键），Ctrl+C 退出。",
                            kVersion, gamesnap::hotkey_name({config.hotkey_vk, config.hotkey_modifiers}));
            }
            system_events.notify_ready();
        }

        // 等到 Ctrl+C 等停止信号；StopSource 是事件，不需要轮询。
        stop->wait();
        SPDLOG_INFO("收到退出信号，正在收尾……");

        // 先停生产者，再用 Shutdown 命令让截图线程退出。
        input.stop();
        input.join();
        system_events.stop();
        system_events.join();

        if (!commands->enqueue(gamesnap::Command{.kind = gamesnap::CommandKind::Shutdown})) {
            SPDLOG_CRITICAL("无法投递 Shutdown 命令，强制退出");
            std::abort();
        }
        capture_thread.join();
        SPDLOG_INFO("capture 线程已退出");

        // 给每个编码线程投递停止哨兵，等它们把在途任务全部落盘。
        encoders.stop();
        SPDLOG_INFO("编码线程已全部退出");
        overlay.stop();
        overlay.join();
        SPDLOG_INFO("overlay 线程已退出");

        ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
        SPDLOG_INFO("已退出");
        return (capture.fatal_error() || capture_crashed.load(std::memory_order_relaxed)) ? 1 : 0;
    }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleOutputCP(CP_UTF8);

    gamesnap::install_terminate_handler();
    gamesnap::install_new_handler();
    gamesnap::init_logging();
    if (!gamesnap::check_cpu_features()) {
        spdlog::shutdown();
        return 1;
    }

    auto config = gamesnap::parse_command_line(argc, argv);
    if (!config) {
        const bool help_requested =
            std::ranges::any_of(std::span(argv, static_cast<std::size_t>(argc)),
                                [](wchar_t const* arg) {
                                    return arg != nullptr &&
                                           (std::wcscmp(arg, L"--help") == 0 || std::wcscmp(arg, L"-h") == 0);
                                });
        spdlog::shutdown();
        return help_requested ? 0 : 2;
    }

    if (config->verbose)
        spdlog::default_logger()->set_level(spdlog::level::debug);

    std::error_code ec;
    std::filesystem::create_directories(config->output_dir, ec);
    if (ec)
        SPDLOG_WARN("无法创建输出目录 {}: {}", gamesnap::wstring_to_utf8(config->output_dir.wstring()), ec.message());

    gamesnap::log_config(*config);

    if (config->selftest) {
        const bool ok = gamesnap::run_selftest(*config);
        spdlog::shutdown();
        return ok ? 0 : 1;
    }

    try {
        const int code = run(*config);
        spdlog::shutdown();
        return code;
    } catch (winrt::hresult_error const& e) {
        SPDLOG_CRITICAL("未处理的 WinRT 异常: 0x{:08X} {}",
                        static_cast<std::uint32_t>(e.code()), gamesnap::wstring_to_utf8(e.message()));
    } catch (std::exception const& e) {
        SPDLOG_CRITICAL("未处理的异常: {}", e.what());
    } catch (...) {
        SPDLOG_CRITICAL("未处理的未知异常");
    }

    spdlog::shutdown();
    return 1;
}
