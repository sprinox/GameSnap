#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 队列里传递的消息类型，以及直接使用的 moodycamel 阻塞队列别名。
// 不额外封装队列：停止通过显式的 Shutdown 命令 / stop 哨兵实现，
// 消费端一律阻塞等待，不做超时轮询。
// ─────────────────────────────────────────────────────────────────────────────

#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>

#include "common.h"
#include "image.h"

namespace gamesnap {

    class D3DContext;

    enum class ReloadReason : std::uint8_t {
        Startup,  // 首次创建
        DisplayChange,   // 分辨率 / 显示器拓扑 / HDR 切换
        DeviceChange,    // 显卡插拔、驱动重启导致 DXGI 工厂失效
        DeviceRemoved,   // GetDeviceRemovedReason 失败（TDR 等）
        MonitorLost,     // 目标 HMONITOR 消失
        CaptureFailure,  // StartCapture / 拷贝失败
        PowerResume,     // 从睡眠恢复
    };

    std::string_view to_string(ReloadReason reason);

    enum class CommandKind : std::uint8_t {
        Trigger,   // 热键：立刻截图
        Validate,  // 廉价自检：只有确实失效时才重建
        Reload,    // 强制重建捕获资源
        Shutdown,
    };

    // 输入线程 / 系统事件线程 → 截图线程。截图线程是捕获资源的唯一所有者。
    struct Command {
        CommandKind kind = CommandKind::Validate;
        ReloadReason reason = ReloadReason::Startup;
        SteadyClock::time_point enqueued = SteadyClock::now();
    };

    // 纹理和占用标记绑定在同一个对象里，避免两个独立 shared_ptr 不配套。
    struct TextureSlot {
        std::shared_ptr<ID3D11Texture2D> texture;
        std::atomic<bool> busy{false};
        bool hdr = false;
    };

    // 截图线程 → 编码线程。GPU 拷贝（CopyResource）在回调里做；
    // Map/Unmap 这种 GPU→CPU 传输在编码线程里做，所以 job 持有 staging 纹理和设备。
    struct EncodeJob {
        bool stop = false;  // 编码线程的停止哨兵
        std::shared_ptr<TextureSlot> texture_slot;
        std::shared_ptr<D3DContext> d3d;             // 保活设备 + 提供 context 锁
        std::shared_ptr<std::atomic<bool>> canceled;  // 超时取消标记
        bool hdr = false;
        ImageFormat hdr_format = ImageFormat::Avif;
        ImageFormat sdr_format = ImageFormat::Webp;
        ToneMapAlgorithm tone_map = ToneMapAlgorithm::Spline;
        DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        std::uint32_t sdr_white_nits = 80;
        std::uint64_t sequence = 0;
        std::uint64_t gpu_fence = 0;
        std::wstring title;            // 截图时前台窗口的标题，用作文件名前缀
        Clock::time_point local_time;  // 墙上时间，只用于文件名
        ShotTimings timings{};
    };

    using CommandQueue = moodycamel::BlockingConcurrentQueue<Command>;
    using EncodeQueue = moodycamel::BlockingConcurrentQueue<EncodeJob>;

}  // namespace gamesnap
