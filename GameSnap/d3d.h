#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// D3D11 设备上下文。每个“资源代(gen)”持有一个共享的 D3DContext：
//   * 捕获线程用它做 CopyResource；
//   * 编码线程用它做 Map/Unmap。
// D3D11 的 immediate context 不是线程安全的，所以所有 context 调用
// （CopyResource / Map / Unmap / Dispatch / GetData 等）都通过 context_mutex() 串行化。
// Map 返回后的 CPU 读取不持锁；Unmap 必须在资源再次被 GPU 使用前完成。
// 编码任务持有 shared_ptr<D3DContext>，因此设备重建时旧设备不会在使用中被释放。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"

#include <d3d11_4.h>

namespace gamesnap {

    class D3DContext {
    public:
        // 优先在目标显示器所属的适配器上创建（多显卡笔记本上很重要）。
        static std::shared_ptr<D3DContext> create(HMONITOR monitor, std::string* error);

        [[nodiscard]] ID3D11Device* device() const noexcept { return device_.get(); }
        [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.get(); }
        [[nodiscard]] winrt::IDirect3DDevice const& winrt_device() const noexcept { return winrt_device_; }

        std::mutex& context_mutex() noexcept { return context_mutex_; }

        // 调用方必须持有 context_mutex；返回 0 表示不支持 fence。
        std::uint64_t signal_fence_locked();
        bool wait_fence(std::uint64_t value, std::chrono::milliseconds timeout, std::string& error);
        // DXGI 工厂失效：显示拓扑或适配器集合发生变化。
        [[nodiscard]] bool factory_current() const;
        // GetDeviceRemovedReason 失败：TDR / 驱动重启 / 显卡被拔掉。
        [[nodiscard]] bool device_removed() const;

        void print_hardware_info() const;

    private:
        D3DContext() = default;

        winrt::com_ptr<ID3D11Device> device_;
        winrt::com_ptr<ID3D11DeviceContext> context_;
        winrt::com_ptr<IDXGIFactory1> factory_;
        winrt::com_ptr<IDXGIAdapter1> adapter_;
        winrt::IDirect3DDevice winrt_device_{nullptr};
        std::mutex context_mutex_;
        winrt::com_ptr<ID3D11Fence> fence_;
        winrt::com_ptr<ID3D11DeviceContext4> context4_;
        // 只在持有 context_mutex 时自增，不需要原子量。
        std::uint64_t fence_value_ = 0;
    };

}  // namespace gamesnap
