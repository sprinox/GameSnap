#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 截图线程 + Windows.Graphics.Capture 资源的唯一所有者。
//
// 并发约定（这是整个项目最关键的约束）：
//   1. CaptureResources / Direct3D11CaptureFramePool / GraphicsCaptureSession
//      由 capture 线程创建、关闭；内部用 mutex 保护句柄。建 pool/会话在锁内完成
//      （此时新 pool 还没有回调，不会阻塞 FrameArrived），其余 COM 调用在锁外。
//   2. WGC 的 FrameArrived 回调运行在 frame pool 的工作线程上（同一 pool 串行投递），
//      只做“取帧 → CopyResource 到 staging → 入队”；Map/Unmap 在编码线程。
//   3. 每张截图新建一套 frame pool + 会话，帧处理完直接关闭，不复用 pool。
//   4. 资源以“代(generation)”为单位整体替换：先建好新一代，成功后才换掉旧的。
// ─────────────────────────────────────────────────────────────────────────────

#include <latch>
#include <semaphore>

#include "common.h"
#include "d3d.h"
#include "display.h"
#include "encoder.h"
#include "messages.h"

namespace gamesnap {

    class CaptureResources;

    // 一次截图的上下文，由 FrameSink 持有当前 shot。
    struct ShotContext {
        std::shared_ptr<D3DContext> d3d;
        std::shared_ptr<EncodeQueue> jobs;
        std::shared_ptr<ID3D11Texture2D> staging;
        std::binary_semaphore done{0};  // 回调处理完（或失败）后 release
        Clock::time_point enqueued;
        Clock::time_point start_call;
        std::uint64_t sequence = 0;
        std::wstring title;
        DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        std::uint32_t sdr_white_nits = 80;
    };

    // FrameArrived 回调只碰这个对象：取走当前 shot，在回调里完成拷贝/入队，
    // 最后 release done。回调对同一个 pool 是串行的，mutex_ 只用于和 capture 线程同步。
    class FrameSink {
    public:
        void arm(std::shared_ptr<ShotContext> shot);
        void disarm();
        void on_frame(winrt::Direct3D11CaptureFramePool const& pool);

    private:
        std::mutex mutex_;
        std::shared_ptr<ShotContext> shot_;
    };

    class CaptureResources {
    public:
        ~CaptureResources();
        CaptureResources(CaptureResources const&) = delete;
        CaptureResources& operator=(CaptureResources const&) = delete;
        CaptureResources(CaptureResources&&) = delete;
        CaptureResources& operator=(CaptureResources&&) = delete;

        static std::shared_ptr<CaptureResources> create(HMONITOR monitor,
                                                        std::shared_ptr<D3DContext> const& d3d,
                                                        winrt::DirectXPixelFormat format,
                                                        std::string& error);

        void start_capture();
        void stop_capture() noexcept;
        void close();

        [[nodiscard]] bool running() const;
        [[nodiscard]] winrt::Windows::Graphics::SizeInt32 item_size() const noexcept { return size_; }
        std::shared_ptr<FrameSink> const& sink() const noexcept { return sink_; }

        // 只能由 FrameArrived 回调调用。
        winrt::Direct3D11CaptureFrame try_get_frame();

    private:
        CaptureResources() = default;

        // 调用方必须持有 mutex_（或对象尚未发布）。
        // 每次截图都换一个全新的 frame pool：同一个 pool 关闭会话后再建会话，
        // 在部分驱动上会偶发 0x8000FFFF（WGC 内部状态没收干净）。
        void create_pool();
        void create_session();

        mutable std::mutex mutex_;

        winrt::IDirect3DDevice device_{nullptr};
        winrt::DirectXPixelFormat format_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        winrt::GraphicsCaptureItem item_{nullptr};
        winrt::Direct3D11CaptureFramePool pool_{nullptr};
        winrt::GraphicsCaptureSession session_{nullptr};
        winrt::event_token frame_token_{};
        winrt::Windows::Graphics::SizeInt32 size_{.Width = 0, .Height = 0};
        std::shared_ptr<FrameSink> sink_;
        bool running_ = false;
        bool closed_ = false;
    };

    class CaptureWorker {
    public:
        CaptureWorker(Config config,
                      std::shared_ptr<CommandQueue> commands,
                      std::shared_ptr<EncodeQueue> jobs,
                      std::shared_ptr<StopSource> stop);

        // 线程入口：所有资源都只在这个线程里创建/销毁。
        void run();

        // 启动时资源就创建失败（例如权限不足）：调用方应尽快退出。
        [[nodiscard]] bool fatal_error() const noexcept { return fatal_error_.load(std::memory_order_relaxed); }
        // 等待启动阶段的资源初始化结束（一次性门闩，初始化完成或失败都会放行）。
        void wait_ready() { ready_.wait(); }

    private:
        bool ensure_ready();
        bool rebuild(ReloadReason reason);
        void handle_command(Command const& command);
        void shot(Command const& command);

        Config config_;
        std::shared_ptr<CommandQueue> commands_;
        std::shared_ptr<EncodeQueue> jobs_;
        std::shared_ptr<StopSource> stop_;
        moodycamel::ConsumerToken command_token_;

        // 当前“资源代”，只由 capture 线程读写。
        std::shared_ptr<D3DContext> d3d_;
        std::shared_ptr<CaptureResources> resources_;
        std::shared_ptr<ID3D11Texture2D> staging_;  // 唯一的主 staging 纹理
        DisplayInfo display_{};
        HMONITOR monitor_ = nullptr;
        std::uint64_t generation_ = 0;
        std::uint64_t sequence_ = 0;
        std::atomic<bool> fatal_error_{false};
        std::latch ready_{1};  // 一次性门闩：1 表示还没初始化完
    };

}  // namespace gamesnap
