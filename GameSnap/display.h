#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 显示器枚举 / 选择 / DXGI 输出查询。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"

namespace gamesnap {

    struct DxgiOutputInfo {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        winrt::com_ptr<IDXGIOutput6> output6;  // 老系统/驱动上可能为空
        DXGI_OUTPUT_DESC desc{};
        DXGI_OUTPUT_DESC1 desc1{};
        bool has_desc1 = false;
    };

    struct DisplayInfo {
        HMONITOR monitor = nullptr;
        std::wstring device_name;  // \\.\DISPLAY1
        RECT desktop_rect{};
        bool primary = false;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t refresh_hz = 0;
        DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
        bool hdr = false;
        DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        std::uint32_t bits_per_color = 8;
        std::uint32_t sdr_white_nits = 80;
        std::wstring adapter_name;
    };

    bool monitor_alive(HMONITOR monitor);
    // 失败时返回的 DisplayInfo.monitor 为 nullptr。
    DisplayInfo describe_monitor(HMONITOR monitor);
    DisplayInfo select_monitor(MonitorChoice choice);
    // 失败时返回的 DxgiOutputInfo.adapter 为空。
    DxgiOutputInfo find_dxgi_output(HMONITOR monitor);

    bool is_hdr_color_space(DXGI_COLOR_SPACE_TYPE color_space);
    std::string_view to_string(DXGI_COLOR_SPACE_TYPE color_space);
    void log_display_info(DisplayInfo const& info);

}  // namespace gamesnap
