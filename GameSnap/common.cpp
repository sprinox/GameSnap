#include "pch.h"

#include "common.h"

#include "tonemap.h"

#include <roapi.h>
#include <shlobj.h>
// ITaskbarList：把控制台窗口从任务栏摘掉/摘回。
#include <shobjidl_core.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <fcntl.h>  // _O_RDWR / _O_TEXT：把控制台句柄绑到 CRT 流
#include <fstream>
#include <io.h>  // _open_osfhandle / _dup2：把控制台句柄绑到 CRT 流
#include <iterator>
#include <stdexcept>

#include <cpu_features/cpuinfo_x86.h>
#include <yaml-cpp/yaml.h>

#include <cxxopts.hpp>

#include <spdlog/sinks/null_sink.h>
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

        // 这个句柄是不是用户重定向出去的文件/管道——是的话必须原样保留，不能抢去写控制台。
        bool is_redirect_handle(HANDLE handle) {
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
                return false;
            const DWORD type = ::GetFileType(handle);
            return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
        }

        // 让 CRT 的 FILE* 也指向这个设备。_open_osfhandle 会接管句柄所有权，
        // 所以先复制一份给它，我们自己的句柄还要留给 SetStdHandle。
        // flags 必须和句柄的真实权限匹配：用户 `2> err.txt` 的句柄通常只可写，
        // 用 _O_RDWR 可能让 _open_osfhandle 失败（那 CRT 就还停在旧流上）。
        bool rebind_crt(std::FILE* stream, HANDLE handle, int flags) {
            HANDLE duplicate = nullptr;
            if (::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(), &duplicate, 0, TRUE,
                                  DUPLICATE_SAME_ACCESS) == FALSE) {
                return false;
            }
            int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(duplicate), flags | _O_TEXT);
            if (fd == -1)  // 权限猜错了就退回读写试试（失败的 _open_osfhandle 不接管所有权）
                fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(duplicate), _O_RDWR | _O_TEXT);
            if (fd == -1) {
                ::CloseHandle(duplicate);
                return false;
            }
            (void)::_dup2(fd, ::_fileno(stream));
            (void)::_close(fd);
            return true;
        }

        // 复制一份句柄：AttachConsole 不保证旧的标准句柄还开着，直接存原值可能拿到失效句柄。
        HANDLE duplicate_handle(HANDLE handle) {
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
                return INVALID_HANDLE_VALUE;
            HANDLE duplicate = nullptr;
            if (::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(), &duplicate, 0, TRUE,
                                  DUPLICATE_SAME_ACCESS) == FALSE) {
                return INVALID_HANDLE_VALUE;
            }
            return duplicate;
        }

        // 打开控制台设备并把句柄写回 STD_*，同时让 CRT 的 stdin/stdout/stderr 指向同一个设备。
        // 必须用 CreateFileW 而不是 freopen_s：freopen(..., "w") 拿到的是**只可写**的句柄，
        // 而 spdlog 的 wincolor_sink 是靠 GetConsoleMode 判断"是不是控制台"的
        // （GetConsoleMode 需要 GENERIC_READ）。只可写 → 它判定不是控制台 →
        // 不打颜色，并且改走 WriteFile 写 UTF-8 字节，在 936 代码页的控制台里就是乱码。
        //
        // err/in 传 false 表示"用户已经把 stderr/stdin 重定向到文件/管道"，此时不要动它们；
        // 注意要在 AttachConsole **之前**采样：attach 会把 STD_* 换成控制台句柄。
        bool open_console_streams(bool keep_stderr, bool keep_stdin) {
            const auto open_device = [](wchar_t const* name, DWORD access) {
                return ::CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                     nullptr);
            };
            const HANDLE out = open_device(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
            if (out == INVALID_HANDLE_VALUE)
                return false;
            ::SetStdHandle(STD_OUTPUT_HANDLE, out);
            (void)rebind_crt(stdout, out, _O_WRONLY);
            // stdout 不用判断——能走到这里说明它要么无效、要么是只可写控制台，替换才是对的。
            if (!keep_stderr) {
                if (const HANDLE err = open_device(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
                    err != INVALID_HANDLE_VALUE) {
                    ::SetStdHandle(STD_ERROR_HANDLE, err);
                    (void)rebind_crt(stderr, err, _O_WRONLY);
                }
            }
            if (!keep_stdin) {
                if (const HANDLE in = open_device(L"CONIN$", GENERIC_READ | GENERIC_WRITE);
                    in != INVALID_HANDLE_VALUE) {
                    ::SetStdHandle(STD_INPUT_HANDLE, in);
                    (void)rebind_crt(stdin, in, _O_RDONLY);
                }
            }
            return true;
        }

        // 控制台是否可用，按“有没有真的控制台”判断，而不是靠编译期子系统宏：
        //   * Console 子系统：从资源管理器或终端启动都会带控制台 → GetConsoleWindow() 非空；
        //   * Windows 子系统：默认没有控制台；从终端启动时用 AttachConsole 挂上父控制台，
        //     再把标准流重定向过去（见 open_console_streams）。
        // 但如果 stdout 已经被重定向到文件/管道（GetFileType 是 DISK/PIPE），就不要抢过来改，
        // 否则 `GameSnap --help > out.txt` 会写进控制台而不是文件。
        // 返回值表示 **stdout** 可用（日志 sink 写的就是它）；不可用时调用方不要加控制台 sink。
        bool ensure_console_available() {
            const HANDLE stdout_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
            if (stdout_handle != nullptr && stdout_handle != INVALID_HANDLE_VALUE) {
                const DWORD type = ::GetFileType(stdout_handle);
                // DISK / PIPE：真的是重定向，保留句柄不动。
                if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)
                    return true;
                // 真控制台句柄：GetConsoleMode 能成功（也就意味着有读权限，spdlog 才认）。
                DWORD console_mode = 0;
                if (::GetConsoleMode(stdout_handle, &console_mode) != 0)
                    return true;
            }
            // AttachConsole 会把 STD_* 换成控制台句柄（哪怕原来是用户重定向的文件/管道），
            // 而且不保证旧句柄还开着——所以先复制一份再 attach，还原的是这份副本。
            const HANDLE saved_stderr = duplicate_handle(::GetStdHandle(STD_ERROR_HANDLE));
            const bool stderr_redirected = is_redirect_handle(saved_stderr);
            const HANDLE saved_stdin = duplicate_handle(::GetStdHandle(STD_INPUT_HANDLE));
            const bool stdin_redirected = is_redirect_handle(saved_stdin);

            if (::GetConsoleWindow() == nullptr && ::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE)
                return false;

            // 走到这里说明要么刚 attach（GUI 进程没有标准句柄），要么句柄是只可写/无效的。
            // 统一重新打开一份读写的控制台句柄：stdout 拿不到就当没有控制台。
            if (!open_console_streams(stderr_redirected, stdin_redirected)) {
                ::FreeConsole();
                return false;
            }
            // 还原用户的重定向：attach 把它们冲掉了，这里塞回去（CRT 流也一起）。
            // 句柄是 attach 前复制的副本，寿命到进程结束（SetStdHandle 存的就是它）。
            if (stderr_redirected) {
                ::SetStdHandle(STD_ERROR_HANDLE, saved_stderr);
                (void)rebind_crt(stderr, saved_stderr, _O_WRONLY);
            }
            if (stdin_redirected) {
                ::SetStdHandle(STD_INPUT_HANDLE, saved_stdin);
                (void)rebind_crt(stdin, saved_stdin, _O_RDONLY);
            }
            return true;
        }

        // 自动生成的默认配置：带注释，键值和 Config/CodecOptions 的默认值一致。
        // 用户改完重启即可生效（程序只在“一个配置都找不到”时才写这个文件）。
        constexpr std::string_view kDefaultConfigYaml = R"(# GameSnap 配置（首次启动自动生成，可随意修改后重启生效）
