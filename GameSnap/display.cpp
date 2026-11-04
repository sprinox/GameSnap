#include "pch.h"

#include "display.h"

#include <cmath>
#include <cstring>

namespace gamesnap {
    namespace {

        std::uint32_t query_sdr_white_nits(std::wstring const& gdi_device_name) {
            UINT32 path_count = 0;
            UINT32 mode_count = 0;
            if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
                return 80;

            std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
            if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
                return 80;

            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{
                    .header = {
                        .type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                        .size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME),
                        .adapterId = paths[i].sourceInfo.adapterId,
                        .id = paths[i].sourceInfo.id,
                    },
                };
                if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
                    continue;
                if (::_wcsicmp(&source.viewGdiDeviceName[0], gdi_device_name.c_str()) != 0)
                    continue;

                DISPLAYCONFIG_SDR_WHITE_LEVEL level{
                    .header = {
                        .type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
                        .size = sizeof(DISPLAYCONFIG_SDR_WHITE_LEVEL),
                        .adapterId = paths[i].targetInfo.adapterId,
                        .id = paths[i].targetInfo.id,
                    },
                };
                if (::DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS)
                    continue;

                // SDRWhiteLevel 的单位是 1/1000 个 80 nit。
                const auto nits = static_cast<std::uint32_t>(std::lround(level.SDRWhiteLevel * 80.0 / 1000.0));
                if (nits >= 20 && nits <= 10000)
                    return nits;
            }
            return 80;
        }

        void log_output_desc1(DXGI_OUTPUT_DESC1 const& desc1) {
            SPDLOG_DEBUG("DXGI 输出: {} rect=[{},{},{},{}] 色彩空间={} (0x{:08X}) 位深={} 亮度=[{:.4f}, {:.4f}] nit",
                         wstring_to_utf8(&desc1.DeviceName[0]),
                         desc1.DesktopCoordinates.left, desc1.DesktopCoordinates.top,
                         desc1.DesktopCoordinates.right, desc1.DesktopCoordinates.bottom,
                         to_string(desc1.ColorSpace), static_cast<std::uint32_t>(desc1.ColorSpace),
                         desc1.BitsPerColor, desc1.MinLuminance, desc1.MaxLuminance);
        }

    }  // namespace

    bool is_hdr_color_space(DXGI_COLOR_SPACE_TYPE color_space) {
        switch (color_space) {
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:       // scRGB
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:    // HDR10 / PQ
        case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:  // HDR10 / PQ (studio)
            return true;
        default:
            return false;
        }
    }

    std::string_view to_string(DXGI_COLOR_SPACE_TYPE color_space) {
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

    DxgiOutputInfo find_dxgi_output(HMONITOR monitor) {
        winrt::com_ptr<IDXGIFactory1> factory;
        if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
            return {};

        for (UINT adapter_index = 0;; ++adapter_index) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            HRESULT hr = factory->EnumAdapters1(adapter_index, adapter.put());
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
                break;

            for (UINT output_index = 0;; ++output_index) {
                winrt::com_ptr<IDXGIOutput> output;
                hr = adapter->EnumOutputs(output_index, output.put());
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(hr))
                    break;

                DXGI_OUTPUT_DESC desc{};
                if (FAILED(output->GetDesc(&desc)))
                    continue;
                if (desc.Monitor != monitor)
                    continue;

                DxgiOutputInfo info;
                info.adapter = adapter;
                info.output6 = output.try_as<IDXGIOutput6>();
                if (info.output6 && SUCCEEDED(info.output6->GetDesc1(&info.desc1))) {
                    info.has_desc1 = true;
                    log_output_desc1(info.desc1);
                }
                return info;
            }
        }
        return {};
    }

    bool monitor_alive(HMONITOR monitor) {
        if (monitor == nullptr)
            return false;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        return ::GetMonitorInfoW(monitor, &info) != FALSE;
    }

    DisplayInfo describe_monitor(HMONITOR monitor) {
        MONITORINFOEXW monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (::GetMonitorInfoW(monitor, &monitor_info) == FALSE)
            return {};

        DisplayInfo info;
        info.monitor = monitor;
        info.device_name = &monitor_info.szDevice[0];
        info.desktop_rect = monitor_info.rcMonitor;
        info.primary = (monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        info.width = static_cast<std::uint32_t>(info.desktop_rect.right - info.desktop_rect.left);
        info.height = static_cast<std::uint32_t>(info.desktop_rect.bottom - info.desktop_rect.top);

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (::EnumDisplaySettingsW(&monitor_info.szDevice[0], ENUM_CURRENT_SETTINGS, &mode) != FALSE) {
            if (mode.dmDisplayFrequency > 1)
                info.refresh_hz = mode.dmDisplayFrequency;
        }

        const auto output = find_dxgi_output(monitor);
        if (output.adapter) {
            DXGI_ADAPTER_DESC adapter_desc{};
            if (SUCCEEDED(output.adapter->GetDesc(&adapter_desc)))
                info.adapter_name = &adapter_desc.Description[0];
        }
        if (output.has_desc1) {
            info.rotation = output.desc1.Rotation;
            info.color_space = output.desc1.ColorSpace;
            info.bits_per_color = output.desc1.BitsPerColor;
            // 显示器是 HDR（PQ/scRGB）才需要处理 FP16 帧。
            // 注意：色域/量子点信息这里不展开，输出统一按 sRGB 处理。
            info.hdr = is_hdr_color_space(info.color_space);
        }

        info.sdr_white_nits = query_sdr_white_nits(info.device_name);
        return info;
    }

    DisplayInfo select_monitor(MonitorChoice choice) {
        HMONITOR monitor = nullptr;

        if (choice == MonitorChoice::Foreground) {
            if (HWND foreground = ::GetForegroundWindow(); foreground != nullptr)
                monitor = ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
        }

        if (monitor == nullptr)
            monitor = ::MonitorFromPoint(POINT{.x = 0, .y = 0}, MONITOR_DEFAULTTOPRIMARY);

        if (monitor == nullptr)
            return {};

        return describe_monitor(monitor);
    }

    void log_display_info(DisplayInfo const& info) {
        SPDLOG_INFO("目标显示器: {} [{}x{} @ {}Hz] 位置=({}, {}) 主显示器={} 旋转={}",
                    wstring_to_utf8(info.device_name), info.width, info.height, info.refresh_hz,
                    info.desktop_rect.left, info.desktop_rect.top, info.primary ? "是" : "否",
                    static_cast<int>(info.rotation));
        SPDLOG_INFO("显示适配器: {}；色彩空间: {}；色深={}bit；HDR={}；SDR 白点={} nit",
                    wstring_to_utf8(info.adapter_name), to_string(info.color_space), info.bits_per_color,
                    info.hdr ? "是" : "否", info.sdr_white_nits);
    }

}  // namespace gamesnap
