#include "pch.h"

#include <set>

#include "overlay.h"

#include "ui_bridge.h"
#include "resource.h"

#include <dcomp.h>
#include <dxgi1_6.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")

// NOLINTBEGIN: Win32/D3D/ImGui glue uses aggregate initialization by design.
namespace gamesnap {
    namespace {

        constexpr wchar_t kOverlayClass[] = L"GameSnap.Overlay";
        constexpr auto kMinShowTime = std::chrono::milliseconds(1200);
        // 面板收起后继续保留窗口/设备/字体/播放器的时间：这段时间内再按热键直接复用，
        // 超过就整体释放，回到“闲置不占显存/内存”。
        constexpr auto kOverlayLinger = std::chrono::seconds(30);
        constexpr float kBasePanelWidth = 440.0F;
        // 内容高度 = 上下 padding 32 + 缩略图 90 = 122；再留一点余量和文件名换行的空间。
        constexpr float kBasePanelHeight = 150.0F;
        constexpr float kBaseMargin = 16.0F;

        LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            switch (message) {
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                ::BeginPaint(hwnd, &paint);
                ::EndPaint(hwnd, &paint);
                return 0;
            }
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
            default:
                return ::DefWindowProcW(hwnd, message, wparam, lparam);
            }
        }

        class OverlayDevice {
        public:
            bool initialize(HWND hwnd, std::uint32_t width, std::uint32_t height);
            void shutdown();
            void set_thumbnail(std::vector<std::uint8_t> const& pixels, std::uint32_t width,
                               std::uint32_t height);
            void begin_frame();
            void end_frame();

            [[nodiscard]] ID3D11Device* device() const noexcept { return device_.get(); }
            [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.get(); }
            [[nodiscard]] ID3D11ShaderResourceView* thumbnail_view() const noexcept {
                return thumbnail_view_.get();
            }
            [[nodiscard]] bool device_lost() const noexcept {
                return device_lost_.load(std::memory_order_relaxed);
            }
            // 设备还能不能用：Present 报过移除，或者驱动报告设备已被移除（TDR/显卡下线）。
            [[nodiscard]] bool healthy() const noexcept {
                if (device_lost_.load(std::memory_order_relaxed))
                    return false;
                return device_ != nullptr && SUCCEEDED(device_->GetDeviceRemovedReason());
            }

        private:
            bool create_swap_chain(HWND hwnd, std::uint32_t width, std::uint32_t height);
            bool create_render_target();
            void release_thumbnail();

            winrt::com_ptr<ID3D11Device> device_;
            winrt::com_ptr<ID3D11DeviceContext> context_;
            winrt::com_ptr<IDXGISwapChain1> swap_chain_;
            winrt::com_ptr<IDCompositionDevice> composition_;
            winrt::com_ptr<IDCompositionTarget> composition_target_;
            winrt::com_ptr<IDCompositionVisual> composition_visual_;
            winrt::com_ptr<ID3D11RenderTargetView> render_target_;
            winrt::com_ptr<ID3D11Texture2D> thumbnail_texture_;
            winrt::com_ptr<ID3D11ShaderResourceView> thumbnail_view_;
            // Present 报设备移除后就别再画了（否则一路失败空转到 linger 到期）。
            std::atomic<bool> device_lost_{false};
            std::uint32_t width_ = 0;
            std::uint32_t height_ = 0;
        };

        bool OverlayDevice::create_swap_chain(HWND hwnd, std::uint32_t width, std::uint32_t height) {
            winrt::com_ptr<IDXGIDevice> dxgi_device;
            if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), dxgi_device.put_void())))
                return false;
            winrt::com_ptr<IDXGIAdapter> adapter;
            if (FAILED(dxgi_device->GetAdapter(adapter.put())))
                return false;
            winrt::com_ptr<IDXGIFactory2> factory;
            if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void())))
                return false;

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width;
            desc.Height = height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            if (FAILED(factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr, swap_chain_.put())))
                return false;

            if (FAILED(::DCompositionCreateDevice(dxgi_device.get(), __uuidof(IDCompositionDevice),
                                                  composition_.put_void())))
                return false;
            if (FAILED(composition_->CreateTargetForHwnd(hwnd, TRUE, composition_target_.put())))
                return false;
            if (FAILED(composition_->CreateVisual(composition_visual_.put())))
                return false;
            if (FAILED(composition_visual_->SetContent(swap_chain_.get())))
                return false;
            if (FAILED(composition_target_->SetRoot(composition_visual_.get())))
                return false;
            return SUCCEEDED(composition_->Commit());
        }

        bool OverlayDevice::create_render_target() {
            render_target_ = nullptr;
            winrt::com_ptr<ID3D11Texture2D> back_buffer;
            if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void())))
                return false;
            return SUCCEEDED(device_->CreateRenderTargetView(back_buffer.get(), nullptr, render_target_.put()));
        }

        bool OverlayDevice::initialize(HWND hwnd, std::uint32_t width, std::uint32_t height) {
            D3D_FEATURE_LEVEL level{};
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                           device_.put(), &level, context_.put()))) {
                return false;
            }
            width_ = width;
            height_ = height;
            return create_swap_chain(hwnd, width, height) && create_render_target();
        }

        void OverlayDevice::shutdown() {
            release_thumbnail();
            composition_visual_ = nullptr;
            composition_target_ = nullptr;
            composition_ = nullptr;
            render_target_ = nullptr;
            swap_chain_ = nullptr;
            context_ = nullptr;
            device_ = nullptr;
        }

        void OverlayDevice::release_thumbnail() {
            thumbnail_view_ = nullptr;
            thumbnail_texture_ = nullptr;
        }

        void OverlayDevice::set_thumbnail(std::vector<std::uint8_t> const& pixels, std::uint32_t width,
                                          std::uint32_t height) {
            release_thumbnail();
            if (pixels.empty() || width == 0 || height == 0)
                return;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = pixels.data();
            data.SysMemPitch = width * 4;
            if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, thumbnail_texture_.put())))
                device_->CreateShaderResourceView(thumbnail_texture_.get(), nullptr, thumbnail_view_.put());
        }

        void OverlayDevice::begin_frame() {
            const float clear[4]{0.0F, 0.0F, 0.0F, 0.0F};
            context_->ClearRenderTargetView(render_target_.get(), clear);
            ID3D11RenderTargetView* rtv = render_target_.get();
            context_->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_), 0.0F, 1.0F};
            context_->RSSetViewports(1, &viewport);
        }

        void OverlayDevice::end_frame() {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            // Present 前解绑 RTV：DXGI 对"仍绑着 RTV 的 back buffer 做 Present"会告警，
            // 部分驱动上还会插入隐式同步。
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            const HRESULT presented = swap_chain_->Present(1, 0);
            if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
                // 设备没了就别再空转 Present，否则会一路失败到 linger 超时。
                device_lost_.store(true, std::memory_order_relaxed);
                SPDLOG_WARN("overlay Present 失败（设备移除）: {}", describe_hresult(presented));
            }
        }

        class AudioPlayer {
        public:
            ~AudioPlayer() {
                try {
                    if (player_)
                        player_.Close();
                } catch (...) {
                }
            }

            void play() {
                if (!player_ && !initialize())
                    return;
                try {
                    player_.PlaybackSession().Position(std::chrono::seconds(0));
                } catch (...) {
                }
                try {
                    player_.Play();
                } catch (winrt::hresult_error const& e) {
                    SPDLOG_WARN("播放 screenshot.mp3 失败: {}", describe_hresult(e.code()));
                } catch (...) {
                }
            }

        private:
            bool initialize() {
                std::vector<std::uint8_t> bytes;
                if (!load_resource(bytes))
                    return false;
                try {
                    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
                    winrt::Windows::Storage::Streams::DataWriter writer(stream);
                    writer.WriteBytes(winrt::array_view<std::uint8_t const>(bytes.data(), bytes.data() + bytes.size()));
                    writer.StoreAsync().get();
                    writer.DetachStream();
                    stream.Seek(0);
                    const auto source =
                        winrt::Windows::Media::Core::MediaSource::CreateFromStream(stream, L"audio/mpeg");
                    player_ = winrt::Windows::Media::Playback::MediaPlayer();
                    player_.Source(source);
                    SPDLOG_INFO("内嵌 screenshot.mp3 已加载，{} 字节", bytes.size());
                    return true;
                } catch (winrt::hresult_error const& e) {
                    SPDLOG_WARN("初始化 MediaPlayer 失败: {}", describe_hresult(e.code()));
                    return false;
                } catch (...) {
                    return false;
                }
            }

            static bool load_resource(std::vector<std::uint8_t>& bytes) {
                const HRSRC resource =
                    ::FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SCREENSHOT_MP3), RT_RCDATA);
                if (resource == nullptr)
                    return false;
                const HGLOBAL loaded = ::LoadResource(nullptr, resource);
                if (loaded == nullptr)
                    return false;
                const auto* data = static_cast<const std::uint8_t*>(::LockResource(loaded));
                const DWORD size = ::SizeofResource(nullptr, resource);
                if (data == nullptr || size == 0)
                    return false;
                bytes.assign(data, data + size);
                return true;
            }

            winrt::Windows::Media::Playback::MediaPlayer player_{nullptr};
        };

        float dpi_scale(HWND hwnd) {
            const UINT dpi = ::GetDpiForWindow(hwnd);
            return dpi == 0 ? 1.0F : static_cast<float>(dpi) / 96.0F;
        }

        // 和 capture 用同一套选屏规则：primary 跟随主显示器，foreground 才跟随前台窗口。
        HMONITOR pick_monitor(MonitorChoice choice) {
            if (choice == MonitorChoice::Foreground) {
                if (HWND foreground = ::GetForegroundWindow(); foreground != nullptr)
                    return ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
            }
            return ::MonitorFromPoint(POINT{.x = 0, .y = 0}, MONITOR_DEFAULTTOPRIMARY);
        }

        // 字体图集：固定文案 + 当前文件名的字形。文件名变化时重建（imgui 1.92 动态字体，
        // 后端会在 NewFrame 里更新纹理）。
        // 文件名里出现的码点是否都已经烘进图集（固定 UI 文案 + 之前见过的文件名）。
        bool glyphs_covered(std::set<ImWchar> const& covered, std::wstring const& filename) {
            for (wchar_t ch : filename) {
                const auto code = static_cast<ImWchar>(ch);
                if (code >= 0x20 && !covered.contains(code))
                    return false;
            }
            return true;
        }

        void remember_glyphs(std::set<ImWchar>& covered, std::wstring const& filename, ImVector<ImWchar> const& ranges) {
            // ranges 是 BuildRanges 的成对区间 [from, to]，把区间内全部标记为已覆盖。
            for (int i = 0; i + 1 < ranges.Size; i += 2) {
                for (ImWchar code = ranges.Data[i]; code <= ranges.Data[i + 1] && code != 0; ++code)
                    covered.insert(code);
            }
            for (wchar_t ch : filename)
                covered.insert(static_cast<ImWchar>(ch));
        }

        ImFont* build_font(float scale, std::wstring const& filename, std::set<ImWchar>& covered) {
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->Clear();
            // 图集刚被清空，之前记录的"已覆盖码点"随之失效——不清就会声称某些字形在，
            // 实际图集里没有（A→B→回到 A 时最容易踩）。
            covered.clear();
            ImFontGlyphRangesBuilder builder;
            builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
            builder.AddText("GameSnap正在生成截图…剩余编码任务: 编码完成");
            const std::string utf8_filename = wstring_to_utf8(filename);
            if (!utf8_filename.empty())
                builder.AddText(utf8_filename.c_str());
            ImVector<ImWchar> ranges;
            builder.BuildRanges(&ranges);

            // 字体路径不能写死 C: —— 系统盘可能不是 C:，非中文版 Windows 也未必有微软雅黑。
            // 按"有 CJK 字形优先"的顺序找；Segoe UI 只作为最后的中文缺字兜底，必须打 warning。
            std::filesystem::path font_path;
            bool cjk_capable = false;
            std::array<wchar_t, MAX_PATH> windows_dir{};
            if (::GetWindowsDirectoryW(windows_dir.data(), MAX_PATH) > 0) {
                const std::filesystem::path fonts_dir = std::filesystem::path(windows_dir.data()) / L"Fonts";
                for (const wchar_t* name : {L"msyh.ttc", L"msyhbd.ttc", L"simhei.ttf", L"simsun.ttc", L"segoeui.ttf"}) {
                    const std::filesystem::path candidate = fonts_dir / name;
                    std::error_code ec;
                    if (std::filesystem::exists(candidate, ec)) {
                        font_path = candidate;
                        cjk_capable = candidate.filename() != L"segoeui.ttf";
                        break;
                    }
                }
            }

            ImFont* result = nullptr;
            if (!font_path.empty()) {
                result = io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 18.0F * scale, nullptr,
                                                      ranges.Data);
                if (result != nullptr) {
                    SPDLOG_INFO("overlay 字体: {}", wstring_to_utf8(font_path.wstring()));
                    if (!cjk_capable) {
                        SPDLOG_WARN("{} 不含中文字形，overlay 的中文文案会显示为方框"
                                    "（装一次微软雅黑/黑体/宋体即可）",
                                    wstring_to_utf8(font_path.filename().wstring()));
                    }
                }
            }
            if (result == nullptr) {
                SPDLOG_WARN("加载字体失败（试过 微软雅黑/黑体/宋体/Segoe UI），overlay 中文可能显示为方框");
                result = io.Fonts->AddFontDefaultVector();
            }
            remember_glyphs(covered, filename, ranges);
            return result;
        }

    }  // namespace

    // 一次触发建好、linger 期间复用的 overlay 资源。窗口/设备/字体/播放器都在这里，
    // state_ 为空即表示已整体释放。
    struct OverlayState {
        HINSTANCE instance = nullptr;
        HWND hwnd = nullptr;
        std::unique_ptr<OverlayDevice> device;
        std::unique_ptr<AudioPlayer> audio;
        bool imgui_ready = false;
        ImFont* font = nullptr;
        std::wstring font_filename;
        // 已经烘进图集的码点。每次发布文件名都重建字体太贵（文件名里只有时间戳/序号在变），
        // 只有出现新字符时才重建。
        std::set<ImWchar> covered_glyphs;
        float scale = 1.0F;
        std::uint32_t panel_width = 0;
        std::uint32_t panel_height = 0;
    };

    OverlayThread::OverlayThread(std::shared_ptr<UiBridge> bridge, MonitorChoice monitor)
        : bridge_(std::move(bridge)), monitor_(monitor), stop_event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    OverlayThread::~OverlayThread() {
        try {
            stop();
            join();
        } catch (...) {  // NOLINT(bugprone-empty-catch): 析构函数不能抛
        }
        if (stop_event_ != nullptr)
            ::CloseHandle(stop_event_);
    }

    void OverlayThread::start() {
        thread_ = std::thread([this] {
            try {
                run();
            } catch (std::exception const& e) {
                SPDLOG_ERROR("overlay 线程退出: {}", e.what());
            } catch (...) {
                SPDLOG_ERROR("overlay 线程退出（未知异常）");
            }
        });
    }

    void OverlayThread::stop() {
        stop_.store(true, std::memory_order_release);
        if (stop_event_ != nullptr)
            ::SetEvent(stop_event_);
    }

    void OverlayThread::join() {
        if (thread_.joinable())
            thread_.join();
    }

    void OverlayThread::play_audio() {
        // 提示音优先：音频初始化（首次要读内嵌 MP3、建 MediaPlayer）与播放都排在
        // 窗口/设备/ImGui/字体之前，所以“咔”声不必等 UI 初始化完成。
        if (!state_)
            state_ = std::make_unique<OverlayState>();
        if (!state_->audio)
            state_->audio = std::make_unique<AudioPlayer>();
        state_->audio->play();
    }

    bool OverlayThread::ensure_ui() {
        if (!state_)
            state_ = std::make_unique<OverlayState>();

        if (state_->hwnd == nullptr) {
            const HINSTANCE instance = ::GetModuleHandleW(nullptr);
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = overlay_window_proc;
            window_class.hInstance = instance;
            window_class.lpszClassName = kOverlayClass;
            if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                SPDLOG_ERROR("注册 overlay 窗口类失败: {}", ::GetLastError());
                return false;
            }

            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (::GetMonitorInfoW(pick_monitor(monitor_), &monitor_info) == FALSE) {
                // 没有显示器（或句柄失效）：退化成屏幕尺寸，避免把窗口放到负坐标。
                monitor_info.rcWork = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                                           ::GetSystemMetrics(SM_CYSCREEN)};
            }

            // DirectComposition 负责合成：只需要 NOREDIRECTIONBITMAP，不要再叠 WS_EX_LAYERED
            // + LWA_ALPHA(255)，两者混用在部分 Windows 版本上会影响合成/透明度行为。
            const HWND hwnd = ::CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                    WS_EX_NOREDIRECTIONBITMAP,
                kOverlayClass, L"GameSnap Overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
            if (hwnd == nullptr) {
                SPDLOG_ERROR("创建 overlay 窗口失败: {}", ::GetLastError());
                ::UnregisterClassW(kOverlayClass, instance);
                return false;
            }
            state_->instance = instance;
            state_->hwnd = hwnd;
            // 面板常驻后会出现在下一次截图里：让 WGC/截图 API 排除本窗口。
            if (!::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
                SPDLOG_DEBUG("SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 失败: {}",
                             ::GetLastError());
            }
            // 先把 1x1 窗口放到目标显示器再取 DPI，避免沿用主显示器/旧窗口的 DPI。
            ::SetWindowPos(hwnd, HWND_TOPMOST, monitor_info.rcWork.right - 1, monitor_info.rcWork.bottom - 1, 1, 1,
                           SWP_NOACTIVATE);
        }

        const float scale = dpi_scale(state_->hwnd);
        const auto panel_width = static_cast<std::uint32_t>(kBasePanelWidth * scale);
        const auto panel_height = static_cast<std::uint32_t>(kBasePanelHeight * scale);
        if (state_->device && (scale != state_->scale || panel_width != state_->panel_width ||
                               panel_height != state_->panel_height)) {
            // 显示器或 DPI 变了：交换链和字体图集都要按新尺寸重建（窗口本身保留）。
            release_ui();
        }
        // 显卡下线 / TDR 后旧设备已经不可用：丢掉它，下面按需重建。
        // 不这么做的话，之后每次触发都会拿着死设备画一帧、Present 失败、面板再也出不来。
        if (state_->device && !state_->device->healthy()) {
            SPDLOG_WARN("overlay 的 D3D 设备已失效（显卡下线或驱动重置），释放后等待重建");
            release_ui();
        }
        state_->scale = scale;
        state_->panel_width = panel_width;
        state_->panel_height = panel_height;

        if (!state_->device) {
            auto device = std::make_unique<OverlayDevice>();
            if (!device->initialize(state_->hwnd, panel_width, panel_height)) {
                SPDLOG_ERROR("overlay D3D/DirectComposition 初始化失败，面板不可用（提示音不受影响）");
                return false;
            }
            state_->device = std::move(device);
        }

        if (!state_->imgui_ready) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendPlatformName = "GameSnapOverlay";
            if (!ImGui_ImplDX11_Init(state_->device->device(), state_->device->context())) {
                SPDLOG_ERROR("ImGui DX11 backend 初始化失败");
                ImGui::DestroyContext();
                return false;
            }
            ImGui::GetStyle().FontSizeBase = 18.0F * state_->scale;
            state_->imgui_ready = true;
            state_->font_filename = bridge_->snapshot().filename;
            state_->font = build_font(state_->scale, state_->font_filename, state_->covered_glyphs);
            SPDLOG_INFO("overlay 就绪（窗口/设备/字体/播放器常驻 {}s）", kOverlayLinger.count());
        }
        return true;
    }

    void OverlayThread::release_ui() {
        if (!state_)
            return;
        if (state_->imgui_ready) {
            ImGui_ImplDX11_Shutdown();
            ImGui::DestroyContext();
            state_->imgui_ready = false;
        }
        state_->font = nullptr;
        state_->font_filename.clear();
        if (state_->device) {
            state_->device->shutdown();
            state_->device.reset();
        }
    }

    void OverlayThread::release_all() {
        if (!state_)
            return;
        release_ui();
        if (state_->hwnd != nullptr) {
            ::DestroyWindow(state_->hwnd);  // 内部的 WM_DESTROY 会 PostQuitMessage，随后丢弃
            state_->hwnd = nullptr;
            MSG message;
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) > 0) {
            }
        }
        if (state_->instance != nullptr) {
            ::UnregisterClassW(kOverlayClass, state_->instance);
            state_->instance = nullptr;
        }
        state_.reset();  // 连同 AudioPlayer（MediaPlayer）一起释放
    }

    void OverlayThread::run() {
        // COM apartment 要覆盖整个线程：窗口、D3D、MediaPlayer 在 linger 期间一直活着。
        ComApartment apartment;
        if (!apartment.ok())
            SPDLOG_WARN("overlay 线程初始化 COM 失败: {}", describe_hresult(apartment.result()));

        std::uint64_t last_trigger = bridge_->snapshot().trigger_serial;
        auto idle_since = SteadyClock::now();
        while (!stop_.load(std::memory_order_acquire)) {
            // 没有保留的 overlay 资源时无限等待；有就等到 linger 到期。
            DWORD timeout = INFINITE;
            if (state_) {
                const auto idle = SteadyClock::now() - idle_since;
                if (idle >= kOverlayLinger) {
                    SPDLOG_INFO("overlay 闲置超过 {}s，释放窗口/设备/字体/播放器", kOverlayLinger.count());
                    release_all();
                    continue;
                }
                timeout = static_cast<DWORD>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(kOverlayLinger - idle).count()) +
                          1;
            }

            HANDLE handles[2]{bridge_->trigger_event(), stop_event_};
            const DWORD waited = (handles[0] != nullptr && stop_event_ != nullptr)
                                     ? ::WaitForMultipleObjects(2, handles, FALSE, timeout)
                                     : WAIT_FAILED;
            if (waited == WAIT_OBJECT_0 + 1)
                break;
            if (stop_.load(std::memory_order_acquire))
                break;
            if (waited == WAIT_TIMEOUT) {
                SPDLOG_INFO("overlay 闲置超过 {}s，释放窗口/设备/字体/播放器", kOverlayLinger.count());
                release_all();
                continue;
            }
            if (waited != WAIT_OBJECT_0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            const UiBridge::Snapshot snapshot = bridge_->snapshot();
            if (snapshot.trigger_serial == last_trigger)
                continue;

            play_audio();  // 先出声，再做 UI 初始化
            if (!ensure_ui()) {
                last_trigger = snapshot.trigger_serial;
                idle_since = SteadyClock::now();
                continue;
            }
            last_trigger = run_panel(snapshot.trigger_serial);
            idle_since = SteadyClock::now();
            // 面板里 Present 失败（设备移除）会在 run_panel 内 break；这里把死设备放掉，
            // 下次触发重新建，显卡恢复后能自愈。
            if (state_ && state_->device && !state_->device->healthy()) {
                SPDLOG_WARN("overlay 结束一帧时设备仍不可用，释放资源等待下次重建");
                release_ui();
            }
        }

        release_all();
    }

    std::uint64_t OverlayThread::run_panel(std::uint64_t trigger) {
        // 进入这里时 state_ 一定是 ensure_ui() 建好的；本函数只画一次面板动画，不建不拆资源。
        OverlayState& state = *state_;

        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (::GetMonitorInfoW(pick_monitor(monitor_), &monitor_info) == FALSE) {
            // 没有显示器时按屏幕尺寸兜底，面板位置仍有定义。
            monitor_info.rcWork = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                                       ::GetSystemMetrics(SM_CYSCREEN)};
        }

        const float scale = state.scale;
        const auto panel_width = state.panel_width;
        const auto panel_height = state.panel_height;
        ImGuiIO& io = ImGui::GetIO();
        ImFont* font = state.font;
        std::string filename_utf8 = wstring_to_utf8(state.font_filename);

        const float margin = kBaseMargin * scale;
        const float visible_x = static_cast<float>(monitor_info.rcWork.right - panel_width) - margin;
        const float hidden_x = static_cast<float>(monitor_info.rcWork.right) + 8.0F;
        const int panel_y =
            monitor_info.rcWork.bottom - static_cast<int>(panel_height) - static_cast<int>(margin);
        float panel_x = hidden_x;
        bool shown = true;
        bool animating = true;
        float anim_from = hidden_x;
        float anim_to = visible_x;
        const auto session_start = SteadyClock::now();
        auto anim_start = session_start;

        std::uint64_t last_trigger = trigger;
        std::uint64_t last_info = 0;
        std::shared_ptr<const UiBridge::Thumbnail> last_thumbnail;
        auto last_trigger_time = session_start;
        auto last_frame = session_start;

        while (!stop_.load(std::memory_order_acquire)) {
            MSG message;
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) > 0) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }

            const auto now = SteadyClock::now();
            const float delta = std::min(0.1F, std::chrono::duration<float>(now - last_frame).count());
            last_frame = now;
            const UiBridge::Snapshot snapshot = bridge_->snapshot();

            if (snapshot.trigger_serial != last_trigger) {
                last_trigger = snapshot.trigger_serial;
                last_trigger_time = now;
                shown = true;
                animating = true;
                anim_from = panel_x;
                anim_to = visible_x;
                anim_start = now;
                last_info = 0;
                // 面板还开着时又按了热键：这一次触发的提示音在这里补上。
                if (state.audio)
                    state.audio->play();
            }

            const bool min_time_elapsed = now - last_trigger_time >= kMinShowTime;
            const bool want_visible = shown && (snapshot.remaining > 0 || !min_time_elapsed);
            if (!want_visible && min_time_elapsed && shown) {
                shown = false;
                animating = true;
                anim_from = panel_x;
                anim_to = hidden_x;
                anim_start = now;
            }

            if (animating) {
                const float t = std::min(1.0F, std::chrono::duration<float>(now - anim_start).count() / 0.30F);
                const float eased = 1.0F - (1.0F - t) * (1.0F - t) * (1.0F - t);
                panel_x = anim_from + (anim_to - anim_from) * eased;
                if (t >= 1.0F)
                    animating = false;
            }

            if (!want_visible && !shown && !animating && panel_x >= hidden_x - 1.0F)
                break;

            ::SetWindowPos(state.hwnd, HWND_TOPMOST, static_cast<int>(panel_x), panel_y,
                           static_cast<int>(panel_width), static_cast<int>(panel_height),
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);

            if (snapshot.info_serial != last_info) {
                last_info = snapshot.info_serial;
                if (snapshot.filename != state.font_filename) {
                    state.font_filename = snapshot.filename;
                    filename_utf8 = wstring_to_utf8(state.font_filename);
                    // 文件名里只有时间戳/序号在变，字符集几乎不变：缺字形才重建图集。
                    if (!glyphs_covered(state.covered_glyphs, state.font_filename)) {
                        font = build_font(scale, state.font_filename, state.covered_glyphs);
                        state.font = font;
                    }
                }
                // 缩略图和文件名是分两次发布的（先图、写盘后补文件名），
                // 指针没变就不必重新上传纹理。
                if (snapshot.thumbnail != last_thumbnail) {
                    last_thumbnail = snapshot.thumbnail;
                    if (snapshot.thumbnail) {
                        state.device->set_thumbnail(snapshot.thumbnail->pixels, snapshot.thumbnail->width,
                                                    snapshot.thumbnail->height);
                    } else {
                        state.device->set_thumbnail({}, 0, 0);  // 新截图已开始但还没有缩略图：清掉上一张
                    }
                }
            }

            io.DisplaySize = ImVec2(static_cast<float>(panel_width), static_cast<float>(panel_height));
            io.DeltaTime = delta > 0.0F ? delta : 1.0F / 60.0F;
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();

            ImDrawList* background = ImGui::GetBackgroundDrawList();
            background->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(panel_width), static_cast<float>(panel_height)),
                                      IM_COL32(18, 20, 26, 205), 14.0F * scale);
            background->AddRect(ImVec2(1, 1), ImVec2(static_cast<float>(panel_width) - 1, static_cast<float>(panel_height) - 1),
                                IM_COL32(255, 255, 255, 32), 14.0F * scale, 0, 1.0F);

            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav;
            ImGui::PushFont(font, 18.0F * scale);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(panel_width), static_cast<float>(panel_height)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
            ImGui::Begin("##overlay", nullptr, flags);
            ImGui::PopStyleVar(2);

            const float padding = 16.0F * scale;
            const float thumb_width = 160.0F * scale;
            const float thumb_height = 90.0F * scale;
            ImGui::SetCursorPos(ImVec2(padding, padding));
            if (state.device->thumbnail_view() != nullptr) {
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                                 reinterpret_cast<std::uintptr_t>(state.device->thumbnail_view()))),
                             ImVec2(thumb_width, thumb_height));
            } else {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos(),
                    ImVec2(ImGui::GetCursorScreenPos().x + thumb_width,
                           ImGui::GetCursorScreenPos().y + thumb_height),
                    IM_COL32(45, 48, 58, 255), 6.0F * scale);
                ImGui::Dummy(ImVec2(thumb_width, thumb_height));
            }

            const float text_x = padding + thumb_width + 14.0F * scale;
            ImGui::SetCursorPos(ImVec2(text_x, padding));
            ImGui::BeginGroup();
            // 标题行：左边 GameSnap，右边编码状态。状态右对齐，放不下时退回短文案，
            // 避免和标题重叠。
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
            ImGui::TextUnformatted("GameSnap");
            ImGui::PopStyleColor();

            char status[64];
            char short_status[32];
            if (snapshot.remaining > 0) {
                std::snprintf(status, sizeof(status), "剩余编码任务: %d", snapshot.remaining);
                std::snprintf(short_status, sizeof(short_status), "剩余 %d", snapshot.remaining);
            } else {
                std::snprintf(status, sizeof(status), "编码完成");
                std::snprintf(short_status, sizeof(short_status), "编码完成");
            }
            const float title_width = ImGui::CalcTextSize("GameSnap").x;
            const float status_room =
                static_cast<float>(panel_width) - padding - text_x - title_width - 8.0F * scale;
            const char* status_text = ImGui::CalcTextSize(status).x <= status_room ? status : short_status;
            const float status_width = ImGui::CalcTextSize(status_text).x;

            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(text_x + title_width + 8.0F * scale,
                                          static_cast<float>(panel_width) - padding - status_width));
            ImGui::TextUnformatted(status_text);

            ImGui::Spacing();
            const std::string filename = snapshot.filename.empty() ? "正在生成截图…" : filename_utf8;
            ImGui::PushTextWrapPos(static_cast<float>(panel_width) - padding);
            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::End();
            ImGui::PopFont();

            ImGui::Render();
            state.device->begin_frame();
            state.device->end_frame();
            if (state.device->device_lost())
                break;  // 设备没了，直接退出面板循环，别再空转
        }

        // 收起后隐藏窗口：linger 期间不让 DWM 继续合成这个顶层表面，也不再 Present。
        ::ShowWindow(state.hwnd, SW_HIDE);
        return last_trigger;
    }

}  // namespace gamesnap
// NOLINTEND
