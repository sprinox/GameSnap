#include <expected>
#include <string>
#include <string_view>
#include <limits>
#include <memory>
#include <fstream>

#define SPDLOG_NO_EXCEPTIONS
#include <spdlog/spdlog.h>
#include <spdlog/fmt/std.h>
#define CPPTRACE_STATIC_DEFINE
#include <cpptrace/cpptrace.hpp>
#pragma comment(lib, "dbghelp.lib")
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>

#define NOMINMAX
#include <Unknwn.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>

std::string wstring_to_utf8(const wchar_t *str, size_t len = std::numeric_limits<size_t>::max()) {
    if (len == 0)
        return {};
    if (len == std::numeric_limits<size_t>::max())
        len = wcslen(str);

    int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        str,
        static_cast<int>(len),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (size <= 0)
        return {};

    std::string result(size, '\0');

    int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        str,
        static_cast<int>(len),
        result.data(),
        size,
        nullptr,
        nullptr);

    if (converted <= 0)
        return {};

    return result;
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty())
        return {};

    int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wstr.data(),
        static_cast<int>(wstr.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (size <= 0)
        return {};

    std::string result(size, '\0');

    int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wstr.data(),
        static_cast<int>(wstr.size()),
        result.data(),
        size,
        nullptr,
        nullptr);

    if (converted <= 0)
        return {};

    return result;
}

void error_report() {
    spdlog::critical("std::terminate is called");
    cpptrace::generate_trace().print_with_snippets();
    std::abort();
}

namespace winrt {
    using namespace ::winrt::Windows::Graphics::Capture;
    using namespace ::winrt::Windows::Graphics::DirectX;
    using namespace ::winrt::Windows::Graphics::DirectX::Direct3D11;
}
namespace Windows {
    using namespace ::Windows::Graphics::DirectX::Direct3D11;
}

struct D3DContext {
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::IDirect3DDevice winrt_device_{nullptr};
    static std::optional<D3DContext> create();
    void print_hardware_info() const;
};

void D3DContext::print_hardware_info() const {
    if (!device_) {
        spdlog::warn("empty ID3D11Device");
        return;
    }

    // 1. 查询 IDXGIDevice
    auto dxgi_device = device_.as<IDXGIDevice>();

    // 2. 获取所属的适配器 (Adapter)
    winrt::com_ptr<IDXGIAdapter> adapter;
    auto hr = dxgi_device->GetAdapter(adapter.put());
    if (FAILED(hr)) {
        auto e = winrt::hresult_error(hr);
        spdlog::error("dxgi_device->GetAdapter error: {}", wstring_to_utf8(e.message().c_str(), e.message().size()));
        return;
    }

    // 3. 查询适配器描述
    DXGI_ADAPTER_DESC desc = {};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) {
        auto e = winrt::hresult_error(hr);
        spdlog::error("adapter->GetDesc error: {}", wstring_to_utf8(e.message().c_str(), e.message().size()));
        return;
    }

    // 4. 打印信息
    spdlog::info("显卡名称: {}", wstring_to_utf8(desc.Description));
    spdlog::info("厂商ID (VendorId): 0x{0:x}", desc.VendorId);
    spdlog::info("设备ID (DeviceId): 0x{0:x}", desc.DeviceId);
    spdlog::info("子系统ID (SubSysId): 0x{0:x}", desc.SubSysId);
    spdlog::info("版本号 (Revision): 0x{0:x}", desc.Revision);
    spdlog::info("专用显存: {:03.2f} GB", desc.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
    spdlog::info("共享系统内存: {:03.2f} GB", desc.SharedSystemMemory / (1024.0 * 1024.0 * 1024.0));
}

