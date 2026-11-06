# GameSnap 并发、锁与资源生命周期

本文说明“谁在哪个线程读写什么、靠什么同步、资源何时分配/释放”。功能说明见 [architecture.md](architecture.md)。

对应代码：

- `GameSnap/main.cpp`
- `GameSnap/capture.h/.cpp`
- `GameSnap/encoder.h/.cpp`
- `GameSnap/input.h/.cpp`
- `GameSnap/system_events.h/.cpp`
- `GameSnap/messages.h`
- `GameSnap/d3d.h/.cpp`
- `GameSnap/tonemap.h/.cpp`
- `GameSnap/codecs.cpp`
- `GameSnap/common.h`

---

## 0. 核心不变量

1. **`CaptureResources`、`Direct3D11CaptureFramePool`、`GraphicsCaptureSession` 只在 capture 线程创建、使用、关闭。**
   这些对象没有自己的锁，靠单线程所有权保证安全。

2. **D3D11 immediate context 不是线程安全的。**
   所有 `CopyResource`、`Map`、`Unmap`、`CSSet*`、`Dispatch`、`Signal` 都通过
   `D3DContext::context_mutex()` 串行化。

3. **跨线程对象一律用 `shared_ptr` 保活。**
   `EncodeJob` 持有 `TextureSlot`、`D3DContext` 和 `UiBridge`；capture 和编码线程之间不传裸指针。

4. **队列无界，积压不丢。**
   `CommandQueue` / `EncodeQueue` 都是 `moodycamel::BlockingConcurrentQueue`。
   连按热键时在途帧会增加内存/显存占用，这是产品决策。

5. **不做纹理/大缓冲复用。**
   capture 纹理、编码 `raw` / `sdr` / `hdr10`、`ToneMapper` 纹理全部按 job 动态分配，
   job 结束即释放；只有 16KB 的 tone LUT 例外。

---

## 1. 线程清单

| 线程 | 入口 | 职责 |
|---|---|---|
| 主线程 | `wmain` | 初始化日志/COM、创建 worker、等待 stop、按顺序收尾、join |
| capture | `CaptureWorker::run()` | 唯一拥有 capture 资源；消费 `CommandQueue`；建 pool/session、`StartCapture`、收尾 |
| input | `InputThread::run()` | 键盘钩子 + GameInput；只投递 `Command::Trigger` |
| system-events | `SystemEventThread::run()` | 隐藏窗口 + WinEvent hook；投递 Validate/Reload |
| encode × N | `EncodeWorkers::worker_main()` | 消费 `EncodeJob`；Map、转换、tone map、编码、写盘 |
| overlay | `OverlayThread::run()` | DirectComposition + ImGui + MP3；先出声再建 UI，资源在触发后常驻 30s 再整体释放 |
| WGC 回调 | `FrameSink::on_frame()` | 取帧、创建本张纹理、`CopyResource`、入队、release `done` |
| GameInput 回调 | `InputThread::reading_callback()` | 读 Xbox Share 键，投递 Trigger |
| Ctrl+C | `console_ctrl_handler()` | 只对 `StopSource` 调 `request_stop()` |

COM apartment：main、capture、input、system-events、overlay 各自在线程入口创建；
编码线程不创建 WinRT 对象，只持有 D3D 的 COM 引用。

---

## 2. 资源所有权

### 2.1 CaptureResources

- capture 线程独占；`CaptureResources` 自身无锁。
- 每个 pool/session 对应一个独立的 `FrameSink`，旧 pool 的迟到回调不会取走新 shot。
- 重建时先建好新一代，全部成功后才替换 `resources_` / `d3d_`；旧代最后 `close()`。
- 收尾顺序只有 `quiesce()` 一份实现：`sink_->disarm()` → 注销 `FrameArrived` → `wait_idle()`；
  `close()` 和单张截图结束都走它，之后才 `Close()` 会话和 pool。
- `disarm()` 若丢下一张已 arm 但没被回调取走的 shot，会把它标记失败并 `release done`，
  避免调用方白等满等帧超时。
- `abandon()` 是回调卡死时的保命路径：不调用 WGC `Close()`，有意延迟回收。

### 2.2 每张截图的纹理

`FrameArrived` 里按当前 shot 新建：

