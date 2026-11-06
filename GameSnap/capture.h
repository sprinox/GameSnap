#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 截图线程 + Windows.Graphics.Capture 资源的唯一所有者。
//
// 并发约定（这是整个项目最关键的约束）：
//   1. CaptureResources / Direct3D11CaptureFramePool / GraphicsCaptureSession
//      由 capture 线程创建、关闭；只在 capture 线程访问，不需要 mutex。
//      建 pool/会话时新 pool 还没有回调，不会阻塞 FrameArrived。
//   2. WGC 的 FrameArrived 回调运行在 frame pool 的工作线程上（FreeThreaded 可能并发）；
//      FrameSink 用 mutex 交接 shot，只做“取帧 → 拷贝到目标纹理 → 入队”；HDR 目标是
//      DEFAULT+SRV，SDR 目标是可 Map 的 staging，Map/Unmap 在编码线程。
//   3. 每张截图新建一套 frame pool + 会话，帧处理完直接关闭，不复用 pool。
//   4. 资源以“代(generation)”为单位整体替换：先建好新一代，成功后才换掉旧的。
// ─────────────────────────────────────────────────────────────────────────────

#include <condition_variable>
#include <latch>
#include <semaphore>

#include "common.h"
#include "d3d.h"
#include "display.h"
#include "encoder.h"
#include "messages.h"

namespace gamesnap {

    class CaptureResources;
    class UiBridge;

    // 一次截图的上下文，由 FrameSink 持有当前 shot。
    struct ShotContext {
        std::shared_ptr<D3DContext> d3d;
        std::shared_ptr<EncodeQueue> jobs;
        std::shared_ptr<UiBridge> ui;
        std::binary_semaphore done{0};                    // 回调处理完（或失败）后 release
        std::shared_ptr<std::atomic<bool>> canceled;      // 超时取消标记，和 EncodeJob 共享
        std::atomic<bool> failed{false};                  // CopyResource / 设备移除失败
        ShotTimings timings;                              // 各阶段时刻，随 EncodeJob 一起传递
        std::uint64_t sequence = 0;
        std::wstring title;
        DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        std::uint32_t sdr_white_nits = 80;
        bool hdr = false;
        ImageFormat hdr_format = ImageFormat::Avif;
        ImageFormat sdr_format = ImageFormat::Webp;
        ToneMapAlgorithm tone_map = ToneMapAlgorithm::Spline;
    };

    // FrameArrived 回调只碰这个对象：取走当前 shot，在回调里完成拷贝/入队。
    // 每个 pool 一个独立 sink；mutex_ 用于 capture 线程 arm/disarm 与回调交接 shot。
    class FrameSink {
    public:
        void arm(std::shared_ptr<ShotContext> shot);
        // 停止接收新 shot。若已 arm 但还没被回调取走，则把它标记为失败并 release done，
        // 让仍在等帧的调用方立刻返回，而不是白等满等帧超时。
        void disarm();
        void on_frame(winrt::Direct3D11CaptureFramePool const& pool);
        [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);

    private:
        std::mutex mutex_;
        std::condition_variable idle_cv_;
        std::shared_ptr<ShotContext> shot_;
        int in_flight_ = 0;
        bool accepting_ = false;
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

        // 建好本张截图要用的 pool + 会话（不 StartCapture），以便先 arm 再启动。
        void prepare();
        void start_capture();
        void stop_capture() noexcept;
        // 收尾顺序的唯一实现：停止接收新 shot → 注销 FrameArrived → 等在途回调退出。
        // 只有它在关闭会话和 pool 之前完成，Close() 才不会被在途回调撞上。
        // false = 回调没有在 timeout 内退出。
        [[nodiscard]] bool quiesce(std::chrono::milliseconds timeout);
        void close();
        // 回调卡死时放弃本代：只释放引用，不调用任何 Close()。
        void abandon() noexcept;

        [[nodiscard]] winrt::Windows::Graphics::SizeInt32 item_size() const noexcept { return size_; }
        [[nodiscard]] std::shared_ptr<FrameSink> const& sink() const noexcept { return sink_; }

    private:
        CaptureResources() = default;

        // 每次截图都换一个全新的 frame pool：同一个 pool 关闭会话后再建会话，
        // 在部分驱动上会偶发 0x8000FFFF（WGC 内部状态没收干净）。
        // 这些方法只在 capture 线程调用，资源对象不需要锁。
        void create_pool();
        void create_session();
        // 只注销 FrameArrived，不碰 sink；由 quiesce() 统一调用。
        void unregister_frame_arrived() noexcept;

        winrt::IDirect3DDevice device_{nullptr};
        winrt::DirectXPixelFormat format_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        winrt::GraphicsCaptureItem item_{nullptr};
        winrt::Direct3D11CaptureFramePool pool_{nullptr};
        winrt::GraphicsCaptureSession session_{nullptr};
        winrt::event_token frame_token_{};
        winrt::Windows::Graphics::SizeInt32 size_{.Width = 0, .Height = 0};
        std::shared_ptr<FrameSink> sink_;
        bool closed_ = false;
    };

    class CaptureWorker {
    public:
        CaptureWorker(Config config,
                      std::shared_ptr<CommandQueue> commands,
                      std::shared_ptr<EncodeQueue> jobs,
                      std::shared_ptr<StopSource> stop,
                      std::shared_ptr<UiBridge> ui);

        // 线程入口：所有资源都只在这个线程里创建/销毁。
        void run();

        // 启动时资源就创建失败（例如权限不足）：调用方应尽快退出。
        [[nodiscard]] bool fatal_error() const noexcept { return fatal_error_.load(std::memory_order_relaxed); }
        // 等待启动阶段的资源初始化结束（一次性门闩，初始化完成或失败都会放行）。
        void wait_ready() { ready_.wait(); }

    private:
        bool ensure_ready(ReloadReason reason);
        bool rebuild(ReloadReason reason, DisplayInfo const* known_display = nullptr);
        void handle_command(Command const& command);
        void shot(Command const& command);

        Config config_;
        std::shared_ptr<CommandQueue> commands_;
        std::shared_ptr<EncodeQueue> jobs_;
        std::shared_ptr<StopSource> stop_;
        std::shared_ptr<UiBridge> ui_;
        moodycamel::ConsumerToken command_token_;

        // 当前“资源代”，只由 capture 线程读写。
        std::shared_ptr<D3DContext> d3d_;
        std::shared_ptr<CaptureResources> resources_;
        DisplayInfo display_{};
        HMONITOR monitor_ = nullptr;
        std::uint64_t generation_ = 0;
        std::uint64_t sequence_ = 0;
        std::atomic<bool> fatal_error_{false};
        std::latch ready_{1};  // 一次性门闩：1 表示还没初始化完
    };

}  // namespace gamesnap