# 查找顺序：--config <路径> → exe 目录\config.yaml → 当前目录\config.yaml。

# ── 输出 ────────────────────────────────────────────────────────────────────
# 截图目录；不存在会自动创建。留空 = %USERPROFILE%\Pictures\GameSnap
# output_dir: D:\GameSnapShots

# 文件名模板，占位符：
#   {title}   截图时前台窗口的标题
#   {exe}     前台进程的 exe 名（不含扩展名）
#   {process} 前台进程的显示名（exe 的 FileDescription；拿不到描述时为 exe 名，
#             连进程都打不开（权限不足）时为空）
#   {time}    时间戳 YYYYMMDD_HHMMSS_mmm
#   {seq}     本次运行的截图序号（4 位）
# 例："{title}_{time}_{seq}"、"{process} {time}"、"{exe}-{seq}"
filename_pattern: "{title}_{time}_{seq}"

# ── 编码 ────────────────────────────────────────────────────────────────────
encode_threads: 2
format_hdr: avif
format_sdr: jxl

png_compression: 6
jpeg_quality: 95

webp_quality: 90
webp_lossless: false
webp_method: 4

avif_quality: 90
avif_lossless: false
avif_speed: 10

heic_quality: 90
heic_lossless: false
heic_preset: fast

# JXL：distance 0 = 数学无损（配合 jxl_lossless），effort 1..10，越高越慢越小
jxl_distance: 0.0
jxl_lossless: true
jxl_effort: 5

