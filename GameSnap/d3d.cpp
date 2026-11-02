#include "pch.h"

#include "d3d.h"

#include "display.h"

namespace gamesnap {
    namespace {

        std::string hresult_text(char const* what, HRESULT hr) {
            return fmt::format("{} 失败: {}", what, describe_hresult(hr));
        }

        constexpr std::array<D3D_FEATURE_LEVEL, 4> kFeatureLevels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        constexpr UINT kFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    }  // namespace

    std::shared_ptr<D3DContext> D3DContext::create(HMONITOR monitor, std::string* error) {  // error cannot be nullptr
        auto fail = [error](std::string message) -> std::shared_ptr<D3DContext> {
            *error = std::move(message);
            return nullptr;
        };

        try {
            auto context = std::shared_ptr<D3DContext>(new D3DContext());

            const HRESULT factory_hr = ::CreateDXGIFactory1(IID_PPV_ARGS(context->factory_.put()));
            if (FAILED(factory_hr))
                return fail(hresult_text("CreateDXGIFactory1", factory_hr));

            // 目标显示器所在的适配器。WGC 的帧来自这个适配器，用同一张卡最省事。
            if (const auto output = find_dxgi_output(monitor); output.adapter)
                context->adapter_ = output.adapter;

            D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
            HRESULT hr = E_FAIL;

            if (context->adapter_) {
                hr = ::D3D11CreateDevice(
                    context->adapter_.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    kFlags, kFeatureLevels.data(), static_cast<UINT>(kFeatureLevels.size()), D3D11_SDK_VERSION,
                    context->device_.put(), &feature_level, context->context_.put());
                if (FAILED(hr)) {
                    SPDLOG_WARN("在目标适配器上创建设备失败 (0x{:08X})，回退到默认硬件设备",
                                static_cast<std::uint32_t>(hr));
                    context->adapter_ = nullptr;
                    context->device_ = nullptr;
                    context->context_ = nullptr;
                }
            }

            if (!context->adapter_) {
                hr = ::D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    kFlags, kFeatureLevels.data(), static_cast<UINT>(kFeatureLevels.size()), D3D11_SDK_VERSION,
                    context->device_.put(), &feature_level, context->context_.put());
                if (FAILED(hr))
                    return fail(hresult_text("D3D11CreateDevice", hr));

                // 反查适配器，仅用于日志与后续描述。
                if (auto dxgi_device = context->device_.try_as<IDXGIDevice>()) {
                    winrt::com_ptr<IDXGIAdapter> adapter;
                    if (SUCCEEDED(dxgi_device->GetAdapter(adapter.put())))
                        context->adapter_ = adapter.try_as<IDXGIAdapter1>();
                }
            }

            auto dxgi_device = context->device_.try_as<IDXGIDevice>();
            if (!dxgi_device)
                return fail("ID3D11Device 不支持 IDXGIDevice");

            winrt::com_ptr<IInspectable> inspectable;
            hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put());
            if (FAILED(hr))
                return fail(hresult_text("CreateDirect3D11DeviceFromDXGIDevice", hr));

            context->winrt_device_ = inspectable.as<winrt::IDirect3DDevice>();
            if (!context->winrt_device_)
                return fail("无法获取 IDirect3DDevice");

            context->print_hardware_info();
            return context;
        } catch (winrt::hresult_error const& e) {
            return fail(fmt::format("创建设备时 WinRT 异常: {}", describe_hresult(e.code())));
        } catch (std::exception const& e) {
            return fail(std::string("创建设备时异常: ") + e.what());
        }
    }

    bool D3DContext::device_removed() const {
        if (!device_)
            return true;
        return device_->GetDeviceRemovedReason() != S_OK;
    }

    bool D3DContext::factory_current() const {
        if (!factory_)
            return false;
        // 注意：IsCurrent() 返回 BOOL（TRUE 表示适配器没变），不是 HRESULT。
        return factory_->IsCurrent() != FALSE;
    }

    void D3DContext::print_hardware_info() const {
        if (!adapter_) {
            SPDLOG_WARN("无法获取 DXGI 适配器");
            return;
        }

        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter_->GetDesc(&desc))) {
            SPDLOG_WARN("GetDesc 失败");
            return;
        }

        SPDLOG_INFO("显卡: {} (厂商 0x{:04X}, 设备 0x{:04X})",
                    wstring_to_utf8(&desc.Description[0]), desc.VendorId, desc.DeviceId);
        SPDLOG_INFO("显存: 专用 {:.2f} GB / 共享 {:.2f} GB",
                    static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0),
                    static_cast<double>(desc.SharedSystemMemory) / (1024.0 * 1024.0 * 1024.0));
    }

}  // namespace gamesnap