std::optional<D3DContext> D3DContext::create() {
    try {
        D3DContext result;

        D3D_FEATURE_LEVEL feature_level;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            result.device_.put(),
            &feature_level,
            result.context_.put());

        if (FAILED(hr)) {
            auto e = winrt::hresult_error(hr);
            spdlog::error("D3D11CreateDevice error: {}", wstring_to_utf8(e.message().c_str(), e.message().size()));
            return std::nullopt;
        }

        // 将 DXGI Device 转换为 WinRT Direct3D Device
        auto dxgi_device = result.device_.as<IDXGIDevice>();

        winrt::com_ptr<IInspectable> inspectable;

        hr = CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.get(),
            inspectable.put());

        if (FAILED(hr)) {
            auto e = winrt::hresult_error(hr);
            spdlog::error("CreateDirect3D11DeviceFromDXGIDevice error: {}", wstring_to_utf8(e.message().c_str(), e.message().size()));
            return std::nullopt;
        }

        result.winrt_device_ =
            inspectable.as<winrt::IDirect3DDevice>();

        result.print_hardware_info();
        return result;
    } catch (winrt::hresult_error const& e) {
        spdlog::error("D3DContext::create error: {}", wstring_to_utf8(e.message().c_str(), e.message().size()));   
        return std::nullopt;
    }
    catch (std::exception const& e) {
        spdlog::error("D3DContext::create error: {}", e.what());
        return std::nullopt;
    }
    catch (...) {
        std::terminate();
        return std::nullopt;
    }
}

int monitor_enum_callback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MONITORINFOEX monitor_info = {};
    monitor_info.cbSize = sizeof(MONITORINFOEX);
    if (!GetMonitorInfo(hMonitor, &monitor_info)) {
        spdlog::warn("GetMonitorInfo error");
    }
    spdlog::info("显示器名称：{}", wstring_to_utf8(monitor_info.szDevice));
    spdlog::info("是否为主显示器：{}", (monitor_info.dwFlags & MONITORINFOF_PRIMARY) ? "是" : "否");
    const RECT& rc = monitor_info.rcMonitor;
    spdlog::info("屏幕位置：左上角 ({}, {})", rc.left, rc.top);
    spdlog::info("屏幕尺寸：{} x {}", rc.right - rc.left, rc.bottom - rc.top);

    DEVMODEW dev_mode = {};
    dev_mode.dmSize = sizeof(DEVMODEW);
    if (EnumDisplaySettings(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &dev_mode)) {
        spdlog::info("当前分辨率：{} x {}",
                     dev_mode.dmPelsWidth, dev_mode.dmPelsHeight);
        spdlog::info("刷新率：{} Hz", dev_mode.dmDisplayFrequency);
    } else {
        spdlog::warn("获取显示设置失败");
    }

    if (monitor_info.dwFlags & MONITORINFOF_PRIMARY) {
        *reinterpret_cast<HMONITOR*>(dwData) = hMonitor;
    }
    return TRUE;
}

constexpr std::string_view color_space_to_string(
    DXGI_COLOR_SPACE_TYPE color_space) noexcept {
    switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "RGB_FULL_G22_NONE_P709 (sRGB)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "RGB_FULL_G10_NONE_P709 (scRGB)";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return "RGB_STUDIO_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
        return "RGB_STUDIO_G22_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "RGB_FULL_G2084_NONE_P2020 (HDR10/PQ)";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "RGB_STUDIO_G2084_NONE_P2020 (HDR10/PQ)";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return "RGB_FULL_G22_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709:
        return "RGB_STUDIO_G24_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020:
        return "RGB_STUDIO_G24_NONE_P2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601:
        return "YCBCR_FULL_G22_NONE_P709_X601";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601:
        return "YCBCR_STUDIO_G22_LEFT_P601";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601:
        return "YCBCR_FULL_G22_LEFT_P601";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
        return "YCBCR_STUDIO_G22_LEFT_P709";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:
        return "YCBCR_FULL_G22_LEFT_P709";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020:
        return "YCBCR_STUDIO_G22_LEFT_P2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:
        return "YCBCR_FULL_G22_LEFT_P2020";
    default:
        return "UNKNOWN_COLOR_SPACE";
    }
}