av1_encoder: auto
aom_cpu_used: -1
svt_preset: -1
chroma_subsampling: auto

# ── 色调映射 ────────────────────────────────────────────────────────────────
tone_map: spline
tone_map_backend: auto

# ── 采集 ────────────────────────────────────────────────────────────────────
hotkey: F12
swallow: false
monitor: primary

# 日志级别：false = info，true = debug
verbose: false
)";

        bool write_default_config(std::filesystem::path const& path) {
            // 先写临时文件再 rename：中途崩溃/断电不会留下半个 YAML，
            // 否则下次启动会把"配置有语法错误"报给用户。
            std::filesystem::path temporary = path;
            temporary += L".tmp";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (!file) {
                    SPDLOG_WARN("无法在 {} 生成默认配置（目录可能只读，例如装在 Program Files），本次使用内置默认值",
                                wstring_to_utf8(path.parent_path().wstring()));
                    return false;
                }
                file.write(kDefaultConfigYaml.data(), static_cast<std::streamsize>(kDefaultConfigYaml.size()));
                file.flush();
                if (!file.good()) {
                    file.close();
                    std::error_code ignored;
                    std::filesystem::remove(temporary, ignored);
                    SPDLOG_WARN("写默认配置失败: {}", wstring_to_utf8(temporary.wstring()));
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                // rename 失败（目标已存在/跨卷）就退回到覆盖写，至少别留 .tmp。
                std::filesystem::remove(temporary, ec);
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    SPDLOG_WARN("无法写入默认配置: {}", wstring_to_utf8(path.wstring()));
                    return false;
                }
                file.write(kDefaultConfigYaml.data(), static_cast<std::streamsize>(kDefaultConfigYaml.size()));
                file.flush();
                if (!file.good())
                    return false;
            }
            SPDLOG_INFO("未找到配置文件，已生成默认配置: {}", wstring_to_utf8(path.wstring()));
            return true;
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
            const std::wstring copy(text);
            wchar_t* end = nullptr;
            const long value = std::wcstol(copy.c_str(), &end, 0);
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

            // 123 / 0x7B 这种数字写法
            if (auto value = parse_int(lowered); value && *value > 0 && *value < 0x100)
                return static_cast<UINT>(*value);
            return std::nullopt;
        }

        std::optional<Hotkey> parse_hotkey(std::wstring_view text) {
            std::wstring lowered(text);
            std::ranges::transform(lowered, lowered.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });

            if (!lowered.empty() && lowered.back() == L'+')
                return std::nullopt;

            Hotkey result;
            bool have_key = false;
            std::size_t start = 0;
            while (start < lowered.size()) {
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
            constexpr std::array<std::string_view, 29> keys{
                "output_dir", "filename_pattern", "encode_threads", "hotkey", "swallow", "monitor",
                "format_hdr", "format_sdr", "tone_map", "tone_map_backend", "verbose",
                "png_compression", "jpeg_quality",
                "webp_quality", "webp_lossless", "webp_method",
                "avif_quality", "avif_lossless", "avif_speed",
                "heic_quality", "heic_lossless", "heic_preset",
                "jxl_distance", "jxl_lossless", "jxl_effort",
                "av1_encoder", "aom_cpu_used", "svt_preset",
                "chroma_subsampling"};
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
                    if (const auto pattern = read_key<std::string>(root, "filename_pattern"); pattern) {
                        const std::wstring text = utf8_to_wstring(*pattern);
                        if (text.empty())
                            SPDLOG_WARN("filename_pattern 为空，保持默认值");
                        else
                            parsed.filename_pattern = text;
                    }
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
                    if (const auto text = read_key<std::string>(root, "format_hdr"); text) {
                        const auto format = parse_image_format(*text);
                        if (!format || !format_supports_hdr(*format))
                            throw ConfigError(fmt::format("format_hdr 只支持 avif / heic / jxl，实际为 {}", *text));
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
                    if (const auto text = read_key<std::string>(root, "tone_map_backend"); text) {
                        if (*text == "auto")
                            parsed.tone_map_backend = ToneMapBackend::Auto;
                        else if (*text == "gpu")
                            parsed.tone_map_backend = ToneMapBackend::Gpu;
                        else if (*text == "cpu")
                            parsed.tone_map_backend = ToneMapBackend::Cpu;
                        else
                            throw ConfigError(
                                fmt::format("tone_map_backend 只支持 auto/gpu/cpu，实际为 {}", *text));
                    }
                    const auto read_range = [&](char const* key, int min_value, int max_value,
                                                int& target) {
                        if (const auto value = read_key<int>(root, key); value) {
                            if (*value < min_value || *value > max_value)
                                throw ConfigError(fmt::format("{} 必须在 {}..{} 之间", key, min_value, max_value));
                            target = *value;
                        }
                    };
                    read_range("png_compression", 0, 9, parsed.codec.png_compression);
                    read_range("jpeg_quality", 1, 100, parsed.codec.jpeg_quality);
                    read_range("webp_quality", 0, 100, parsed.codec.webp_quality);
                    read_range("webp_method", 0, 6, parsed.codec.webp_method);
                    read_range("avif_quality", 0, 100, parsed.codec.avif_quality);
                    read_range("avif_speed", 0, 10, parsed.codec.avif_speed);
                    read_range("heic_quality", 0, 100, parsed.codec.heic_quality);
                    if (const auto value = read_key<bool>(root, "webp_lossless"); value)
                        parsed.codec.webp_lossless = *value;
                    if (const auto value = read_key<bool>(root, "avif_lossless"); value)
                        parsed.codec.avif_lossless = *value;
                    if (const auto value = read_key<bool>(root, "heic_lossless"); value)
                        parsed.codec.heic_lossless = *value;
                    if (const auto text = read_key<std::string>(root, "heic_preset"); text) {
                        constexpr std::array<std::string_view, 9> presets{
                            "ultrafast", "superfast", "veryfast", "faster", "fast",
                            "medium", "slow", "slower", "veryslow"};
                        if (std::ranges::find(presets, *text) == presets.end())
                            throw ConfigError(fmt::format("无法识别的 heic_preset: {}", *text));
                        parsed.codec.heic_preset = *std::ranges::find(presets, *text);
                    }
                    if (const auto value = read_key<bool>(root, "verbose"); value)
                        parsed.verbose = *value;
                    // ── JXL ──
                    if (const auto value = read_key<float>(root, "jxl_distance"); value) {
                        if (!(*value >= 0.0F && *value <= 15.0F))
                            throw ConfigError("jxl_distance 必须在 0..15 之间");
                        parsed.codec.jxl_distance = *value;
                    }
                    if (const auto value = read_key<bool>(root, "jxl_lossless"); value)
                        parsed.codec.jxl_lossless = *value;
                    read_range("jxl_effort", 1, 10, parsed.codec.jxl_effort);
                    // ── AV1 编码库 ──
                    if (const auto text = read_key<std::string>(root, "av1_encoder"); text) {
                        const auto encoder = parse_av1_encoder(*text);
                        if (!encoder)
                            throw ConfigError(fmt::format("av1_encoder 只支持 auto / aom / svt，实际为 {}", *text));
                        parsed.codec.av1_encoder = *encoder;
                    }
                    read_range("aom_cpu_used", -1, 8, parsed.codec.aom_cpu_used);
                    read_range("svt_preset", -1, 13, parsed.codec.svt_preset);
                    // 色度采样：AVIF / HEIC / JPEG 生效（PNG/BMP/JXL/WebP 无损是 RGB，WebP 有损固定 4:2:0）。
                    if (const auto text = read_key<std::string>(root, "chroma_subsampling"); text) {
                        const auto chroma = parse_chroma_subsampling(*text);
                        if (!chroma)
                            throw ConfigError(
                                fmt::format("chroma_subsampling 只支持 auto / 444 / 422 / 420，实际为 {}", *text));
                        parsed.codec.chroma = *chroma;
                    }
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

    FilenamePlaceholders parse_filename_placeholders(std::wstring_view pattern) {
        FilenamePlaceholders placeholders;
        placeholders.title = pattern.find(L"{title}") != std::wstring_view::npos;
        placeholders.exe = pattern.find(L"{exe}") != std::wstring_view::npos;
        placeholders.process = pattern.find(L"{process}") != std::wstring_view::npos;
        placeholders.time = pattern.find(L"{time}") != std::wstring_view::npos;
        placeholders.seq = pattern.find(L"{seq}") != std::wstring_view::npos;
        return placeholders;
    }

    void write_console_utf8(std::string_view text, bool to_stderr) {
        if (text.empty())
            return;
        const HANDLE handle = ::GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            // 控制台句柄：用 WriteConsoleW 输出宽字符，绕开会话代码页
            // （cmd 默认 936，直接写 UTF-8 字节会变成乱码）。
            const int length = static_cast<int>(text.size());
            const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
            if (wide_length > 0) {
                std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
                if (::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), wide_length) ==
                    wide_length) {
                    DWORD written = 0;
                    if (::WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr) != 0)
                        return;
                }
            }
        }
        // 不是控制台（重定向到文件/管道）：按原始 UTF-8 字节写，交给下游解码。
        std::FILE* const file = to_stderr ? stderr : stdout;
        (void)std::fwrite(text.data(), 1, text.size(), file);
        (void)std::fflush(file);
    }

    // 不分配堆内存的版本：给 OOM 处理器和控制台控制处理器用。
    // 前者此时再 new 会直接抛 bad_alloc，后者有超时限制、也不该在 CRT 锁里纠缠。
    void write_console_utf8_fixed(std::string_view text, bool to_stderr) {
        constexpr std::size_t kStackChars = 512;
        wchar_t wide[kStackChars];
        const int length = static_cast<int>(std::min(text.size(), kStackChars - 1));
        const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide, kStackChars);
        const HANDLE handle = ::GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
            return;
        if (wide_length > 0) {
            DWORD written = 0;
            if (::WriteConsoleW(handle, wide, static_cast<DWORD>(wide_length), &written, nullptr) != 0)
                return;
        }
        // 重定向到文件/管道：直接写原始 UTF-8 字节，同样不走 CRT。
        DWORD written = 0;
        (void)::WriteFile(handle, text.data(), static_cast<DWORD>(length), &written, nullptr);
    }

    void init_logging() {
        std::vector<spdlog::sink_ptr> sinks;

        // 只有真的有控制台（Console 子系统，或 Windows 子系统从终端启动）才加控制台 sink；
        // 否则日志只进文件，避免往无效句柄写、也避免多创建一个看不见的控制台。
        const bool console = ensure_console_available();
        const HANDLE console_handle = console ? ::GetStdHandle(STD_OUTPUT_HANDLE) : nullptr;
        if (console)
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        const auto log_path = log_file_path();
        if (!probe_writable(log_path)) {
            const std::string message =
                fmt::format("GameSnap: 无法写入日志文件 {}，仅输出到控制台\n", log_path.string());
            write_console_utf8(message, true);
        } else {
            try {
                auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_path.wstring(), kLogFileSize, kLogFileCount);
                sinks.push_back(file_sink);
            } catch (std::exception const& e) {
                const std::string message =
                    fmt::format("GameSnap: cannot open log file ({}), logging to console only\n", e.what());
                write_console_utf8(message, true);
            } catch (...) {
                constexpr std::string_view message = "GameSnap: cannot open log file, logging to console only\n";
                write_console_utf8(message, true);
            }
        }

        // 两个 sink 都可能没建起来（无控制台 + 日志文件不可写）：这时至少给一个空 sink，
        // 否则 spdlog 的 logger 构造会崩。
        if (sinks.empty())
            sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());

        auto logger = std::make_shared<spdlog::logger>("gamesnap", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->set_pattern("[%T.%e] [%^%l%$] [%s:%# %!] %v");
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(2));
        if (console) {
            // 放在 set_default_logger 之后：之前还没装好 sink，这条会丢。
            // 用 info：用户最常问的就是“终端里怎么没有日志”，这行直接回答。
            const DWORD type = ::GetFileType(console_handle);
            DWORD console_mode = 0;
            // GetConsoleMode 能成，spdlog 才会当它是控制台（打颜色 + WriteConsoleW）。
            const BOOL console_ok = ::GetConsoleMode(console_handle, &console_mode);
            SPDLOG_INFO("{}（STD_OUTPUT_HANDLE={:#x}, GetConsoleMode={}）",
                        type == FILE_TYPE_CHAR   ? "控制台输出已启用"
                        : type == FILE_TYPE_PIPE ? "标准输出已重定向到管道"
                        : type == FILE_TYPE_DISK ? "标准输出已重定向到文件"
                                                 : "标准输出句柄类型未知（FILE_TYPE_UNKNOWN）",
                        reinterpret_cast<std::uintptr_t>(console_handle), static_cast<int>(console_ok));
        }
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
            // 这里已经 OOM：不能再走会分配 std::wstring 的版本，用不分配的固定版本。
            write_console_utf8_fixed(message, true);

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
            if (auto logger = spdlog::default_logger())
                logger->flush();
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
        // 用 SHGetFolderPathW 而不是 SHGetKnownFolderPath：这个函数在解析命令行时就会调用，
        // 那时 COM 还没初始化（ComApartment 在 run() 里），后者会直接失败回退到当前目录。
        std::array<wchar_t, MAX_PATH> pictures{};
        std::filesystem::path base;
        if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures.data())))
            base = pictures.data();
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
            ("filename-pattern", "文件名模板，占位符 {title} {exe} {process} {time} {seq}",
             cxxopts::value<std::string>())
            ("threads", "编码线程数 1..8", cxxopts::value<int>())
            ("monitor", "primary / foreground", cxxopts::value<std::string>())
            ("swallow", "吞掉热键（游戏收不到）")
            ("format-hdr", "HDR 容器 avif / heic / jxl", cxxopts::value<std::string>())
            ("format-sdr", "SDR 格式 bmp/png/jpeg/webp/avif/heic/jxl", cxxopts::value<std::string>())
            ("tone-map", "色调映射 spline / analysis", cxxopts::value<std::string>())
            ("verbose", "输出调试日志")
            ("selftest", "用合成纹理跑编码链路自检")
            ("bench", "编码基准：给一张 PNG 跑格式矩阵；给一个目录则用当前配置逐张编码并算压缩率",
             cxxopts::value<std::string>())
            ("bench-scale", "基准：把源缩放到指定高度（例如 1080）再编码",
             cxxopts::value<int>())
            ("bench-verify", "基准：JXL 无损编码后解码回来逐像素校验")
            ("h,help", "显示帮助");
        // clang-format on

        const auto print_help = [&options] {
            std::string help = options.help();
            help += "\n手柄：按 Xbox Share 键触发截图。\n退出：Ctrl+C。日志位于 exe 目录下的 logs\\GameSnap.log。\n";
            write_console_utf8(help);
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
            if (config_path.empty()) {
                // 一处配置都没找到：在 exe 目录生成一份带注释的默认配置，方便用户直接改。
                const auto created = exe_config;
                if (write_default_config(created))
                    config_path = created;
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
        if (result.count("filename-pattern") != 0) {
            const std::wstring pattern = utf8_to_wstring(result["filename-pattern"].as<std::string>());
            if (pattern.empty()) {
                SPDLOG_ERROR("--filename-pattern 不能为空");
                return std::nullopt;
            }
            config.filename_pattern = pattern;
        }
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
        if (result.count("format-hdr") != 0) {
            const auto format = parse_image_format(result["format-hdr"].as<std::string>());
            if (!format || !format_supports_hdr(*format)) {
                SPDLOG_ERROR("--format-hdr 只支持 avif / heic / jxl");
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
                SPDLOG_ERROR("--tone-map 只支持 spline / analysis");
                return std::nullopt;
            }
            config.tone_map = *algorithm;
        }
        if (result.count("verbose") != 0)
            config.verbose = true;
        if (result.count("selftest") != 0)
            config.selftest = true;

        if (result.count("bench") != 0) {
            const std::filesystem::path source = utf8_to_wstring(result["bench"].as<std::string>());
            std::error_code ec;
            if (!std::filesystem::exists(source, ec)) {
                SPDLOG_ERROR("--bench 指定的源文件不存在: {}", wstring_to_utf8(source.wstring()));
                return std::nullopt;
            }
            config.bench_source = source;
        }
        if (result.count("bench-scale") != 0) {
            const int scale = result["bench-scale"].as<int>();
            if (scale <= 0 || scale > 4320) {
                SPDLOG_ERROR("--bench-scale 只支持 1..4320");
                return std::nullopt;
            }
            config.bench_scale_height = static_cast<std::uint32_t>(scale);
        }
        if (result.count("bench-verify") != 0)
            config.bench_verify = true;

        config.encode_threads = std::clamp(config.encode_threads, 1, 8);
        return config;
    }

    void log_config(Config const& config) {
        SPDLOG_INFO("输出目录: {}", wstring_to_utf8(config.output_dir.wstring()));
        SPDLOG_INFO("文件名模板: {}", wstring_to_utf8(config.filename_pattern));
        const std::string_view backend = config.tone_map_backend == ToneMapBackend::Cpu   ? "cpu"
                                         : config.tone_map_backend == ToneMapBackend::Gpu ? "gpu"
                                                                                          : "auto";
        SPDLOG_INFO("输出格式: HDR={} SDR={}，色调映射={}，计算后端={}",
                    to_string(config.hdr_format), to_string(config.sdr_format), to_string(config.tone_map),
                    backend);
        SPDLOG_INFO("截图热键: {} ({}吞掉)",
                    hotkey_name({config.hotkey_vk, config.hotkey_modifiers}),
                    config.hotkey_swallow ? "" : "不");
        SPDLOG_INFO("目标显示器: {}", to_string(config.monitor));
        SPDLOG_INFO("编码线程: {}", config.encode_threads);
        SPDLOG_INFO("编码参数: PNG compression={} JPEG quality={} WebP quality={} lossless={} method={}",
                    config.codec.png_compression, config.codec.jpeg_quality,
                    config.codec.webp_quality, config.codec.webp_lossless, config.codec.webp_method);
        SPDLOG_INFO(
            "编码参数: AVIF quality={} lossless={} speed={} 编码库={} aom_cpu_used={} svt_preset={}；"
            "HEIC quality={} lossless={} preset={}；色度采样={}",
            config.codec.avif_quality, config.codec.avif_lossless, config.codec.avif_speed,
            to_string(config.codec.av1_encoder), config.codec.aom_cpu_used, config.codec.svt_preset,
            config.codec.heic_quality, config.codec.heic_lossless, config.codec.heic_preset,
            to_string(config.codec.chroma));
        SPDLOG_INFO("编码参数: JXL distance={:.2f} lossless={} effort={}",
                    config.codec.jxl_distance, config.codec.jxl_lossless, config.codec.jxl_effort);
        SPDLOG_INFO("采集模式: 按需(热键时 StartCapture)，HDR 由目标显示器决定");
    }

}  // namespace gamesnap
