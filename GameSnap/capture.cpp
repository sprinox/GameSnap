#include "pch.h"

#include "capture.h"
#include "ui_bridge.h"

#include <windows.graphics.capture.interop.h>

#include <cstring>

namespace gamesnap {
    namespace {

        constexpr int kFramePoolBuffers = 2;
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

        std::shared_ptr<ID3D11Texture2D> create_gpu_texture(ID3D11Device* device,
                                                            D3D11_TEXTURE2D_DESC const& desc) {
            winrt::com_ptr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
                return {};
            return std::shared_ptr<ID3D11Texture2D>{texture.detach(), [](ID3D11Texture2D* p) {
                if (p != nullptr)
                    p->Release();
            }};
        }

        // 前台窗口标题，用作文件名前缀；拿不到就由编码侧回退到 GameSnap。
        std::wstring foreground_window_title() {
            const HWND window = ::GetForegroundWindow();
            if (window == nullptr)
                return {};
            const int length = ::GetWindowTextLengthW(window);
            if (length <= 0)
                return {};

            std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
            const int copied = ::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
            if (copied <= 0)
                return {};
            title.resize(static_cast<std::size_t>(copied));
            return title;
        }

        bool texture_matches(ID3D11Texture2D& texture, D3D11_TEXTURE2D_DESC const& desc) {
            D3D11_TEXTURE2D_DESC actual{};
            texture.GetDesc(&actual);
            return actual.Width == desc.Width && actual.Height == desc.Height &&
                   actual.Format == desc.Format && actual.Usage == desc.Usage &&
                   actual.BindFlags == desc.BindFlags;
        }

        // 纹理槽的 RAII：成功交给 job 前，任何提前返回/异常都会释放占用。
        struct BusyLease {
            explicit BusyLease(std::shared_ptr<TextureSlot> slot_) : slot(std::move(slot_)) {}
            std::shared_ptr<TextureSlot> slot;
            ~BusyLease() {
                if (slot)
                    slot->busy.store(false, std::memory_order_release);
            }
            BusyLease(BusyLease const&) = delete;
            BusyLease& operator=(BusyLease const&) = delete;
            BusyLease(BusyLease&&) = delete;
            BusyLease& operator=(BusyLease&&) = delete;
        };

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
            std::shared_ptr<TextureSlot> slot;
            if (shot.cache.slot && shot.cache.slot->texture && shot.cache.slot->hdr == shot.hdr &&
                texture_matches(*shot.cache.slot->texture, wanted) &&
                !shot.cache.slot->busy.exchange(true, std::memory_order_acq_rel)) {
                SPDLOG_DEBUG("复用 {} 纹理", shot.hdr ? "HDR" : "SDR");
                slot = shot.cache.slot;
            } else {
                SPDLOG_DEBUG("创建新 {} 纹理", shot.hdr ? "HDR" : "SDR");
                slot = std::make_shared<TextureSlot>();
                slot->hdr = shot.hdr;
                slot->texture = shot.hdr ? create_gpu_texture(shot.d3d->device(), wanted)
                                         : create_staging_texture(shot.d3d->device(), wanted);
                slot->busy.store(true, std::memory_order_release);
            }
            if (!slot || !slot->texture) {
                shot.failed.store(true, std::memory_order_release);
                frame.Close();
                SPDLOG_ERROR("创建 {} 纹理失败 ({}x{}, fmt={})",
                             shot.hdr ? "HDR" : "staging", desc.Width, desc.Height,
                             static_cast<int>(desc.Format));
                return;
            }
            BusyLease lease{slot};

            {
                std::scoped_lock lock(shot.d3d->context_mutex());
                shot.d3d->context()->CopyResource(slot->texture.get(), source.get());
            }
            frame.Close();
            // CopyResource 没有 HRESULT；TDR/拔卡时可能静默失败。GetDeviceRemovedReason 是
            // 唯一廉价的事后反馈，避免把旧内容或垃圾帧交给编码线程。
            if (FAILED(shot.d3d->device()->GetDeviceRemovedReason())) {
                shot.failed.store(true, std::memory_order_release);
                return;
            }
            const auto copied = SteadyClock::now();
            if (shot.canceled->load(std::memory_order_acquire))
                return;

            SPDLOG_DEBUG("[#{:04}] 采集 {}x{}: 命令排队 {:.2f}ms，StartCapture→首帧 {:.2f}ms，GPU 拷贝 {:.2f}ms",
                         shot.sequence, desc.Width, desc.Height,
                         duration_ms(shot.start_call - shot.enqueued),
                         duration_ms(received - shot.start_call),
                         duration_ms(copied - received));