void print_output_desc1(const DXGI_OUTPUT_DESC1& desc1) {
    spdlog::info("========== DXGI_OUTPUT_DESC1 ==========");

    spdlog::info("DeviceName: {}",
                 wstring_to_utf8(desc1.DeviceName));

    spdlog::info("DesktopCoordinates: left={}, top={}, right={}, bottom={}",
                 desc1.DesktopCoordinates.left,
                 desc1.DesktopCoordinates.top,
                 desc1.DesktopCoordinates.right,
                 desc1.DesktopCoordinates.bottom);

    spdlog::info("AttachedToDesktop: {}",
                 desc1.AttachedToDesktop ? "true" : "false");

    spdlog::info("Rotation: {}",
                 static_cast<unsigned int>(desc1.Rotation));

    spdlog::info("BitsPerColor: {}",
                 desc1.BitsPerColor);

    spdlog::info("ColorSpace: {} (0x{:08X})",
                 color_space_to_string(desc1.ColorSpace),
                 static_cast<unsigned int>(desc1.ColorSpace));

    spdlog::info("RedPrimary: ({:.6f}, {:.6f})",
                 desc1.RedPrimary[0], desc1.RedPrimary[1]);

    spdlog::info("GreenPrimary: ({:.6f}, {:.6f})",
                 desc1.GreenPrimary[0], desc1.GreenPrimary[1]);

    spdlog::info("BluePrimary: ({:.6f}, {:.6f})",
                 desc1.BluePrimary[0], desc1.BluePrimary[1]);

    spdlog::info("WhitePoint: ({:.6f}, {:.6f})",
                 desc1.WhitePoint[0], desc1.WhitePoint[1]);

    spdlog::info("MinLuminance: {:.6f} nits",
                 desc1.MinLuminance);

    spdlog::info("MaxLuminance: {:.6f} nits",
                 desc1.MaxLuminance);

    spdlog::info("MaxFullFrameLuminance: {:.6f} nits",
                 desc1.MaxFullFrameLuminance);

    spdlog::info("=======================================");
}

winrt::GraphicsCaptureItem CreateCaptureItemFromMonitor(HMONITOR monitor) {
    auto interop =
        winrt::get_activation_factory<
            winrt::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

    winrt::GraphicsCaptureItem item{nullptr};

    winrt::check_hresult(
        interop->CreateForMonitor(
            monitor,    
            winrt::guid_of<
                winrt::GraphicsCaptureItem>(),
            winrt::put_abi(item)));

    return item;
}
struct Payload {
    enum class Operation: std::uint8_t {
        Stop,
        Encode
    } op_;
    std::chrono::time_point<std::chrono::system_clock> snap_time_;
    winrt::com_ptr<ID3D11Texture2D> texture_;
};

struct CaptureEvent {
    std::mutex lock;
    std::condition_variable cv;
    enum class Status {
        NeedCreateResources,
        Stop,
        Running,
    } status = Status::NeedCreateResources;
    int catch_count = 0;
};

struct CaptureResources {
    std::shared_ptr<CaptureEvent> event_{new CaptureEvent()};
    std::shared_ptr<D3DContext> d3d_ = nullptr;
    winrt::Windows::Graphics::SizeInt32 last_size_{0, 0};
    winrt::Direct3D11CaptureFramePool frame_pool_ = nullptr;
    winrt::GraphicsCaptureSession session_ = nullptr;
    std::shared_ptr<moodycamel::BlockingConcurrentQueue<Payload>> queue_ = nullptr;
    static CaptureResources create(HMONITOR monitor, std::shared_ptr<moodycamel::BlockingConcurrentQueue<Payload>> queue, const std::shared_ptr<D3DContext>& d3d);
    void create_resources(HMONITOR monitor);
    void close_resources();
    CaptureResources(const CaptureResources&) = default;
    CaptureResources() = default;
};


