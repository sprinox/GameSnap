#include "pch.h"

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
#include <filesystem>
#include <vector>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")

// NOLINTBEGIN: Win32/D3D/ImGui glue uses aggregate initialization by design.
namespace gamesnap {
    namespace {

        constexpr wchar_t kOverlayClass[] = L"GameSnap.Overlay";
        constexpr auto kMinShowTime = std::chrono::milliseconds(1200);
        constexpr float kBasePanelWidth = 440.0F;
        constexpr float kBasePanelHeight = 210.0F;
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
            swap_chain_->Present(1, 0);
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

    }  // namespace

    OverlayThread::OverlayThread(std::shared_ptr<UiBridge> bridge) : bridge_(std::move(bridge)) {}

    OverlayThread::~OverlayThread() {
        try {
            stop();
            join();
        } catch (...) {  // NOLINT(bugprone-empty-catch): 析构函数不能抛
        }
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
    }

    void OverlayThread::join() {
        if (thread_.joinable())
            thread_.join();
    }

    void OverlayThread::run() {
        ComApartment apartment;
        if (!apartment.ok())
            SPDLOG_WARN("overlay 线程初始化 COM 失败: {}", describe_hresult(apartment.result()));

        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = overlay_window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kOverlayClass;
        if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            SPDLOG_ERROR("注册 overlay 窗口类失败: {}", ::GetLastError());
            return;
        }

        HWND hwnd = ::CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                0x00200000 /* NOREDIRECTIONBITMAP */,
            kOverlayClass, L"GameSnap Overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (hwnd == nullptr) {
            SPDLOG_ERROR("创建 overlay 窗口失败: {}", ::GetLastError());
            return;
        }

        ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        const float scale = dpi_scale(hwnd);
        const std::uint32_t panel_width = static_cast<std::uint32_t>(kBasePanelWidth * scale);
        const std::uint32_t panel_height = static_cast<std::uint32_t>(kBasePanelHeight * scale);

        OverlayDevice device;
        if (!device.initialize(hwnd, panel_width, panel_height)) {
            SPDLOG_ERROR("overlay D3D/DirectComposition 初始化失败，面板和提示音不可用");
            ::DestroyWindow(hwnd);
            return;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendPlatformName = "GameSnapOverlay";
        if (!ImGui_ImplDX11_Init(device.device(), device.context())) {
            SPDLOG_ERROR("ImGui DX11 backend 初始化失败");
            ImGui::DestroyContext();
            device.shutdown();
            ::DestroyWindow(hwnd);
            return;
        }

        ImFont* font = nullptr;
        const std::filesystem::path font_path = L"C:\\Windows\\Fonts\\msyh.ttc";
        if (std::filesystem::exists(font_path)) {
            font = io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 18.0F * scale, nullptr,
                                                io.Fonts->GetGlyphRangesChineseFull());
        }
        if (font == nullptr)
            font = io.Fonts->AddFontDefaultVector();
        ImGui::GetStyle().FontSizeBase = 18.0F * scale;

        SPDLOG_INFO("overlay 就绪: ImGui DX11 backend + DirectComposition");
        AudioPlayer audio;

        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        ::GetMonitorInfoW(monitor, &monitor_info);
        const float margin = kBaseMargin * scale;
        const float visible_x = static_cast<float>(monitor_info.rcWork.right - panel_width) - margin;
        const float hidden_x = static_cast<float>(monitor_info.rcWork.right) + 8.0F;
        const int panel_y =
            monitor_info.rcWork.bottom - static_cast<int>(panel_height) - static_cast<int>(margin);
        float panel_x = hidden_x;
        bool shown = false;
        bool animating = false;
        float anim_from = hidden_x;
        float anim_to = hidden_x;
        std::chrono::steady_clock::time_point anim_start{};

        std::uint64_t last_trigger = bridge_->snapshot().trigger_serial;
        std::uint64_t last_info = 0;
        auto last_trigger_time = std::chrono::steady_clock::now();
        auto last_frame = std::chrono::steady_clock::now();

        while (!stop_.load(std::memory_order_acquire)) {
            MSG message;
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) > 0) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }

            const auto now = std::chrono::steady_clock::now();
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
                audio.play();
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

            if (!want_visible && !shown && !animating && panel_x >= hidden_x - 1.0F) {
                ::ShowWindow(hwnd, SW_HIDE);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            ::SetWindowPos(hwnd, HWND_TOPMOST, static_cast<int>(panel_x), panel_y,
                           static_cast<int>(panel_width), static_cast<int>(panel_height),
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);

            if (snapshot.info_serial != last_info) {
                last_info = snapshot.info_serial;
                device.set_thumbnail(snapshot.thumbnail, snapshot.thumb_width, snapshot.thumb_height);
            }

            io.DisplaySize = ImVec2(static_cast<float>(panel_width), static_cast<float>(panel_height));
            io.DeltaTime = delta > 0.0F ? delta : 1.0F / 60.0F;
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();

            ImDrawList* background = ImGui::GetBackgroundDrawList();
            background->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(panel_width),
                                                        static_cast<float>(panel_height)),
                                      IM_COL32(18, 20, 26, 205), 14.0F * scale);
            background->AddRect(ImVec2(1, 1), ImVec2(static_cast<float>(panel_width) - 1,
                                                       static_cast<float>(panel_height) - 1),
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
            if (device.thumbnail_view() != nullptr) {
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                                 reinterpret_cast<std::uintptr_t>(device.thumbnail_view()))),
                             ImVec2(thumb_width, thumb_height));
            } else {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImGui::GetCursorScreenPos(),
                    ImVec2(ImGui::GetCursorScreenPos().x + thumb_width,
                           ImGui::GetCursorScreenPos().y + thumb_height),
                    IM_COL32(45, 48, 58, 255), 6.0F * scale);
                ImGui::Dummy(ImVec2(thumb_width, thumb_height));
            }

            ImGui::SetCursorPos(ImVec2(padding + thumb_width + 14.0F * scale, padding));
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
            ImGui::TextUnformatted("GameSnap");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            const std::string filename = snapshot.filename.empty()
                                             ? "正在生成截图…"
                                             : wstring_to_utf8(snapshot.filename);
            ImGui::PushTextWrapPos(static_cast<float>(panel_width) - padding);
            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (snapshot.remaining > 0)
                ImGui::Text("剩余编码任务: %d", snapshot.remaining);
            else
                ImGui::TextUnformatted("编码完成");
            ImGui::EndGroup();
            ImGui::End();
            ImGui::PopFont();

            ImGui::Render();
            device.begin_frame();
            device.end_frame();
        }

        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        device.shutdown();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(kOverlayClass, instance);
    }

}  // namespace gamesnap
// NOLINTEND