- SDR：`D3D11_USAGE_STAGING` + `CPU_ACCESS_READ`，4B/像素；
- HDR：`DEFAULT` + `D3D11_BIND_SHADER_RESOURCE`，FP16，8B/像素。

纹理包装在 `TextureSlot` 里，由 `EncodeJob` 持有：

```cpp
struct TextureSlot {
    std::shared_ptr<ID3D11Texture2D> texture;
    bool hdr = false;
};
```

没有 `busy` 标记、没有 `texture_cache_`、没有 capture 侧主纹理。
`TextureSlot` 的最后一个 `shared_ptr` 在编码 job 结束时释放纹理。

### 2.3 编码 job 的缓冲

每次 `EncodeWorkers::encode()` 局部创建：

```cpp
RawCopy raw;          // CPU fallback / SDR Map
Image sdr;            // SDR 输出
U16Buffer hdr10;      // HDR 10-bit
```

三者都是 `UninitBuffer`（`resize()` 只分配、不置零），写满即整体覆盖，省掉 4K HDR 一张图近百 MiB 的 memset。
函数返回即释放，不使用 `thread_local EncodeScratch`，也没有 ScratchPool。
SDR 的 BGRA→BGRA 路径仍通过 `pixels.swap()` 避免一次整帧复制。

### 2.4 ToneMapper

每个 HDR job 临时创建自己的 `ToneMapper`：

- HDR output / readback（R10G10B10A2，4B/像素）；
- SDR output / readback（BGRA8，4B/像素）。

`map()` 完成并编码后，`ToneMapper` 随 job 析构，纹理全部释放。
**刻意不按线程复用**：复用等于把这 4 张全尺寸纹理（4K 下约 127 MiB）变成闲置常驻显存，
而截图工具绝大多数时间处于闲置状态；`ensure_size()` 的尺寸缓存因此不会被命中，
它只是给“同一个 mapper 服务多张图”留的实现细节，不构成复用路径。
因此没有常驻 mapper，也没有 `mapper_mutex_` / `mapper_owner_` / 熔断状态。

### 2.5 16KB tone LUT

`ToneLutCache` 是进程级静态对象，包含 `shared_ptr<const ToneLut>` 和一个
`std::atomic<bool> empty{true}`：

- 只在 HDR CPU 回退路径（FP16 逐像素转换）首次用到时构建；
- HDR job 缓存，SDR job 结束时释放（HDR 期间常驻，闲置时不留）；
- `empty` 为 true 时 `release_tone_lut()` 直接返回，不拿锁；只有表非空才进入 mutex；
- `empty` 与 mutex 保护的 `current` 允许短暂不一致，不保证 100% 避免重复加锁/释放，
  只优化绝大多数 SDR job。

LUT 的 `current` 由 `ToneLutCache::mutex` 保护；`empty` 是快速路径原子标记。

### 2.6 WGC pool/session 预创建

上一张截图结束后，capture 会为下一张预创建 `Direct3D11CaptureFramePool` +
`GraphicsCaptureSession`，目的是让热键路径只做 arm + `StartCapture`。

这不算截图纹理缓存；如果以后要连这部分也去掉，热键到首帧的延迟会增加。

---

## 3. 锁清单

| 锁 | 保护对象 | 持有时长 | 说明 |
|---|---|---|---|
| `D3DContext::context_mutex` | immediate context 的所有调用 | 微秒–毫秒 | 必要；Map 后的 CPU 行拷贝在锁外 |
| `ToneLutCache::mutex` | 16KB LUT 的 `current` | 建表/取指针/非空时释放 | HDR CPU 回退 + SDR 释放；空表由 atomic `empty` 快速跳过 |
| `UiBridge::mutex_` | 文件名、缩略图、`info_serial_` | 微秒 | Overlay 每帧只复制 `shared_ptr`/字符串 |
| `FrameSink::mutex_` + `idle_cv_` | `shot_` / `accepting_` / `in_flight_` | 微秒 | 等回调退出用 |
| `codec_options` | 编码参数 | 无锁 | 构造编码线程前写一次，之后只读 |

没有 `mapper_mutex_`、`ScratchPool::mutex`、`staging_busy`。

### 3.1 锁序

- 持 `context_mutex` 时不再拿 `ToneLutCache::mutex` / `FrameSink::mutex_` / `UiBridge::mutex_`；
- `ToneMapper` 的 fence 等待在 `context_mutex` 外；
- `UiBridge::publish()` 不进入 D3D 锁；
- `FrameSink::on_frame` 只在取/清 shot 时拿自己的 mutex，`CopyResource` 使用 `context_mutex`。