            EncodeJob job;
            job.texture_slot = slot;
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
            job.timings.input = shot.enqueued;
            job.timings.dequeued = shot.dequeued;
            job.timings.title_done = shot.title_done;
            job.timings.prepared = shot.prepared;
            job.timings.start_capture = shot.start_call;
            job.timings.start_capture_done = shot.start_capture_done;
            job.timings.frame_received = received;
            job.timings.copied = copied;

            // 先让 job 持有占用标记；只有真正入队成功后才把 lease 的所有权交出去。
            if (shot.canceled->load(std::memory_order_acquire))
                return;  // lease 析构时释放 busy
            if (shot.ui)
                shot.ui->job_enqueued();  // 先计数，避免极快 job 先 finished 再 enqueued
            if (!shot.jobs->enqueue(std::move(job))) {
                if (shot.ui)
                    shot.ui->job_finished();
                SPDLOG_WARN("编码队列入队失败，丢弃本次截图");
                return;  // lease 析构时释放 busy
            }
            lease.slot.reset();
        }

        // 初始化结束时放开就绪闩锁（成功/失败/异常都走析构）。
        struct ReadyGuard {
            explicit ReadyGuard(std::latch& latch_) : latch(latch_) {}
            ~ReadyGuard() { release(); }
            void release() {
                if (!released) {
                    released = true;
                    latch.count_down();
                }
            }
            ReadyGuard(ReadyGuard const&) = delete;
            ReadyGuard& operator=(ReadyGuard const&) = delete;
            ReadyGuard(ReadyGuard&&) = delete;
            ReadyGuard& operator=(ReadyGuard&&) = delete;