bool is_monitor_hdr(HMONITOR monitor) {

    winrt::com_ptr<IDXGIFactory1> factory;

    winrt::check_hresult(
        CreateDXGIFactory1(
            IID_PPV_ARGS(&factory)));

    for (UINT adapter_index = 0;; ++adapter_index) {
        winrt::com_ptr<IDXGIAdapter1> adapter;

        auto hr = factory->EnumAdapters1(
            adapter_index, adapter.put());

        if (hr == DXGI_ERROR_NOT_FOUND)
            break;

        if (FAILED(hr))
            throw winrt::hresult_error(hr);

        for (UINT output_index = 0;; ++output_index) {
            winrt::com_ptr<IDXGIOutput> output;

            hr = adapter->EnumOutputs(
                output_index, output.put());

            if (hr == DXGI_ERROR_NOT_FOUND)
                break;

            if (FAILED(hr))
                throw winrt::hresult_error(hr);

            DXGI_OUTPUT_DESC base_desc;

            hr = output->GetDesc(&base_desc);

            if (FAILED(hr))
                continue;

            // 先通过基础描述确认目标 HMONITOR。
            if (base_desc.Monitor != monitor)
                continue;

            // 获取包含色彩空间信息的 IDXGIOutput6。
            winrt::com_ptr<IDXGIOutput6> output6;

            output6 = output.as<IDXGIOutput6>();

            DXGI_OUTPUT_DESC1 desc1{};

            hr = output6->GetDesc1(&desc1);

            if (FAILED(hr))
                throw winrt::hresult_error(hr);

            print_output_desc1(desc1);

            return desc1.ColorSpace ==
                   DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
    }

    // 未找到与 HMONITOR 对应的 DXGI 输出。
    throw std::exception("monitor's dxgi output not found");
}

CaptureResources CaptureResources::create(HMONITOR monitor, std::shared_ptr<moodycamel::BlockingConcurrentQueue<Payload>> queue, const std::shared_ptr<D3DContext>& d3d) {
    CaptureResources ret{};
    ret.d3d_ = d3d;
    ret.queue_ = queue;
    ret.create_resources(monitor);
    return ret;
}

void CaptureResources::close_resources() {
    // with lock
    session_.Close();
    frame_pool_.Close();
    last_size_ = {0, 0};
}

void CaptureResources::create_resources(HMONITOR monitor) {
    // with lock
    winrt::GraphicsCaptureItem item_ = CreateCaptureItemFromMonitor(monitor);
    last_size_ = item_.Size();
    auto pixel_format = is_monitor_hdr(monitor) ? winrt::DirectXPixelFormat::R16G16B16A16Float : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    auto frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d_->winrt_device_, pixel_format, 2, last_size_);
    winrt::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item_);
    frame_pool.FrameArrived([this](auto& frame_pool, auto&) {
        {
            std::lock_guard lk(this->event_->lock);
            if (this->event_.catch_count == 0) {
                this->close_resources();
                this->event_->status = CaptureEvent::Status::Stop;
                return;
            }
            this->event_.catch_count--;
        }
        
        winrt::Direct3D11CaptureFrame frame = frame_pool.TryGetNextFrame();

        if (!frame) {
            return;
        }

        auto access =
            frame.Surface().as<Windows::IDirect3DDxgiInterfaceAccess>();

        winrt::com_ptr<ID3D11Texture2D> source_texture;

        HRESULT hr = access->GetInterface(
            __uuidof(ID3D11Texture2D),
            source_texture.put_void());

        if (FAILED(hr)) // TODO
            throw winrt::hresult_error(hr);

        D3D11_TEXTURE2D_DESC desc{};
        source_texture->GetDesc(&desc);

        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Texture2D> staging_texture;

        hr = this->d3d_->device_->CreateTexture2D(
            &desc,
            nullptr,
            staging_texture.put());

        if (FAILED(hr))
            throw winrt::hresult_error(hr);

        this->d3d_->context_->CopyResource(
            staging_texture.get(),
            source_texture.get());
        this->queue_->enqueue(Payload(Payload::Operation::Encode, std::chrono::system_clock::now(), staging_texture));
        frame.Close();


    });
}