---

## 4. 一次截图的同步

1. capture 线程 `arm(shot)`，`StartCapture()`。
2. WGC 回调：
   - 从 pool 取 frame；
   - 新建 `TextureSlot` 和纹理；
   - 在 `context_mutex` 内 `CopyResource`，随后 `signal_fence_locked()`；
   - 填 `EncodeJob`，入队；
   - 入队后 `shot.done.release()`。
3. capture 线程等 `done`，再 `quiesce()`（disarm → 注销 → 等回调退出）→ `stop_capture()`；
   上一张结束后预创建下一套 pool/session。
4. 编码线程：
   - HDR 且允许 GPU：本 job 现建 `ToneMapper`，`map()` 内部等 fence，map 完随 job 析构；
   - 否则 `map_to_cpu()`：先等 `gpu_fence`，再在 `context_mutex` 内 `Map` / `Unmap`；
   - CPU 行拷贝、转换、编码、写盘全部在锁外。
5. `EncodeJob` 析构，纹理和缓冲释放。

`done` 只表示帧已经进入目标纹理，不表示已经写盘。

---

## 5. 退出与失败

- 退出顺序：input / system-events → capture `Shutdown` → encode `stop` 哨兵 → join。
  哨兵只在“队列里不再有新生产者”之后投递；由于 moodycamel 的 MPMC 队列不保证跨生产者 FIFO，
  最后一个消费到哨兵的编码线程会清空残留任务，避免 Shutdown 时丢掉已入队的截图。
- capture 初始化失败会设置 `fatal_error_` 并 `request_stop()`；主线程用 ready latch 退出等待。
- 编码异常在 `worker_main()` 里捕获，单张失败不会杀死线程。
- 回调 2 秒不退出：`quiesce()` 超时，资源走 `abandon()`，不调用 WGC `Close()`。
- `std::bad_alloc` 由全局 new_handler 记录并 abort；不在每个分配点做恢复。

---

## 6. 分配策略与取舍

### 6.1 动态分配

以下资源不再常驻：

- capture 的 SDR staging / HDR DEFAULT 纹理；
- 编码线程的 `raw` / `sdr` / `hdr10`；
- `ToneMapper` 的 4 张全尺寸纹理。

优点：

- 闲置时几乎不占全尺寸显存/内存；
- 没有纹理 `busy` 的跨线程竞争；
- 生命周期由 `shared_ptr` / job 作用域直接表达。

代价：

- 每张截图增加纹理创建、ToneMapper 初始化、缓冲分配；
- HDR 连拍时分配/释放更频繁；
- 驱动分配器可能产生碎片或额外延迟。

### 6.2 例外

- **16KB tone LUT**：HDR 缓存，SDR 释放；空表由 atomic `empty` 快速跳过，避免大多数 SDR job 拿锁。
- **WGC pool/session**：保留下一张预创建，以压低热键延迟。
- **libjpeg `thread_local cinfo` / 行缓冲**：`longjmp` 不执行普通局部对象析构，
  改成每张分配会泄漏或破坏状态；这是非纹理的小缓冲。

---

## 7. 审查清单

- [ ] `CaptureResources` 是否只在 capture 线程访问？
- [ ] `close()` 与单张收尾是否都走 `quiesce()`，且在 `Close()` 之前等到回调退出？
- [ ] 每张截图是否新建 `TextureSlot`，job 是否持有到编码结束？
- [ ] 编码线程是否不再使用 `thread_local` 大缓冲，大缓冲是否都是 `UninitBuffer`（不整帧清零）？
- [ ] `ToneMapper` 是否仍然每个 HDR job 现建现用、不跨 job 复用（闲置显存）？
- [ ] 16KB LUT 是否 HDR 缓存、SDR 释放，且空表时 atomic 快路径不拿锁？
- [ ] `context_mutex` 内是否没有 fence 等待、CPU 行拷贝、编码或写盘？
- [ ] `UiBridge` 是否只复制 `shared_ptr`/字符串，不复制缩略图像素？
- [ ] `FrameSink::disarm()` 是否丢弃已 arm 的 shot 并唤醒等待方？
- [ ] 队列积压时是否仍能通过 `stop` 哨兵正常退出？
