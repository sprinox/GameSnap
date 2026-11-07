#include "pch.h"

#include "capture.h"
#include "ui_bridge.h"

#include <windows.graphics.capture.interop.h>

#include <cstring>

namespace gamesnap {
    namespace {

        constexpr int kFramePoolBuffers = 1;
        constexpr auto kFrameWait = std::chrono::milliseconds(500);
        constexpr auto kCallbackWait = std::chrono::seconds(2);

        winrt::GraphicsCaptureItem create_item_for_monitor(HMONITOR monitor) {
            auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(interop->CreateForMonitor(
                monitor,
                winrt::guid_of<winrt::GraphicsCaptureItem>(),
                winrt::put_abi(item)));
            return item;
        }

        DXGI_FORMAT dxgi_format_of(winrt::DirectXPixelFormat format) {
            switch (format) {
            case winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized:
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            case winrt::DirectXPixelFormat::R16G16B16A16Float:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default:
                return DXGI_FORMAT_UNKNOWN;
            }
        }

        // WGC 帧的真实描述可能和创建时不同（旋转屏、驱动做格式转换等），
        // 这里只保留尺寸/格式，其余改成可 CPU 读取的 staging 用法。
        D3D11_TEXTURE2D_DESC make_staging_desc_from(D3D11_TEXTURE2D_DESC const& source) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = source.Width;
            desc.Height = source.Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = source.Format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            return desc;
        }