bool SaveStagingTextureAsBmp(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* stagingTexture,
    const wchar_t* filename) {
    if (!context || !stagingTexture || !filename)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    stagingTexture->GetDesc(&desc);

    // 这里只处理单张、非多重采样的 2D 纹理。
    if (desc.Width == 0 ||
        desc.Height == 0 ||
        desc.SampleDesc.Count != 1 ||
        desc.ArraySize != 1) {
        return false;
    }

    // 24 位 BMP 写入时，统一输出 BGR。
    const bool isBGRA =
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    const bool isRGBA =
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    if (!isBGRA && !isRGBA)
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};

    HRESULT hr = context->Map(
        stagingTexture,
        0,
        D3D11_MAP_READ,
        0,
        &mapped);

    if (FAILED(hr))
        return false;

    bool success = false;

    do {
        const uint64_t width = desc.Width;
        const uint64_t height = desc.Height;

        // BMP 每行必须按 4 字节对齐。
        const uint64_t bmpRowPitch = (width * 3 + 3) & ~uint64_t(3);
        const uint64_t imageSize = bmpRowPitch * height;
        const uint64_t fileSize =
            14 + 40 + imageSize;

        if (imageSize > std::numeric_limits<DWORD>::max() ||
            fileSize > std::numeric_limits<DWORD>::max()) {
            break;
        }

        std::vector<uint8_t> pixels(
            static_cast<size_t>(imageSize), 0);

        const auto* srcBase =
            static_cast<const uint8_t*>(mapped.pData);

        // BMP 使用正高度时，像素按 bottom-up 存储。
        for (uint32_t y = 0; y < desc.Height; ++y) {
            const uint8_t* src =
                srcBase + static_cast<size_t>(y) * mapped.RowPitch;

            uint8_t* dst =
                pixels.data() +
                static_cast<size_t>(desc.Height - 1 - y) *
                    static_cast<size_t>(bmpRowPitch);

            for (uint32_t x = 0; x < desc.Width; ++x) {
                const uint8_t* p = src + x * 4;
                uint8_t* q = dst + x * 3;

                if (isBGRA) {
                    q[0] = p[0];  // B
                    q[1] = p[1];  // G
                    q[2] = p[2];  // R
                } else {
                    q[0] = p[2];  // B
                    q[1] = p[1];  // G
                    q[2] = p[0];  // R
                }
            }
        }

        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4D42;  // "BM"
        fileHeader.bfOffBits =
            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = static_cast<DWORD>(fileSize);

        BITMAPINFOHEADER infoHeader{};
        infoHeader.biSize = sizeof(BITMAPINFOHEADER);
        infoHeader.biWidth = static_cast<LONG>(desc.Width);
        infoHeader.biHeight = static_cast<LONG>(desc.Height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 24;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = static_cast<DWORD>(imageSize);

        std::ofstream file(filename, std::ios::binary);
        if (!file)
            break;

        // 注意：Windows BMP 文件头是固定格式。
        // 不直接写 BITMAPFILEHEADER，避免结构体填充导致格式不正确。
        const uint16_t bfType = 0x4D42;
        const uint32_t bfSize = static_cast<uint32_t>(fileSize);
        const uint16_t reserved = 0;
        const uint32_t bfOffBits = 14 + 40;

        file.write(reinterpret_cast<const char*>(&bfType), sizeof(bfType));
        file.write(reinterpret_cast<const char*>(&bfSize), sizeof(bfSize));
        file.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
        file.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
        file.write(reinterpret_cast<const char*>(&bfOffBits), sizeof(bfOffBits));

        file.write(
            reinterpret_cast<const char*>(&infoHeader),
            sizeof(infoHeader));

        file.write(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));

        success = file.good();
    } while (false);

    context->Unmap(stagingTexture, 0);
    return success;
}

void encode_thread(std::shared_ptr<moodycamel::BlockingConcurrentQueue<Payload>> queue, std::shared_ptr<D3DContext> d3d) {
    spdlog::info("encode thread {} start", std::this_thread::get_id());
    auto token = moodycamel::ConsumerToken(*queue);
    Payload item;
    while (true) {
        queue->wait_dequeue(token, item);
        if (item.op_ == Payload::Operation::Stop) {
            spdlog::info("encode thread {} stop", std::this_thread::get_id());
            return;
        }
        std::wstring path = std::format(
            L"{:%F %T}",
            std::chrono::floor<std::chrono::milliseconds>(item.snap_time_));
        spdlog::info("save to {}", wstring_to_utf8(path));
        if (SaveStagingTextureAsBmp(d3d->context_.get(), item.texture_.get(), path.c_str())) {
            spdlog::info("successfully save to {}", wstring_to_utf8(path));
        } else {
            spdlog::warn("fail save to {}", wstring_to_utf8(path));
        }
    }
}

void capture_thread(std::shared_ptr<D3DContext> d3d) {
    
}

int run() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    spdlog::info("正在创建D3D设备");
    std::shared_ptr<D3DContext> d3d(new D3DContext());
    if (auto tmp = D3DContext::create(); tmp.has_value()) {
        *d3d = tmp.value();
    } else {
        spdlog::error("无法获取到主显示器");
        return 1;
    }
    spdlog::info("正在获取显示器句柄");
    HMONITOR target_monitor = nullptr;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_callback,
                        reinterpret_cast<LPARAM>(&target_monitor));
    if (!target_monitor) {
        spdlog::error("无法获取到主显示器");
        return 1;
    }
    std::shared_ptr<moodycamel::BlockingConcurrentQueue<Payload>> encode_queue;
    std::thread t(encode_thread, encode_queue, d3d);
    CaptureResources res = CaptureResources::create(target_monitor, encode_queue, d3d);
    res.session_.StartCapture();
    encode_queue->enqueue(Payload{.op_ = Payload::Operation::Stop});
    t.join();
    return 0;
}

int main() {
    std::set_terminate(error_report);
    try {
        return run();
    }
    catch (...) {
        std::terminate();
    }
    return 1;
}
