#include "pch.h"

#include "common.h"

#include "tonemap.h"

#include <roapi.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <cpu_features/cpuinfo_x86.h>
#include <yaml-cpp/yaml.h>

#include <cxxopts.hpp>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#undef GetObject

namespace gamesnap {
    namespace {

        constexpr std::size_t kLogFileSize = 8ULL * 1024 * 1024;
        constexpr std::size_t kLogFileCount = 2;

        std::filesystem::path executable_dir() {
            // exe 路径在进程生命周期内不变，只取一次；避免每次分配 64KB。
            static const std::filesystem::path kDir = [] {
                // manifest 里声明了 longPathAware，Win32 API 可接受最长约 32767 字符的路径。
                std::wstring buffer(0x8000, L'\0');
                const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0 || length >= buffer.size())
                    return std::filesystem::current_path();
                buffer.resize(length);
                buffer.shrink_to_fit();
                return std::filesystem::path(buffer).parent_path();
            }();
            return kDir;
        }

        // 日志放在 exe 旁边，和启动时的当前目录无关。
        std::filesystem::path log_file_path() {
            return executable_dir() / L"logs" / L"GameSnap.log";
        }

        // spdlog 在 SPDLOG_NO_EXCEPTIONS 下遇到错误会直接 abort，
        // 所以文件日志先自己探一次可写性。注意探的必须是 spdlog
        // 真正会用的窄字符路径（spdlog 默认 filename_t 是 std::string）。
        bool probe_writable(std::filesystem::path const& path) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            const std::ofstream file(path, std::ios::app);
            return file.good();
        }

        std::optional<int> parse_int(std::wstring_view text) {
            if (text.empty())
                return std::nullopt;
            wchar_t* end = nullptr;
            const std::wstring copy(text);
            const long value = std::wcstol(copy.c_str(), &end, 10);
            if (end == copy.c_str() || *end != L'\0')
                return std::nullopt;
            return static_cast<int>(value);
        }

        std::optional<UINT> parse_key_name(std::wstring_view lowered) {
            if (lowered.size() >= 2 && lowered[0] == L'f') {
                if (auto number = parse_int(lowered.substr(1)); number && *number >= 1 && *number <= 24)
                    return static_cast<UINT>(VK_F1 + *number - 1);
            }
            if (lowered == L"printscreen" || lowered == L"prtsc" || lowered == L"snapshot")
                return static_cast<UINT>(VK_SNAPSHOT);
            if (lowered == L"scrolllock")
                return static_cast<UINT>(VK_SCROLL);
            if (lowered == L"pause")
                return static_cast<UINT>(VK_PAUSE);
            if (lowered == L"insert")
                return static_cast<UINT>(VK_INSERT);
            if (lowered == L"home")
                return static_cast<UINT>(VK_HOME);
            if (lowered == L"end")
                return static_cast<UINT>(VK_END);
            if (lowered == L"space")
                return static_cast<UINT>(VK_SPACE);
            if (lowered == L"tab")
                return static_cast<UINT>(VK_TAB);
            if (lowered == L"enter" || lowered == L"return")
                return static_cast<UINT>(VK_RETURN);
            if (lowered == L"esc" || lowered == L"escape")
                return static_cast<UINT>(VK_ESCAPE);
            if (lowered == L"backspace")
                return static_cast<UINT>(VK_BACK);
            if (lowered == L"delete" || lowered == L"del")
                return static_cast<UINT>(VK_DELETE);
            if (lowered == L"up")
                return static_cast<UINT>(VK_UP);
            if (lowered == L"down")
                return static_cast<UINT>(VK_DOWN);
            if (lowered == L"left")
                return static_cast<UINT>(VK_LEFT);
            if (lowered == L"right")
                return static_cast<UINT>(VK_RIGHT);
            if (lowered == L"pageup" || lowered == L"pgup")
                return static_cast<UINT>(VK_PRIOR);
            if (lowered == L"pagedown" || lowered == L"pgdn")
                return static_cast<UINT>(VK_NEXT);

            // 单字母 / 单数字：a-z、0-9
            if (lowered.size() == 1) {
                if (lowered[0] >= L'a' && lowered[0] <= L'z')
                    return static_cast<UINT>('A') + static_cast<UINT>(lowered[0] - L'a');
                if (lowered[0] >= L'0' && lowered[0] <= L'9')
                    return static_cast<UINT>('0') + static_cast<UINT>(lowered[0] - L'0');
            }

            // 0x7B 或 123 这种数字写法
            const std::wstring copy(lowered);
            wchar_t* end = nullptr;
            const wchar_t* begin = copy.c_str();
            const unsigned long value = std::wcstoul(begin, &end, 0);
            if (end != begin && *end == L'\0' && value > 0 && value < 0x100)
                return static_cast<UINT>(value);
            return std::nullopt;
        }

        std::optional<Hotkey> parse_hotkey(std::wstring_view text) {
            std::wstring lowered(text);
            std::ranges::transform(lowered, lowered.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });

            Hotkey result;
            bool have_key = false;
            std::size_t start = 0;
            while (start <= lowered.size()) {
                const std::size_t plus = lowered.find(L'+', start);
                std::wstring_view token = std::wstring_view(lowered).substr(
                    start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
                while (!token.empty() && std::iswspace(token.front()))
                    token.remove_prefix(1);
                while (!token.empty() && std::iswspace(token.back()))
                    token.remove_suffix(1);
                if (token.empty())
                    return std::nullopt;

                if (token == L"ctrl" || token == L"control") {
                    result.modifiers |= MOD_CONTROL;
                } else if (token == L"shift") {
                    result.modifiers |= MOD_SHIFT;
                } else if (token == L"alt") {
                    result.modifiers |= MOD_ALT;
                } else if (token == L"win" || token == L"windows" || token == L"meta") {
                    result.modifiers |= MOD_WIN;
                } else {
                    if (have_key)
                        return std::nullopt;
                    const auto key = parse_key_name(token);
                    if (!key)
                        return std::nullopt;
                    result.vk = *key;
                    have_key = true;
                }

                if (plus == std::wstring::npos)
                    break;
                start = plus + 1;
            }
            if (!have_key)
                return std::nullopt;
            return result;
        }

        // ── YAML 配置 ────────────────────────────────────────────────────────
        class ConfigError : public std::runtime_error {
        public:
            using std::runtime_error::runtime_error;
        };

        // 配置项允许省略：const Node 的 operator[] 对缺失键返回 undefined，不会插入。
        template <typename T>
        std::optional<T> read_key(YAML::Node const& root, char const* key) {
            const YAML::Node value = root[key];
            if (!value)
                return std::nullopt;
            try {
                return value.as<T>();
            } catch (YAML::Exception const& e) {
                throw ConfigError(fmt::format("配置项 {}: {}", key, e.what()));
            }
        }

        bool is_known_config_key(std::string_view key) {
            constexpr std::array<std::string_view, 10> keys{
                "output_dir", "encode_threads", "hotkey", "swallow", "monitor", "hdr",
                "format_hdr", "format_sdr", "tone_map", "verbose"};
            return std::ranges::find(keys, key) != keys.end();
        }

        bool load_config_file(std::filesystem::path const& path, Config& config) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return false;

            // 先解析到临时副本，全部成功后再提交；避免半个配置生效。
            Config parsed = config;
            try {
                std::ifstream file(path);
                if (!file) {
                    SPDLOG_WARN("无法打开配置文件: {}", wstring_to_utf8(path.wstring()));
                    return false;
                }

                const YAML::Node root = YAML::Load(file);
                if (root.IsNull()) {
                    SPDLOG_WARN("配置文件为空: {}", wstring_to_utf8(path.wstring()));
                } else if (!root.IsMap()) {
                    throw ConfigError("配置文件顶层必须是 key: value 映射");
                } else {
                    if (const auto dir = read_key<std::string>(root, "output_dir"); dir && !dir->empty())
                        parsed.output_dir = utf8_to_wstring(*dir);
                    if (const auto value = read_key<int>(root, "encode_threads"); value)
                        parsed.encode_threads = *value;
                    if (const auto text = read_key<std::string>(root, "hotkey"); text) {
                        if (auto hotkey = parse_hotkey(utf8_to_wstring(*text)); hotkey) {
                            parsed.hotkey_vk = hotkey->vk;
                            parsed.hotkey_modifiers = hotkey->modifiers;
                        } else {
                            throw ConfigError(fmt::format("无法识别的热键: {}", *text));
                        }
                    }
                    if (const auto value = read_key<bool>(root, "swallow"); value)
                        parsed.hotkey_swallow = *value;
                    if (const auto value = read_key<bool>(root, "hdr"); value)
                        parsed.hdr_capture = *value;
                    if (const auto text = read_key<std::string>(root, "format_hdr"); text) {
                        const auto format = parse_image_format(*text);
                        if (!format || !format_supports_hdr(*format))
                            throw ConfigError(fmt::format("format_hdr 只支持 avif / heic，实际为 {}", *text));
                        parsed.hdr_format = *format;
                    }
                    if (const auto text = read_key<std::string>(root, "format_sdr"); text) {
                        const auto format = parse_image_format(*text);
                        if (!format)
                            throw ConfigError(fmt::format("无法识别的 format_sdr: {}", *text));
                        parsed.sdr_format = *format;
                    }
                    if (const auto text = read_key<std::string>(root, "tone_map"); text) {
                        const auto algorithm = parse_tone_map_algorithm(*text);
                        if (!algorithm)
                            throw ConfigError(fmt::format("无法识别的 tone_map: {}", *text));
                        parsed.tone_map = *algorithm;
                    }
                    if (const auto value = read_key<bool>(root, "verbose"); value)
                        parsed.verbose = *value;
                    if (const auto text = read_key<std::string>(root, "monitor"); text) {
                        if (*text == "primary")
                            parsed.monitor = MonitorChoice::Primary;
                        else if (*text == "foreground")
                            parsed.monitor = MonitorChoice::Foreground;
                        else
                            throw ConfigError(fmt::format("monitor 只支持 primary / foreground，实际为 {}", *text));
                    }

                    for (const auto& entry : root) {
                        if (!entry.first.IsScalar())
                            continue;
                        const auto key = entry.first.as<std::string>();
                        if (!is_known_config_key(key))
                            SPDLOG_WARN("未知配置项: {}", key);
                    }
                }
            } catch (std::exception const& e) {
                SPDLOG_ERROR("配置文件 {} 无效: {}", wstring_to_utf8(path.wstring()), e.what());
                return false;
            }

            config = std::move(parsed);
            SPDLOG_INFO("已加载配置文件: {}", wstring_to_utf8(path.wstring()));
            return true;
        }

    }  // namespace

    std::string wstring_to_utf8(std::wstring_view str) {
        if (str.empty())
            return {};

        const int size = ::WideCharToMultiByte(
            CP_UTF8, 0,
            str.data(), static_cast<int>(str.size()),
            nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            return {};

        std::string result(static_cast<std::size_t>(size), '\0');
        const int converted = ::WideCharToMultiByte(
            CP_UTF8, 0,
            str.data(), static_cast<int>(str.size()),
            result.data(), size, nullptr, nullptr);
        if (converted <= 0)
            return {};

        result.resize(static_cast<std::size_t>(converted));
        return result;
    }

    std::wstring utf8_to_wstring(std::string_view str) {
        if (str.empty())
            return {};

        const int size = ::MultiByteToWideChar(
            CP_UTF8, 0,
            str.data(), static_cast<int>(str.size()),
            nullptr, 0);
        if (size <= 0)
            return {};

        std::wstring result(static_cast<std::size_t>(size), L'\0');
        const int converted = ::MultiByteToWideChar(
            CP_UTF8, 0,
            str.data(), static_cast<int>(str.size()),
            result.data(), size);
        if (converted <= 0)
            return {};

        result.resize(static_cast<std::size_t>(converted));
        return result;
    }

    std::string describe_hresult(HRESULT hr) {
        std::string result = fmt::format("0x{:08X}", static_cast<std::uint32_t>(hr));

        wchar_t* buffer = nullptr;
        const DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(hr), 0,
            reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        if (length != 0 && buffer != nullptr) {
            std::wstring text(buffer, length);
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'\t'))
                text.pop_back();
            if (!text.empty())
                result += " " + wstring_to_utf8(text);
        }
        if (buffer != nullptr)
            ::LocalFree(buffer);

        switch (hr) {
        case E_ACCESSDENIED:
            result +=
                " 可能原因：进程完整性级别过低（沙盒/AppContainer）、运行在服务或非交互会话，"
                "或目标显示器内容受保护。请改在交互式桌面上以普通用户权限直接运行。";
            break;
        case DXGI_ERROR_UNSUPPORTED:
            result += " DXGI 不支持该操作（常见于虚拟显示器/远程桌面适配器）。";
            break;
        case DXGI_ERROR_DEVICE_REMOVED:
            result += " 显卡设备已被移除（TDR/驱动重启/热插拔），等待资源重建。";
            break;
        case DXGI_ERROR_DEVICE_HUNG:
            result += " 显卡设备挂起（TDR）。";
            break;
        default:
            break;
        }
        return result;
    }

    ComApartment::ComApartment() noexcept
        : result_(::RoInitialize(RO_INIT_MULTITHREADED)), initialized_(SUCCEEDED(result_)) {}

    ComApartment::~ComApartment() {
        if (initialized_)
            ::RoUninitialize();
    }

    void init_logging(bool verbose) {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        const auto log_path = log_file_path();
        if (!probe_writable(log_path)) {
            const std::string message =
                fmt::format("GameSnap: 无法写入日志文件 {}，仅输出到控制台\n", log_path.string());
            (void)std::fwrite(message.data(), 1, message.size(), stderr);
        } else {
            try {
                auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_path.string(), kLogFileSize, kLogFileCount);
                sinks.push_back(file_sink);
            } catch (std::exception const& e) {
                const std::string message =
                    fmt::format("GameSnap: cannot open log file ({}), logging to console only\n", e.what());
                (void)std::fwrite(message.data(), 1, message.size(), stderr);
            } catch (...) {
                constexpr std::string_view message = "GameSnap: cannot open log file, logging to console only\n";
                (void)std::fwrite(message.data(), 1, message.size(), stderr);
            }
        }

        auto logger = std::make_shared<spdlog::logger>("gamesnap", sinks.begin(), sinks.end());
        logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
        logger->set_pattern("[%T.%e] [%^%l%$] [%s:%# %!] %v");
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(2));
    }

    bool check_cpu_features() {
        const cpu_features::X86Info info = cpu_features::GetX86Info();
        bool ok = true;
        if (info.features.avx2 == 0) {
            SPDLOG_CRITICAL("CPU 不支持 AVX2，GameSnap 无法运行");
            ok = false;
        }
        if (info.features.f16c == 0) {
            SPDLOG_CRITICAL("CPU 不支持 F16C，GameSnap 无法运行");
            ok = false;
        }
        return ok;
    }

    void install_new_handler() {
        // 项目不逐处处理 bad_alloc：分配失败打印调用栈后终止，避免留下 mapped 纹理等半状态。
        std::set_new_handler([]() noexcept {
            // 先摘掉处理器：打印栈时再次 OOM 不再递归进入本函数。
            std::set_new_handler(nullptr);

            constexpr std::string_view message = "GameSnap: out of memory\n";
            (void)std::fwrite(message.data(), 1, message.size(), stderr);
            (void)std::fflush(stderr);

            try {
                SPDLOG_CRITICAL("std::bad_alloc: 内存分配失败");
                if (auto logger = spdlog::default_logger())
                    logger->flush();
            } catch (...) {  // NOLINT(bugprone-empty-catch): 新处理器里不能再次抛出
            }
            try {
                cpptrace::generate_trace().print_with_snippets();
            } catch (...) {  // NOLINT(bugprone-empty-catch): 新处理器里不能再次抛出
            }
            std::abort();
        });
    }

    void install_terminate_handler() {
        std::set_terminate([] {
            if (auto exception = std::current_exception()) {
                try {
                    std::rethrow_exception(exception);
                } catch (winrt::hresult_error const& e) {
                    SPDLOG_CRITICAL("std::terminate: WinRT 异常 0x{:08X} {}",
                                    static_cast<std::uint32_t>(e.code()),
                                    wstring_to_utf8(e.message()));
                } catch (std::exception const& e) {
                    SPDLOG_CRITICAL("std::terminate: {}", e.what());
                } catch (...) {
                    SPDLOG_CRITICAL("std::terminate: 未知异常");
                }
            } else {
                SPDLOG_CRITICAL("std::terminate: 没有活动异常");
            }
            spdlog::default_logger()->flush();
            try {
                cpptrace::generate_trace().print_with_snippets();
            } catch (...) {  // NOLINT(bugprone-empty-catch): terminate 路径里不能再次抛出
            }
            try {
                spdlog::shutdown();
            } catch (...) {  // NOLINT(bugprone-empty-catch): terminate 路径里不能再次抛出
            }
            std::abort();
        });
    }

    std::string_view to_string(MonitorChoice choice) {
        return choice == MonitorChoice::Foreground ? "foreground" : "primary";
    }

    std::string hotkey_name(Hotkey const& hotkey) {
        std::string name;
        const auto append = [&name](std::string_view part) {
            if (!name.empty())
                name += '+';
            name += part;
        };
        if ((hotkey.modifiers & MOD_CONTROL) != 0)
            append("Ctrl");
        if ((hotkey.modifiers & MOD_SHIFT) != 0)
            append("Shift");
        if ((hotkey.modifiers & MOD_ALT) != 0)
            append("Alt");
        if ((hotkey.modifiers & MOD_WIN) != 0)
            append("Win");

        const UINT vk = hotkey.vk;
        if (vk >= VK_F1 && vk <= VK_F24) {
            static constexpr std::array<const char*, 24> kNames{
                "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8",
                "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
                "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
            append(kNames[vk - VK_F1]);
        } else {
            switch (vk) {
            case VK_SPACE:
                append("Space");
                break;
            case VK_TAB:
                append("Tab");
                break;
            case VK_RETURN:
                append("Enter");
                break;
            case VK_ESCAPE:
                append("Esc");
                break;
            case VK_BACK:
                append("Backspace");
                break;
            case VK_DELETE:
                append("Delete");
                break;
            case VK_UP:
                append("Up");
                break;
            case VK_DOWN:
                append("Down");
                break;
            case VK_LEFT:
                append("Left");
                break;
            case VK_RIGHT:
                append("Right");
                break;
            case VK_PRIOR:
                append("PageUp");
                break;
            case VK_NEXT:
                append("PageDown");
                break;
            case VK_SNAPSHOT:
                append("PrintScreen");
                break;
            case VK_SCROLL:
                append("ScrollLock");
                break;
            case VK_PAUSE:
                append("Pause");
                break;
            case VK_INSERT:
                append("Insert");
                break;
            case VK_HOME:
                append("Home");
                break;
            case VK_END:
                append("End");
                break;
            default:
                if (vk >= static_cast<UINT>('A') && vk <= static_cast<UINT>('Z'))
                    append(std::string(1, static_cast<char>('A' + vk - static_cast<UINT>('A'))));
                else if (vk >= static_cast<UINT>('0') && vk <= static_cast<UINT>('9'))
                    append(std::string(1, static_cast<char>('0' + vk - static_cast<UINT>('0'))));
                else
                    append(fmt::format("VK 0x{:02X}", vk));
                break;
            }
        }
        return name;
    }

    std::filesystem::path default_output_dir() {
        PWSTR pictures = nullptr;
        std::filesystem::path base;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures)) && pictures != nullptr) {
            base = pictures;
            ::CoTaskMemFree(pictures);
        }
        if (base.empty()) {
            std::error_code ec;
            base = std::filesystem::current_path(ec);
        }
        return base / L"GameSnap";
    }

    namespace {
        // cxxopts 只接受 char*，wmain 给的是 wchar_t**，这里转成 UTF-8。
        std::vector<std::string> utf8_arguments(int argc, wchar_t const* const* argv) {
            std::vector<std::string> arguments;
            arguments.reserve(static_cast<std::size_t>(argc));
            for (int i = 0; i < argc; ++i)
                arguments.push_back(wstring_to_utf8(argv[i]));
            return arguments;
        }
    }  // namespace

    std::optional<Config> parse_command_line(int argc, wchar_t const* const* argv) {
        const auto arguments = utf8_arguments(argc, argv);
        std::vector<const char*> argv_utf8;
        argv_utf8.reserve(arguments.size());
        for (auto const& argument : arguments)
            argv_utf8.push_back(argument.c_str());

        cxxopts::Options options("GameSnap", "低延迟整显示器截图");
        // clang-format off
        options.add_options()
            ("config", "配置文件（默认 exe 目录 / 当前目录的 config.yaml）", cxxopts::value<std::string>())
            ("key", "截图热键：F1..F24 / printscreen / scrolllock / 0x7B，可加 ctrl+shift+alt+win", cxxopts::value<std::string>())
            ("out", "输出目录", cxxopts::value<std::string>())
            ("threads", "编码线程数 1..8", cxxopts::value<int>())
            ("monitor", "primary / foreground", cxxopts::value<std::string>())
            ("swallow", "吞掉热键（游戏收不到）")
            ("hdr", "HDR 显示器以 FP16 捕获并输出 HDR + SDR 两份")
            ("format-hdr", "HDR 容器 avif / heic", cxxopts::value<std::string>())
            ("format-sdr", "SDR 格式 bmp/png/jpeg/webp/avif/heic", cxxopts::value<std::string>())
            ("tone-map", "色调映射 spline / lilium", cxxopts::value<std::string>())
            ("verbose", "输出调试日志")
            ("selftest", "用合成纹理跑编码链路自检")
            ("h,help", "显示帮助");
        // clang-format on

        const auto print_help = [&options] {
            std::string help = options.help();
            help += "\n手柄：按 Xbox Share 键触发截图。\n退出：Ctrl+C。日志位于 exe 目录下的 logs\\GameSnap.log。\n";
            (void)std::fwrite(help.data(), 1, help.size(), stdout);
            (void)std::fflush(stdout);
        };

        cxxopts::ParseResult result;
        try {
            result = options.parse(argc, argv_utf8.data());
        } catch (std::exception const& e) {
            SPDLOG_ERROR("参数错误: {}", e.what());
            print_help();
            return std::nullopt;
        }
        if (result.count("help") != 0) {
            print_help();
            return std::nullopt;
        }

        Config config;
        config.output_dir = default_output_dir();

        // 配置文件：--config 优先，其次 exe 目录/config.yaml，最后当前目录/config.yaml。
        std::filesystem::path config_path;
        if (result.count("config") != 0) {
            config_path = utf8_to_wstring(result["config"].as<std::string>());
        } else {
            std::error_code ec;
            const auto exe_config = executable_dir() / "config.yaml";
            if (std::filesystem::exists(exe_config, ec)) {
                config_path = exe_config;
            } else {
                const auto cwd_config = std::filesystem::current_path(ec) / "config.yaml";
                if (std::filesystem::exists(cwd_config, ec))
                    config_path = cwd_config;
            }
        }
        const bool explicit_config = result.count("config") != 0;
        if (!config_path.empty() && !load_config_file(config_path, config) && explicit_config) {
            SPDLOG_ERROR("配置文件不存在: {}", wstring_to_utf8(config_path.wstring()));
            return std::nullopt;
        }

        // 命令行覆盖 YAML。
        if (result.count("key") != 0) {
            const auto text = utf8_to_wstring(result["key"].as<std::string>());
            const auto hotkey = parse_hotkey(text);
            if (!hotkey) {
                SPDLOG_ERROR("无法识别的热键: {}", wstring_to_utf8(text));
                return std::nullopt;
            }
            config.hotkey_vk = hotkey->vk;
            config.hotkey_modifiers = hotkey->modifiers;
        }
        if (result.count("out") != 0)
            config.output_dir = utf8_to_wstring(result["out"].as<std::string>());
        if (result.count("threads") != 0)
            config.encode_threads = result["threads"].as<int>();
        if (result.count("monitor") != 0) {
            const auto text = utf8_to_wstring(result["monitor"].as<std::string>());
            if (text == L"primary") {
                config.monitor = MonitorChoice::Primary;
            } else if (text == L"foreground") {
                config.monitor = MonitorChoice::Foreground;
            } else {
                SPDLOG_ERROR("--monitor 只支持 primary / foreground");
                return std::nullopt;
            }
        }
        if (result.count("swallow") != 0)
            config.hotkey_swallow = true;
        if (result.count("hdr") != 0)
            config.hdr_capture = true;
        if (result.count("format-hdr") != 0) {
            const auto format = parse_image_format(result["format-hdr"].as<std::string>());
            if (!format || !format_supports_hdr(*format)) {
                SPDLOG_ERROR("--format-hdr 只支持 avif / heic");
                return std::nullopt;
            }
            config.hdr_format = *format;
        }
        if (result.count("format-sdr") != 0) {
            const auto format = parse_image_format(result["format-sdr"].as<std::string>());
            if (!format) {
                SPDLOG_ERROR("--format-sdr 无法识别");
                return std::nullopt;
            }
            config.sdr_format = *format;
        }
        if (result.count("tone-map") != 0) {
            const auto algorithm = parse_tone_map_algorithm(result["tone-map"].as<std::string>());
            if (!algorithm) {
                SPDLOG_ERROR("--tone-map 只支持 spline / lilium");
                return std::nullopt;
            }
            config.tone_map = *algorithm;
        }
        if (result.count("verbose") != 0)
            config.verbose = true;
        if (result.count("selftest") != 0)
            config.selftest = true;

        config.encode_threads = std::clamp(config.encode_threads, 1, 8);
        return config;
    }

    void log_config(Config const& config) {
        SPDLOG_INFO("输出目录: {}", wstring_to_utf8(config.output_dir.wstring()));
        SPDLOG_INFO("输出格式: HDR={} SDR={}，色调映射={}",
                    to_string(config.hdr_format), to_string(config.sdr_format), to_string(config.tone_map));
        SPDLOG_INFO("截图热键: {} ({}吞掉)",
                    hotkey_name({config.hotkey_vk, config.hotkey_modifiers}),
                    config.hotkey_swallow ? "" : "不");
        SPDLOG_INFO("目标显示器: {}", to_string(config.monitor));
        SPDLOG_INFO("编码线程: {}", config.encode_threads);
        SPDLOG_INFO("采集模式: 按需(热键时 StartCapture){}", config.hdr_capture ? " + HDR" : "");
    }

}  // namespace gamesnap