            std::latch& latch;
            bool released = false;
        };

    }  // namespace

    std::string_view to_string(ReloadReason reason) {
        switch (reason) {
        case ReloadReason::Startup:
            return "首次创建";
        case ReloadReason::Manual:
            return "手动";
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
        if (running_)
            return;
        auto session = session_;
        running_ = true;
        try {
            session.StartCapture();
        } catch (...) {
            running_ = false;
            throw;
        }
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
        running_ = false;
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
        closed_ = true;
        stop_capture();
        item_ = nullptr;
        if (sink_) {
            sink_->disarm();
            sink_.reset();
        }
    }

    void CaptureResources::abandon() noexcept {
        closed_ = true;
        running_ = false;
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
        std::scoped_lock lock(mutex_);
        shot_.reset();
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

        {
            std::scoped_lock lock(mutex_);
            --in_flight_;
        }
        idle_cv_.notify_all();
    }

    bool FrameSink::close_and_wait(std::chrono::milliseconds timeout) {
        {
            std::scoped_lock lock(mutex_);
            accepting_ = false;
            shot_.reset();
        }
        idle_cv_.notify_all();
        return wait_idle(timeout);
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
        // 第一行建立，覆盖所有早退/异常；初始化完成或失败后显式 release，
        // 不能等到线程退出才 count_down，否则 main 的 wait_ready() 永远等不到。
        ReadyGuard ready_guard{ready_};

        // COM apartment 必须覆盖整个 run()：之后还会创建 pool/会话并关闭 WinRT 对象。
        ComApartment apartment;
        if (!apartment.ok())
            SPDLOG_WARN("capture 线程初始化 COM 失败: {}", describe_hresult(apartment.result()));

        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        ::SetThreadDescription(::GetCurrentThread(), L"GameSnap.Capture");

        // 启动时就把资源准备好，第一次热键不需要重建。
        // 如果这里就失败（典型是 E_ACCESSDENIED），继续跑也没有任何意义，直接通知主线程退出。
        if (!ensure_ready(ReloadReason::Startup)) {
            SPDLOG_CRITICAL("捕获资源初始化失败，GameSnap 无法截图，退出");
            fatal_error_.store(true, std::memory_order_relaxed);
            stop_->request_stop();
            return;
        }
        ready_guard.release();

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
        texture_cache_ = {};
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
            return rebuild(ReloadReason::Startup);

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
            return rebuild(ReloadReason::DisplayChange);
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
            return rebuild(ReloadReason::DisplayChange);
        }
        return true;
    }

    bool CaptureWorker::rebuild(ReloadReason reason) {
        SPDLOG_INFO("重建捕获资源: {}", to_string(reason));

        // 1) 重新选择显示器（可能被拔掉，或主显示器换了）。
        auto display = select_monitor(config_.monitor);
        if (display.monitor == nullptr) {
            SPDLOG_ERROR("没有可用的显示器");
            return false;
        }
        if (display.monitor != monitor_) {
            if (monitor_ != nullptr) {
                SPDLOG_INFO("目标显示器发生变化: {} -> {}",
                            wstring_to_utf8(display_.device_name), wstring_to_utf8(display.device_name));
            }
            log_display_info(display);
        }

        // 2) 先建好新一代；任何一步失败都保留旧代继续服役。
        std::string error;
        auto d3d = D3DContext::create(display.monitor, &error);
        if (!d3d) {
            SPDLOG_ERROR("创建 D3D 设备失败: {}", error);
            return false;
        }

        const bool hdr = display.hdr;

        const auto format = hdr ? winrt::DirectXPixelFormat::R16G16B16A16Float
                                : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;

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
        texture_cache_ = {};
        {
            const auto wanted = hdr ? make_gpu_desc_from(D3D11_TEXTURE2D_DESC{
                                                 .Width = static_cast<UINT>(size.Width),
                                                 .Height = static_cast<UINT>(size.Height),
                                                 .MipLevels = 1,
                                                 .ArraySize = 1,
                                                 .Format = dxgi_format_of(format),
                                                 .SampleDesc = {.Count = 1}})
                                   : make_staging_desc_from(D3D11_TEXTURE2D_DESC{
                                                 .Width = static_cast<UINT>(size.Width),
                                                 .Height = static_cast<UINT>(size.Height),
                                                 .MipLevels = 1,
                                                 .ArraySize = 1,
                                                 .Format = dxgi_format_of(format),
                                                 .SampleDesc = {.Count = 1}});
            auto texture = hdr ? create_gpu_texture(d3d_->device(), wanted)
                               : create_staging_texture(d3d_->device(), wanted);
            if (texture) {
                auto slot = std::make_shared<TextureSlot>();
                slot->texture = std::move(texture);
                slot->hdr = hdr;
                texture_cache_.slot = std::move(slot);
            }
        }
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
        const bool hdr = display_.hdr;
        if (!resources_ || !d3d_) {
            if (!rebuild(ReloadReason::Startup))
                return;
        }

        if (d3d_->device_removed()) {
            SPDLOG_WARN("截图前检测到 D3D 设备已移除，重建资源");
            if (!rebuild(ReloadReason::DeviceRemoved))
                return;
        }

        auto shot = std::make_shared<ShotContext>();
        shot->d3d = d3d_;
        shot->jobs = jobs_;
        shot->ui = ui_;
        shot->cache = texture_cache_;
        shot->canceled = std::make_shared<std::atomic<bool>>(false);
        shot->enqueued = command.enqueued;
        shot->sequence = ++sequence_;
        shot->dequeued = dequeued;
        shot->title = foreground_window_title();
        shot->title_done = SteadyClock::now();
        shot->color_space = display_.color_space;
        shot->sdr_white_nits = display_.sdr_white_nits;
        shot->hdr = hdr;
        shot->hdr_format = config_.hdr_format;
        shot->sdr_format = config_.sdr_format;
        shot->tone_map = config_.tone_map;

        try {
            resources_->prepare();
            shot->prepared = SteadyClock::now();
        } catch (winrt::hresult_error const& e) {
            SPDLOG_ERROR("准备捕获会话失败: {}", describe_hresult(e.code()));
            // 会话已不可信；即使 rebuild 失败，也不要让下一张继续用坏资源。
            resources_->close();
            resources_.reset();
            rebuild(ReloadReason::CaptureFailure);
            return;
        }

        // 每个 pool 有自己的 sink；先记录时间再 arm，避免回调线程读到未发布的 start_call。
        auto sink = resources_->sink();
        shot->start_call = SteadyClock::now();
        sink->arm(shot);
        try {
            resources_->start_capture();
            shot->start_capture_done = SteadyClock::now();
        } catch (winrt::hresult_error const& e) {
            SPDLOG_ERROR("StartCapture 失败: {}", describe_hresult(e.code()));
            sink->disarm();
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
        // 先注销 token 阻止新回调，再由 sink 停止接收并等在途回调退出。
        resources_->unregister_frame_arrived();
        if (!sink->close_and_wait(kCallbackWait)) {
            SPDLOG_ERROR("[#{:04}] FrameArrived 回调未退出，放弃当前资源代", shot->sequence);
            resources_->abandon();
            resources_.reset();
            texture_cache_ = {};
            return;
        }
        if (shot->failed.load(std::memory_order_acquire)) {
            SPDLOG_WARN("[#{:04}] 帧拷贝失败（设备移除/TDR），重建资源", shot->sequence);
            resources_->close();
            resources_.reset();
            texture_cache_ = {};
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
