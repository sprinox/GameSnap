#include "pch.h"

#include "capture.h"

#include <windows.graphics.capture.interop.h>

#include <cstring>

namespace gamesnap {
    namespace {

        constexpr int kFramePoolBuffers = 2;
        constexpr auto kFrameWait = std::chrono::milliseconds(500);

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

        D3D11_TEXTURE2D_DESC make_staging_desc(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            return desc;
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

        bool staging_matches(ID3D11Texture2D& texture, D3D11_TEXTURE2D_DESC const& desc) {
            D3D11_TEXTURE2D_DESC actual{};
            texture.GetDesc(&actual);
            return actual.Width == desc.Width && actual.Height == desc.Height && actual.Format == desc.Format;
        }

        // FrameArrived 回调里只做 GPU 侧的事：取帧 → CopyResource 到 staging → 入队。
        // Map/Unmap（GPU→CPU）交给编码线程，回调线程不被整帧 memcpy 拖住。
        void handle_frame(winrt::Direct3D11CaptureFramePool const& pool, ShotContext& shot) {
            winrt::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
            if (!frame)
                return;
            const auto received = Clock::now();

            winrt::com_ptr<ID3D11Texture2D> source;
            try {
                auto access = frame.Surface().as<Windows::IDirect3DDxgiInterfaceAccess>();
                if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()))) {
                    frame.Close();
                    SPDLOG_ERROR("获取帧纹理失败");
                    return;
                }
            } catch (winrt::hresult_error const& e) {
                frame.Close();
                SPDLOG_ERROR("获取帧纹理失败: {}", describe_hresult(e.code()));
                return;
            }

            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);

            const auto staging_desc = make_staging_desc_from(desc);
            std::shared_ptr<ID3D11Texture2D> texture;
            // 主纹理空闲（capture 线程的 staging_ + 本次 shot 的副本，共 2 个引用）且尺寸/格式
            // 匹配时复用；编码线程还占着（3 个引用）或分辨率变了，就临时建一块，随 job 析构释放。
            // use_count() 在这里只当“有没有别的持有者”的探针用，不会误判：
            //   - 只有本回调线程会新增引用（交给编码 job），其它线程只会释放（编码完成）；
            //   - 本次 shot 的引用在 arm() 的 mutex 之前就已建立，之前 job 的引用也都
            //     sequenced-before 本线程，所以读到的计数只会偏高、不会偏低；
            //   - 读到偏高（job 刚释放）时最坏只是多建一块临时纹理，不影响正确性。
            if (shot.staging && shot.staging.use_count() <= 2 && staging_matches(*shot.staging, staging_desc)) {
                SPDLOG_DEBUG("复用旧纹理");
                texture = shot.staging;
            } else {
                SPDLOG_DEBUG("创建新纹理");
                texture = create_staging_texture(shot.d3d->device(), staging_desc);
            }
            if (!texture) {
                frame.Close();
                SPDLOG_ERROR("创建 staging 纹理失败 ({}x{}, fmt={})",
                             desc.Width, desc.Height, static_cast<int>(desc.Format));
                return;
            }

            {
                std::scoped_lock lock(shot.d3d->context_mutex());
                shot.d3d->context()->CopyResource(texture.get(), source.get());
            }
            frame.Close();
            const auto copied = Clock::now();

            SPDLOG_DEBUG("[#{:04}] 采集 {}x{}: 命令排队 {:.2f}ms，StartCapture→首帧 {:.2f}ms，GPU 拷贝 {:.2f}ms",
                         shot.sequence, desc.Width, desc.Height,
                         duration_ms(shot.start_call - shot.enqueued),
                         duration_ms(received - shot.start_call),
                         duration_ms(copied - received));

            EncodeJob job;
            job.texture = std::move(texture);
            job.d3d = shot.d3d;
            job.color_space = shot.color_space;
            job.sdr_white_nits = shot.sdr_white_nits;
            job.sequence = shot.sequence;
            job.title = std::move(shot.title);
            job.local_time = Clock::now();
            job.timings.input = shot.enqueued;
            job.timings.start_capture = shot.start_call;
            job.timings.frame_received = received;
            job.timings.copied = copied;

            if (!shot.jobs->enqueue(std::move(job)))
                SPDLOG_WARN("编码队列入队失败，丢弃本次截图");
        }

        // 初始化结束时放开就绪闩锁（成功/失败/异常都走析构）。
        struct ReadyGuard {
            explicit ReadyGuard(std::latch& latch_) : latch(latch_) {}
            ~ReadyGuard() { latch.count_down(); }
            ReadyGuard(ReadyGuard const&) = delete;
            ReadyGuard& operator=(ReadyGuard const&) = delete;
            ReadyGuard(ReadyGuard&&) = delete;
            ReadyGuard& operator=(ReadyGuard&&) = delete;

            std::latch& latch;
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
            resources->sink_ = std::make_shared<FrameSink>();

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

    void CaptureResources::start_capture() {
        winrt::GraphicsCaptureSession session{nullptr};
        {
            std::scoped_lock lock(mutex_);
            if (closed_ || !item_ || !device_)
                throw winrt::hresult_error(RO_E_CLOSED, L"capture 资源已关闭");
            if (running_)
                return;
            if (!pool_)
                create_pool();
            if (!session_)
                create_session();
            session = session_;
            running_ = true;
        }

        try {
            session.StartCapture();
        } catch (...) {
            std::scoped_lock lock(mutex_);
            running_ = false;
            throw;
        }
    }

    void CaptureResources::stop_capture() noexcept {
        winrt::GraphicsCaptureSession session{nullptr};
        winrt::Direct3D11CaptureFramePool pool{nullptr};
        winrt::event_token token{};
        {
            std::scoped_lock lock(mutex_);
            running_ = false;
            session = std::exchange(session_, nullptr);
            pool = std::exchange(pool_, nullptr);
            token = std::exchange(frame_token_, {});
        }

        // 帧在 done 之前已经处理完，pool 也不复用，所以这里直接关闭、不用等回调退栈：
        // 注销 FrameArrived → 关会话 → 关 pool（下一张在 start_capture() 里建全新的）。
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
        winrt::Direct3D11CaptureFramePool pool{nullptr};
        winrt::GraphicsCaptureSession session{nullptr};
        winrt::GraphicsCaptureItem item{nullptr};
        winrt::event_token token{};
        {
            std::scoped_lock lock(mutex_);
            if (closed_)
                return;
            closed_ = true;
            running_ = false;
            pool = std::exchange(pool_, nullptr);
            session = std::exchange(session_, nullptr);
            item = std::exchange(item_, nullptr);
            token = std::exchange(frame_token_, {});
        }

        // COM 调用全部放在锁外。
        if (pool && token) {
            try {
                pool.FrameArrived(token);
            } catch (winrt::hresult_error const& e) {
                SPDLOG_DEBUG("注销 FrameArrived 失败: {}", describe_hresult(e.code()));
            }
        }
        if (session) {
            try {
                session.Close();
            } catch (winrt::hresult_error const& e) {
                SPDLOG_DEBUG("关闭 session 失败: {}", describe_hresult(e.code()));
            }
        }
        if (pool) {
            try {
                pool.Close();
            } catch (winrt::hresult_error const& e) {
                SPDLOG_DEBUG("关闭 frame pool 失败: {}", describe_hresult(e.code()));
            }
        }

        if (sink_)
            sink_->disarm();
    }

    // ── FrameSink ────────────────────────────────────────────────────────────

    void FrameSink::arm(std::shared_ptr<ShotContext> shot) {
        std::scoped_lock lock(mutex_);
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
            shot = std::exchange(shot_, nullptr);
        }
        if (!shot)
            return;

        try {
            handle_frame(pool, *shot);
        } catch (std::exception const& e) {
            SPDLOG_ERROR("处理帧失败: {}", e.what());
        } catch (...) {
            SPDLOG_ERROR("处理帧失败");
        }
        shot->done.release();
    }

    // ── CaptureWorker ────────────────────────────────────────────────────────

    CaptureWorker::CaptureWorker(Config config,
                                 std::shared_ptr<CommandQueue> commands,
                                 std::shared_ptr<EncodeQueue> jobs,
                                 std::shared_ptr<StopSource> stop)
        : config_(std::move(config)),
          commands_(std::move(commands)),
          jobs_(std::move(jobs)),
          stop_(std::move(stop)),
          command_token_(*commands_) {}

    void CaptureWorker::run() {
        {
            // 就绪闩锁只覆盖启动初始化：初始化成功/失败/异常后主线程都应继续。
            // 不能放在 run() 最外层，否则成功路径要等到线程退出才 release，
            // 主线程会卡在 wait_ready()，连 Shutdown 都发不出去。
            const ReadyGuard ready_guard{ready_};

            ComApartment apartment;
            if (!apartment.ok())
                SPDLOG_WARN("capture 线程初始化 COM 失败: {}", describe_hresult(apartment.result()));

            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            ::SetThreadDescription(::GetCurrentThread(), L"GameSnap.Capture");

            // 启动时就把资源准备好，第一次热键不需要重建。
            // 如果这里就失败（典型是 E_ACCESSDENIED），继续跑也没有任何意义，直接通知主线程退出。
            if (!ensure_ready()) {
                SPDLOG_CRITICAL("捕获资源初始化失败，GameSnap 无法截图，退出");
                fatal_error_.store(true, std::memory_order_relaxed);
                stop_->request_stop();
                return;
            }
        }

        for (;;) {
            Command command;
            commands_->wait_dequeue(command_token_, command);
            if (command.kind == CommandKind::Shutdown)
                break;
            handle_command(command);
        }

        if (resources_) {
            resources_->close();
            resources_.reset();
        }
        staging_.reset();
        d3d_.reset();
        SPDLOG_INFO("capture 线程退出");
    }

    void CaptureWorker::handle_command(Command const& command) {
        switch (command.kind) {
        case CommandKind::Trigger:
            shot(command);
            break;
        case CommandKind::Validate:
            ensure_ready();
            break;
        case CommandKind::Reload:
            rebuild(command.reason);
            break;
        case CommandKind::Shutdown:
            break;
        }
    }

    bool CaptureWorker::ensure_ready() {
        if (!resources_ || !d3d_ || !staging_)
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

        // 分辨率 / 旋转 / HDR / 显示器变化才重建；单纯切全屏不应触发。
        const DisplayInfo current = describe_monitor(monitor_);
        if (current.monitor == nullptr) {
            SPDLOG_WARN("无法读取目标显示器信息");
            return rebuild(ReloadReason::MonitorLost);
        }
        if (current.width != display_.width || current.height != display_.height ||
            current.rotation != display_.rotation || current.hdr != display_.hdr ||
            current.color_space != display_.color_space || current.device_name != display_.device_name) {
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

        const bool hdr = config_.hdr_capture && display.hdr;
        if (config_.hdr_capture && !display.hdr)
            SPDLOG_WARN("--hdr 已开启，但目标显示器不是 HDR：按 SDR 捕获");

        const auto format = hdr ? winrt::DirectXPixelFormat::R16G16B16A16Float
                                : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;

        auto resources = CaptureResources::create(display.monitor, d3d, format, error);
        if (!resources) {
            SPDLOG_ERROR("{}", error);
            return false;
        }

        const auto size = resources->item_size();
        const auto desc = make_staging_desc(
            static_cast<std::uint32_t>(size.Width), static_cast<std::uint32_t>(size.Height), dxgi_format_of(format));

        auto staging = create_staging_texture(d3d->device(), desc);
        if (!staging) {
            SPDLOG_ERROR("创建 staging 纹理失败");
            resources->close();
            return false;
        }

        // 3) 全部成功后再替换，旧的一代最后关闭。
        if (resources_) {
            resources_->close();
            resources_.reset();
        }
        d3d_ = std::move(d3d);
        resources_ = std::move(resources);
        staging_ = std::move(staging);
        display_ = display;
        monitor_ = display.monitor;
        ++generation_;

        SPDLOG_INFO("捕获资源就绪: 代 #{}，{}x{}，{}",
                    generation_, desc.Width, desc.Height,
                    hdr ? "FP16 捕获(HDR→SDR)" : "BGRA8");
        return true;
    }

    void CaptureWorker::shot(Command const& command) {
        // 热路径上不做完整自检：资源缺失就建，StartCapture 失败再走重建。
        if (!resources_ || !d3d_ || !staging_) {
            if (!rebuild(ReloadReason::Startup))
                return;
        }

        auto shot = std::make_shared<ShotContext>();
        shot->d3d = d3d_;
        shot->jobs = jobs_;
        shot->staging = staging_;
        shot->enqueued = command.enqueued;
        shot->start_call = Clock::now();
        shot->sequence = ++sequence_;
        shot->title = foreground_window_title();
        shot->color_space = display_.color_space;
        shot->sdr_white_nits = display_.sdr_white_nits;

        resources_->sink()->arm(shot);
        try {
            resources_->start_capture();
        } catch (winrt::hresult_error const& e) {
            SPDLOG_ERROR("StartCapture 失败: {}", describe_hresult(e.code()));
            resources_->sink()->disarm();
            rebuild(ReloadReason::CaptureFailure);
            return;
        }

        // 等回调把这张图处理完；静止画面下 WGC 可能不给帧，必须有上限。
        if (!shot->done.try_acquire_for(kFrameWait))
            SPDLOG_WARN("[#{:04}] {}ms 内没有等到帧，丢弃本次截图", shot->sequence, kFrameWait.count());

        resources_->sink()->disarm();
        resources_->stop_capture();
    }

}  // namespace gamesnap