        // HDR 帧只给 GPU tone mapper 用：DEFAULT + SRV 可以直接绑定，
        // 不再在 tonemap 里整帧 CopyResource 到 input_。
        D3D11_TEXTURE2D_DESC make_gpu_desc_from(D3D11_TEXTURE2D_DESC const& source) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = source.Width;
            desc.Height = source.Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = source.Format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            return desc;
        }

        winrt::com_ptr<ID3D11Texture2D> create_gpu_texture(ID3D11Device* device,
                                                           D3D11_TEXTURE2D_DESC const& desc) {
            winrt::com_ptr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
                return {};
            return texture;
        }

        // 前台窗口标题，用作文件名前缀；拿不到就由编码侧回退到 GameSnap。
        std::wstring foreground_window_title() {
            const HWND window = ::GetForegroundWindow();
            if (window == nullptr)
                return {};
            const int length = ::GetWindowTextLengthW(window);
            if (length <= 0)
                return {};

            const int clamped = std::min(length, 512);
            std::wstring title(static_cast<std::size_t>(clamped) + 1, L'\0');
            const int copied = ::GetWindowTextW(window, title.data(), clamped);
            if (copied <= 0)
                return {};
            title.resize(static_cast<std::size_t>(copied));
            return title;
        }

        // FrameArrived 回调里只做 GPU 侧的事：取帧 → CopyResource 到 staging → 入队。
        // Map/Unmap（GPU→CPU）交给编码线程，回调线程不被整帧 memcpy 拖住。
        void handle_frame(winrt::Direct3D11CaptureFramePool const& pool, ShotContext& shot) {
            winrt::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
            if (!frame) {
                shot.failed.store(true, std::memory_order_release);
                return;
            }
            if (shot.canceled->load(std::memory_order_acquire)) {
                frame.Close();
                return;
            }
            const auto received = SteadyClock::now();
            shot.timings.frame_received = received;

            winrt::com_ptr<ID3D11Texture2D> source;
            try {
                auto access = frame.Surface().as<Windows::IDirect3DDxgiInterfaceAccess>();
                if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()))) {
                    shot.failed.store(true, std::memory_order_release);
                    frame.Close();
                    SPDLOG_ERROR("获取帧纹理失败");
                    return;
                }
            } catch (winrt::hresult_error const& e) {
                shot.failed.store(true, std::memory_order_release);
                frame.Close();
                SPDLOG_ERROR("获取帧纹理失败: {}", describe_hresult(e.code()));
                return;
            }

            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);

            const D3D11_TEXTURE2D_DESC wanted =
                shot.hdr ? make_gpu_desc_from(desc) : make_staging_desc_from(desc);
            auto target = shot.hdr ? create_gpu_texture(shot.d3d->device(), wanted)
                                   : create_staging_texture(shot.d3d->device(), wanted);
            if (!target) {
                shot.failed.store(true, std::memory_order_release);
                frame.Close();
                SPDLOG_ERROR("创建 {} 纹理失败 ({}x{}, fmt={})",
                             shot.hdr ? "HDR" : "staging", desc.Width, desc.Height,
                             static_cast<int>(desc.Format));
                return;
            }
            std::uint64_t gpu_fence = 0;
            {
                std::scoped_lock lock(shot.d3d->context_mutex());
                shot.d3d->context()->CopyResource(target.get(), source.get());
                gpu_fence = shot.d3d->signal_fence_locked();
            }
            frame.Close();
            // CopyResource 没有 HRESULT；TDR/拔卡时可能静默失败。GetDeviceRemovedReason 是
            // 唯一廉价的事后反馈，避免把旧内容或垃圾帧交给编码线程。
            if (FAILED(shot.d3d->device()->GetDeviceRemovedReason())) {
                shot.failed.store(true, std::memory_order_release);
                return;
            }
            const auto copied = SteadyClock::now();
            shot.timings.copied = copied;
            if (shot.canceled->load(std::memory_order_acquire))
                return;

            SPDLOG_DEBUG("[#{:04}] 采集 {}x{}: 命令排队 {:.2f}ms，StartCapture→首帧 {:.2f}ms，GPU 拷贝 {:.2f}ms",
                         shot.sequence, desc.Width, desc.Height,
                         duration_ms(shot.timings.start_capture - shot.timings.input),
                         duration_ms(received - shot.timings.start_capture),
                         duration_ms(copied - received));

            EncodeJob job;
            job.texture = std::move(target);
            job.gpu_fence = gpu_fence;
            job.d3d = shot.d3d;
            job.canceled = shot.canceled;
            job.hdr = shot.hdr;
            job.hdr_format = shot.hdr_format;
            job.sdr_format = shot.sdr_format;
            job.tone_map = shot.tone_map;
            job.color_space = shot.color_space;
            job.sdr_white_nits = shot.sdr_white_nits;
            job.sequence = shot.sequence;
            job.title = std::move(shot.title);
            job.local_time = Clock::now();
            job.timings = shot.timings;

            if (shot.ui)
                shot.ui->job_enqueued();  // 先计数，避免极快 job 先 finished 再 enqueued
            if (!shot.jobs->enqueue(std::move(job))) {
                if (shot.ui)
                    shot.ui->job_finished();
                SPDLOG_WARN("编码队列入队失败，丢弃本次截图");
                return;
            }
        }
    }

    std::string_view to_string(ReloadReason reason) {
        switch (reason) {
        case ReloadReason::Startup:
            return "首次创建";
        case ReloadReason::DisplayChange:
            return "显示设置变化";
        case ReloadReason::DeviceChange:
            return "显卡/显示拓扑变化";
        case ReloadReason::DeviceRemoved:
            return "D3D 设备被移除";
        case ReloadReason::MonitorLost:
            return "目标显示器消失";
        case ReloadReason::CaptureFailure:
            return "采集失败";
        case ReloadReason::PowerResume:
            return "从睡眠恢复";
        }
        return "未知";
    }

    // ── CaptureResources ─────────────────────────────────────────────────────

    CaptureResources::~CaptureResources() {
        try {
            close();
        } catch (...) {  // NOLINT(bugprone-empty-catch): 析构函数不能抛异常
        }
    }

    std::shared_ptr<CaptureResources> CaptureResources::create(HMONITOR monitor,
                                                               std::shared_ptr<D3DContext> const& d3d,
                                                               winrt::DirectXPixelFormat format,
                                                               std::string& error) {
        try {
            if (!d3d || !d3d->winrt_device())
                return nullptr;

            auto resources = std::shared_ptr<CaptureResources>(new CaptureResources());

            resources->item_ = create_item_for_monitor(monitor);
            resources->size_ = resources->item_.Size();
            if (resources->size_.Width <= 0 || resources->size_.Height <= 0) {
                error = "捕获区域尺寸无效";
                return nullptr;
            }
            resources->device_ = d3d->winrt_device();
            resources->format_ = format;

            // 第一次的 pool + 会话先建好；之后每张截图换新 pool。
            resources->create_pool();
            resources->create_session();

            return resources;
        } catch (winrt::hresult_error const& e) {
            error = fmt::format("创建捕获资源失败: {}", describe_hresult(e.code()));
            return nullptr;
        } catch (std::exception const& e) {
            error = fmt::format("创建捕获资源失败: {}", e.what());
            return nullptr;
        }
    }

    void CaptureResources::create_pool() {
        pool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, format_, kFramePoolBuffers, size_);
        // 每个 pool 一个独立 sink：旧 pool 的迟到回调不可能取走下一张截图的 shot。
        sink_ = std::make_shared<FrameSink>();
        auto sink = sink_;
        frame_token_ = pool_.FrameArrived(
            [sink](auto const& sender, auto const&) {
                sink->on_frame(sender);
            });
    }

    void CaptureResources::create_session() {
        session_ = pool_.CreateCaptureSession(item_);
        try {
            session_.IsCursorCaptureEnabled(false);  // 截图不包含鼠标指针
            session_.IsBorderRequired(false);        // Win11：去掉黄色捕获边框
        } catch (winrt::hresult_error const& e) {
            // 旧系统不支持这两个属性，不影响采集本身。
            SPDLOG_DEBUG("采集属性设置失败: {}", describe_hresult(e.code()));
        }
    }

    void CaptureResources::prepare() {
        if (closed_ || !item_ || !device_)
            throw winrt::hresult_error(RO_E_CLOSED, L"capture 资源已关闭");
        if (!pool_)
            create_pool();
        if (!session_)
            create_session();
    }

    void CaptureResources::start_capture() {
        if (closed_ || !session_)
            throw winrt::hresult_error(RO_E_CLOSED, L"capture 资源已关闭");
        session_.StartCapture();
    }

    void CaptureResources::unregister_frame_arrived() noexcept {
        auto token = std::exchange(frame_token_, {});
        auto pool = pool_;
        if (pool && token) {
            try {
                pool.FrameArrived(token);
            } catch (...) {  // NOLINT(bugprone-empty-catch): noexcept 收尾路径不能记日志
            }
        }
    }

    void CaptureResources::stop_capture() noexcept {
        auto session = std::exchange(session_, nullptr);
        auto pool = std::exchange(pool_, nullptr);
        auto token = std::exchange(frame_token_, {});

        // 注销 FrameArrived → 关会话 → 关 pool；下一张由 prepare() 建全新的。
        if (pool && token) {
            try {
                pool.FrameArrived(token);
            } catch (...) {  // NOLINT(bugprone-empty-catch): noexcept 收尾路径不能记日志
            }
        }
        if (session) {
            try {
                session.Close();
            } catch (...) {  // NOLINT(bugprone-empty-catch): noexcept 收尾路径不能记日志
            }
        }
        if (pool) {
            try {
                pool.Close();
            } catch (...) {  // NOLINT(bugprone-empty-catch): noexcept 收尾路径不能记日志
            }
        }
    }

    void CaptureResources::close() {
        if (closed_)
            return;
        // 先停止接收、注销回调并等在途回调退出，再 Close 会话和 pool。
        // 回调 2 秒不退出说明驱动卡住，此时不能 Close，直接放弃这一代。
        if (!quiesce(kCallbackWait)) {
            SPDLOG_WARN("关闭 capture 资源时 FrameArrived 回调未在 {}ms 内退出，放弃本代资源",
                        kCallbackWait.count());
            abandon();
            return;
        }
        closed_ = true;
        stop_capture();
        item_ = nullptr;
        sink_.reset();
    }

    bool CaptureResources::quiesce(std::chrono::milliseconds timeout) {
        // 顺序不能反：先挡住新 shot（并唤醒等帧的一方），再注销投递，最后等在途回调退出。
        if (sink_)
            sink_->disarm();
        unregister_frame_arrived();
        return sink_ ? sink_->wait_idle(timeout) : true;
    }

    void CaptureResources::abandon() noexcept {
        closed_ = true;
        (void)std::exchange(pool_, nullptr);
        (void)std::exchange(session_, nullptr);
        (void)std::exchange(item_, nullptr);
        (void)std::exchange(frame_token_, {});
        if (auto sink = std::exchange(sink_, nullptr))
            sink->disarm();
        // 不调用 Close：回调可能仍持有 pool 的 sender 引用，强行关闭会违反 WGC 约束。
        // 句柄引用在这里释放，等回调自己的引用结束后由 COM 回收。
    }

    // ── FrameSink ────────────────────────────────────────────────────────────

    void FrameSink::arm(std::shared_ptr<ShotContext> shot) {
        std::scoped_lock lock(mutex_);
        accepting_ = true;
        shot_ = std::move(shot);
    }

    void FrameSink::disarm() {
        std::shared_ptr<ShotContext> dropped;
        {
            std::scoped_lock lock(mutex_);
            accepting_ = false;
            dropped = std::exchange(shot_, nullptr);
        }
        // 没有回调接手这张 shot：标记失败并唤醒等帧的一方。
        // 与回调并发时双方都在锁内交接，因此 done 只会被 release 一次。
        if (dropped) {
            dropped->failed.store(true, std::memory_order_release);
            dropped->done.release();
        }
    }

    void FrameSink::on_frame(winrt::Direct3D11CaptureFramePool const& pool) {
        std::shared_ptr<ShotContext> shot;
        {
            std::scoped_lock lock(mutex_);
            if (!accepting_)
                return;
            ++in_flight_;
            shot = std::exchange(shot_, nullptr);
        }

        if (shot) {
            try {
                handle_frame(pool, *shot);
            } catch (std::exception const& e) {
                shot->failed.store(true, std::memory_order_release);
                SPDLOG_ERROR("处理帧失败: {}", e.what());
            } catch (...) {
                shot->failed.store(true, std::memory_order_release);
                SPDLOG_ERROR("处理帧失败");
            }
            shot->done.release();
        }

        // 只在最后一个在途回调退出时唤醒：关闭流程等待期间 WGC 仍会按刷新率投递空回调，
        // 每次都 notify 会让 capture 线程反复醒来检查谓词。
        bool idle = false;
        {
            std::scoped_lock lock(mutex_);
            idle = --in_flight_ == 0;
        }
        if (idle)
            idle_cv_.notify_all();
    }

    bool FrameSink::wait_idle(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return idle_cv_.wait_for(lock, timeout, [this] { return in_flight_ == 0; });
    }

    // ── CaptureWorker ────────────────────────────────────────────────────────

    CaptureWorker::CaptureWorker(Config config,
                                 std::shared_ptr<CommandQueue> commands,
                                 std::shared_ptr<EncodeQueue> jobs,
                                 std::shared_ptr<StopSource> stop,
                                 std::shared_ptr<UiBridge> ui)
        : config_(std::move(config)),
          commands_(std::move(commands)),
          jobs_(std::move(jobs)),
          stop_(std::move(stop)),
          ui_(std::move(ui)),
          command_token_(*commands_) {}

    void CaptureWorker::run() {
        // 启动阶段的资源初始化：无论成功还是失败都必须放行主线程的 wait_ready()。
        // guard 只管失败/异常提前返回的那条路径；成功路径要显式 count_down 再 dismiss，
        // 否则 dismiss() 会把 count_down 一起取消，主线程永远卡在 ready_.wait() 上。
        auto ready_guard = ScopeExit{[&] { ready_.count_down(); }};


        // COM apartment 必须覆盖整个 run()：之后还会创建 pool/会话并关闭 WinRT 对象。
        ComApartment apartment;
        if (!apartment.ok())
            SPDLOG_WARN("capture 线程初始化 COM 失败: {}", describe_hresult(apartment.result()));

        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        ::SetThreadDescription(::GetCurrentThread(), L"GameSnap.Capture");

        // 启动时就把资源准备好，第一次热键不需要重建。
        // 如果这里就失败（典型是 E_ACCESSDENIED），继续跑也没有任何意义，直接通知主线程退出。
        // 例外：一个显示器都没有时不是致命错误——保留托盘和消息循环，接入后自动恢复。
        if (!ensure_ready(ReloadReason::Startup)) {
            if (no_display()) {
                SPDLOG_WARN("当前没有检测到显示器，等待接入后自动恢复");
            } else {
                SPDLOG_CRITICAL("捕获资源初始化失败，GameSnap 无法截图，退出");
                fatal_error_.store(true, std::memory_order_relaxed);
                stop_->request_stop();
                return;
            }
        }
        ready_.count_down();  // 成功：先放行主线程，再取消 guard 避免重复 count_down
        ready_guard.dismiss();

        for (;;) {
            Command command;
            commands_->wait_dequeue(command_token_, command);
            if (command.kind == CommandKind::Shutdown)
                break;
            // 已收到停止信号：只消费队列，不再启动新的截图/重建。
            if (stop_->stop_requested())
                continue;
            handle_command(command);
        }

        if (resources_) {
            resources_->close();
            resources_.reset();
        }
        d3d_.reset();
        SPDLOG_INFO("capture 线程退出");
    }

    void CaptureWorker::handle_command(Command const& command) {
        switch (command.kind) {
        case CommandKind::Trigger:
            shot(command);
            break;
        case CommandKind::Validate:
            ensure_ready(command.reason);
            break;
        case CommandKind::Reload:
            rebuild(command.reason);
            break;
        case CommandKind::Shutdown:
            break;
        }
    }

    bool CaptureWorker::ensure_ready(ReloadReason reason) {
        if (!resources_ || !d3d_)
            return rebuild(reason);

        if (d3d_->device_removed()) {
            SPDLOG_WARN("D3D 设备已被移除（TDR / 驱动重启 / 显卡拔出）");
            return rebuild(ReloadReason::DeviceRemoved);
        }
        if (!monitor_alive(monitor_)) {
            SPDLOG_WARN("目标显示器已失效（拔掉或拓扑变化）");
            return rebuild(ReloadReason::MonitorLost);
        }
        if (!d3d_->factory_current()) {
            SPDLOG_WARN("DXGI 工厂已失效（显卡或显示拓扑发生变化）");
            return rebuild(ReloadReason::DeviceChange);
        }

        // WM_DEVICECHANGE 可能是任意 USB 设备，没必要每次枚举 DXGI 输出；
        // 设备/显示器是否失效已由上面 device_removed / monitor_alive / factory 检查覆盖。
        if (reason == ReloadReason::DeviceChange)
            return true;

        // 目标显示器可能已经变化（主屏切换 / 前台窗口换屏），每次自检重新选择。
        const DisplayInfo current = select_monitor(config_.monitor);
        if (current.monitor == nullptr) {
            SPDLOG_WARN("无法读取目标显示器信息");
            return rebuild(ReloadReason::MonitorLost);
        }
        if (current.monitor != monitor_) {
            SPDLOG_INFO("目标显示器变化: {} -> {}",
                        wstring_to_utf8(display_.device_name), wstring_to_utf8(current.device_name));
            return rebuild(ReloadReason::DisplayChange, &current);
        }
        if (current.width != display_.width || current.height != display_.height ||
            current.rotation != display_.rotation || current.hdr != display_.hdr ||
            current.color_space != display_.color_space ||
            current.sdr_white_nits != display_.sdr_white_nits ||
            current.device_name != display_.device_name) {
            SPDLOG_INFO("显示配置变化: {}x{} -> {}x{}，HDR {} -> {}，色域 {} -> {}",
                        display_.width, display_.height, current.width, current.height,
                        display_.hdr ? "是" : "否", current.hdr ? "是" : "否",
                        to_string(display_.color_space), to_string(current.color_space));
            return rebuild(ReloadReason::DisplayChange, &current);
        }
        return true;
    }

    bool CaptureWorker::rebuild(ReloadReason reason, DisplayInfo const* known_display) {
        SPDLOG_INFO("重建捕获资源: {}", to_string(reason));

        // 1) ensure_ready 已经选过显示器时直接复用，避免重复枚举。
        auto display = known_display != nullptr ? *known_display : select_monitor(config_.monitor);
        if (display.monitor == nullptr) {
            // 没有显示器（启动时就没接，或者全部被拔掉）。标记成可恢复状态：
            // 保留托盘/线程，等 WM_DISPLAYCHANGE 触发下一次 Validate 再重建。
            no_display_.store(true, std::memory_order_relaxed);
            SPDLOG_WARN("没有可用的显示器，等待接入");
            return false;
        }
        no_display_.store(false, std::memory_order_relaxed);
        if (display.monitor != monitor_) {
            if (monitor_ != nullptr) {
                SPDLOG_INFO("目标显示器发生变化: {} -> {}",
                            wstring_to_utf8(display_.device_name), wstring_to_utf8(display.device_name));
            }
            log_display_info(display);
        }

        // 2) 先建好新一代；任何一步失败都保留旧代继续服役。
        // 这几条日志是卡死时唯一能定位“卡在哪一步”的线索：D3D 创建和 WGC 的
        // CreateForMonitor 都可能因为驱动/HDR 全屏而长时间不返回。
        SPDLOG_INFO("重建捕获资源: 创建 D3D 设备…");
        std::string error;
        auto d3d = D3DContext::create(display.monitor, &error);
        if (!d3d) {
            SPDLOG_ERROR("创建 D3D 设备失败: {}", error);
            return false;
        }

        const bool hdr = display.hdr;

        const auto format = hdr ? winrt::DirectXPixelFormat::R16G16B16A16Float
                                : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;

        SPDLOG_INFO("重建捕获资源: 创建 GraphicsCaptureItem（CreateForMonitor）…");
        auto resources = CaptureResources::create(display.monitor, d3d, format, error);
        if (!resources) {
            SPDLOG_ERROR("{}", error);
            return false;
        }
        const auto size = resources->item_size();

        // 3) 全部成功后再替换，旧的一代最后关闭。
        if (resources_) {
            resources_->close();
            resources_.reset();
        }
        d3d_ = std::move(d3d);
        resources_ = std::move(resources);
        display_ = display;
        monitor_ = display.monitor;
        ++generation_;

        SPDLOG_INFO("捕获资源就绪: 代 #{}，{}x{}，{}",
                    generation_, size.Width, size.Height,
                    hdr ? "FP16 捕获(HDR→SDR)" : "BGRA8");
        return true;
    }

    void CaptureWorker::shot(Command const& command) {
        const auto dequeued = SteadyClock::now();
        // 热路径上不做完整自检：资源缺失就建，StartCapture 失败再走重建。
        if (!resources_ || !d3d_) {
            if (!rebuild(ReloadReason::Startup))
                return;
        }

        if (d3d_->device_removed()) {
            SPDLOG_WARN("截图前检测到 D3D 设备已移除，重建资源");
            if (!rebuild(ReloadReason::DeviceRemoved))
                return;
        }
        const bool hdr = display_.hdr;

        auto shot = std::make_shared<ShotContext>();
        shot->d3d = d3d_;
        shot->jobs = jobs_;
        shot->ui = ui_;
        shot->canceled = std::make_shared<std::atomic<bool>>(false);
        shot->timings.input = command.enqueued;
        shot->sequence = ++sequence_;
        shot->timings.dequeued = dequeued;
        shot->title = foreground_window_title();
        shot->timings.title_done = SteadyClock::now();
        // WGC 抓的是 DWM 的合成表面，FP16 帧的编码是合成色彩空间（CCCS）：scRGB，
        // 即 BT.709 原色 + 线性伽马，1.0 = 80 nits，HDR 高光会超过 1.0。
        // 它不是显示器的线格式：显示器的 ColorSpace（G2084/P2020 = PQ）只说明屏幕怎么输出，
        // 只用于判断是否 HDR 和打日志。把帧当 PQ 处理会把白点写成 10000 nits、中间调压暗、
        // 并用 saturate() 丢掉 1.0 以上的高光。
        shot->color_space = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        shot->sdr_white_nits = display_.sdr_white_nits;
        shot->hdr = hdr;
        shot->hdr_format = config_.hdr_format;
        shot->sdr_format = config_.sdr_format;
        shot->tone_map = config_.tone_map;

        // pool/session 已由上一张结束时预创建；这里只记录时间。
        shot->timings.prepared = SteadyClock::now();

        // 每个 pool 有自己的 sink；先记录时间再 arm，避免回调线程读到未发布的 start_capture。
        auto sink = resources_->sink();
        shot->timings.start_capture = SteadyClock::now();
        sink->arm(shot);
        try {
            resources_->start_capture();
            SPDLOG_DEBUG("[#{:04}] StartCapture 调用 {:.2f}ms", shot->sequence,
                         duration_ms(SteadyClock::now() - shot->timings.start_capture));
        } catch (winrt::hresult_error const& e) {
            SPDLOG_ERROR("StartCapture 失败: {}", describe_hresult(e.code()));
            resources_->close();
            resources_.reset();
            rebuild(ReloadReason::CaptureFailure);
            return;
        }

        // 等回调把这张图处理完；静止画面下 WGC 可能不给帧，必须有上限。
        if (!shot->done.try_acquire_for(kFrameWait)) {
            shot->canceled->store(true, std::memory_order_release);
            SPDLOG_WARN("[#{:04}] {}ms 内没有等到帧，取消本次截图", shot->sequence, kFrameWait.count());
        }
        // 收尾顺序和 close() 共用 quiesce()：disarm → 注销 → 等在途回调退出。
        // 回调 2 秒不退出说明驱动卡住，此时不能 Close，直接放弃这一代。
        if (!resources_->quiesce(kCallbackWait)) {
            SPDLOG_ERROR("[#{:04}] FrameArrived 回调未退出，放弃当前资源代", shot->sequence);
            resources_->abandon();
            resources_.reset();
            return;
        }
        if (shot->failed.load(std::memory_order_acquire)) {
            SPDLOG_WARN("[#{:04}] 帧拷贝失败（设备移除/TDR），重建资源", shot->sequence);
            resources_->close();
            resources_.reset();
            rebuild(ReloadReason::DeviceRemoved);
            return;
        }
        resources_->stop_capture();
        // pool/session 已在上一张结束后重建好，下一次热键只做 arm + StartCapture。
        try {
            resources_->prepare();
        } catch (winrt::hresult_error const& e) {
            SPDLOG_WARN("预创建下一张 capture pool/session 失败: {}", describe_hresult(e.code()));
            resources_->close();
            resources_.reset();
        }
    }

}  // namespace gamesnap
