# GameSnap 代码审查报告

审查范围：`GameSnap/` 全部源码（9 个 .cpp + 9 个 .h，约 2.4k 行有效代码）、`docs/`、`config.example.yaml`、`GameSnap.vcxproj`。

审查重点（按你的要求）：**并发安全性 → 不必要的锁 → 冗长/无意义的设计 → 写法与性能**。

结论先行：**整体设计是这个规模项目里少见的清醒**——资源"按代"整体替换、生产者先停消费者后停、`shared_ptr` 承担跨线程生命周期、队列不额外封装、编码任务持有设备保活，这几条都做对了，没有发现死锁路径。问题集中在三处：**几条"注释承诺了但代码没做到"的并发约定**、**用 `use_count()` 当所有权探针**、以及**若干只写不读的死代码与重复的三段式手工裁剪**。

---

## 1. 并发模型评估：锁清单逐个定性

先回答"有没有不必要的锁"。全项目一共 5 个真正的同步原语，逐个给出结论：

| 同步原语 | 保护对象 | 是否必要 | 评价 |
| --- | --- | --- | --- |
| `D3DContext::context_mutex_` | 同一 immediate context 上的 `CopyResource` / `Map`+`Unmap` | **逻辑必要，但 API 层面非必需** | 设备创建时未指定 `D3D11_CREATE_DEVICE_SINGLETHREADED`（`d3d.cpp:18` 只有 `BGRA_SUPPORT`），D3D11 运行时本身就会内部串行化，所以这个锁对"API 安全"是冗余的；它真正的价值是**显式排序**。见 §4.1 |
| `CaptureResources::mutex_` | `pool_` / `session_` / `item_` / `token_` 句柄 | **必要**（截图线程 vs frame pool 回调线程） | 但**有 3 处在锁内做 COM 调用**，违反了自己在 `capture.h:8` 写下的"COM 调用都在锁外"。见 §3.4 |
| `FrameSink::mutex_` | `shot_` | **必要**（一次性交接） | 用得不一致：`owner_` 在锁外读（§3.3），`set_owner()` 的锁则是纯装饰。若采用 §4.2 的简化，**这个锁和整个类成员可以删掉一半** |
| `std::latch ready_` / `std::atomic` 若干 | 就绪信号、线程 id、停止标志 | **必要** | 用法正确，`stop()` 在消息队列未就绪时补 `PostThreadMessage` 的处理很到位 |
| `std::binary_semaphore done` | 单张截图的完成信号 | **必要** | 但外面套了一层 `shared_ptr`，纯属多余分配（§5.3） |

**锁序是良性的（无死锁）**：`CaptureResources::mutex_` → `FrameSink::mutex_` 与 `context_mutex_` 从不嵌套持有；`FrameSink::on_frame` 在 `handle_frame` 前就已释放 `mutex_`（`capture.cpp:398-406`），这一步做得很对。**全项目没有发现 ABBA 死锁路径。**

### 1.1 `context_mutex_` 的真相："看起来并行，实际串行"

```cpp
// capture.cpp:137-140
std::scoped_lock lock(shot.d3d->context_mutex());
shot.d3d->context()->CopyResource(texture.get(), source.get());
```

```cpp
// encoder.cpp:169-181
std::scoped_lock lock(d3d.context_mutex());          // ← 锁
d3d.context()->Map(texture, 0, D3D11_MAP_READ, 0, &mapped);
for (y ...) std::memcpy(...);                         // ← 整帧 memcpy 在锁内
d3d.context()->Unmap(texture, 0);
```

`Map`/`Unmap` 必须包住整帧读取，所以这个临界区的**长度不可压缩**：4K BGRA 是 33 MB，锁内时间约 3–6 ms。也就是说 `encode_threads = N` 时，**GPU→CPU 读回这一段永远是串行的**，N 个线程只在"转换 + 写盘"上并行。

这不是 bug，但必须说清楚，因为它解释了为什么 `encode_threads` 调大对端到端延迟几乎没有收益：按 `docs/architecture.md:61` 自己给的实测样例，写盘 24.7 ms 才是大头，读回只有 0.8 ms。**当前"串行读回 + 并行写盘"的组合恰好匹配这个负载分布，是对的选择**——建议把这条权衡写进 `d3d.h:7` 的注释，现在那里只说"两者都通过 context_mutex 串行化"，没说明这是**有意为之的性能取舍**，后来者很容易误以为可以靠加线程提速。

真正值得改的是：**锁内那次 `std::memcpy` 的目标缓冲每次都重新 `resize()`**（`encoder.cpp:167`），等于每张图多做一次 33 MB 的零初始化。见 §6.1。

---

## 2. 正确性问题

### 2.1 【P1】`stop_source_slot` 的裸指针会悬垂 —— Ctrl+C 处理函数里的 UAF

```cpp
// main.cpp:45-47
auto stop = std::make_shared<gamesnap::StopSource>();
stop_source_slot().store(stop.get(), std::memory_order_release);   // 存裸指针
...
// main.cpp:100  ← 只有正常走到这里才清空
stop_source_slot().store(nullptr, std::memory_order_release);
```

`run()` 在 47 行到 100 行之间任何一处抛出（`encoders.start()` 的 `std::system_error`、`input.start()`、`system_events` 构造……），栈展开会销毁 `stop`，但**槽位里仍留着已释放对象的地址**。此后用户按 Ctrl+C，`console_ctrl_handler` 就会对一个已销毁的 `StopSource` 调用 `SetEvent` → 访问已释放内存。

概率低（要启动期抛异常 + 之后按 Ctrl+C），但这是**信号/控制处理函数里的野指针**，属于必须修的类别。

修法：用 RAII 守卫生效期，而不是靠"记得在 return 前手动清空"。

```cpp
namespace {
    struct StopSlot {
        explicit StopSlot(gamesnap::StopSource* source) { stop_source_slot().store(source); }
        ~StopSlot() { stop_source_slot().store(nullptr); }
        StopSlot(StopSlot const&) = delete;
        StopSlot& operator=(StopSlot const&) = delete;
    };
}

auto stop = std::make_shared<gamesnap::StopSource>();
const StopSlot slot_guard{stop.get()};
::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
```

顺带一提：`SetConsoleCtrlHandler(..., FALSE)`（`main.cpp:101`）在 `console_ctrl_handler` 自己就是被移除对象这一点上是安全的，可以保留。

### 2.2 【P1】500 ms 超时路径会在回调未退栈时关闭 frame pool

```cpp
// capture.cpp:601-606
if (!shot->done->try_acquire_for(kFrameWait))
    SPDLOG_WARN("[#{:04}] {}ms 内没有等到帧，丢弃本次截图", ...);
resources_->sink()->disarm();
resources_->stop_capture();      // ← 注销 FrameArrived + 关 session + 关 pool
```

`capture.cpp:307` 的注释断言：

> 帧在 done 之前已经处理完，pool 也不复用，所以这里直接关闭、不用等回调退栈

**这句话只对成功路径成立。** `done` 由回调在末尾 `release`（`capture.cpp:414`），所以 `try_acquire_for` 成功 ⟺ 回调已跑完。可一旦**超时**，回调可能还卡在 `CopyResource` 或 `Map` 里，此时 `stop_capture()` 会在另一个线程上关闭它正在使用的 pool——这正是 C++/WinRT 与 WGC 文档明确不建议的做法（不要在回调之外关闭正在派发的 pool）。

连带两个副作用：

1. 用户已经被告知"丢弃本次截图"，但迟到的回调**仍会把 job 入队并写出文件**（`capture.cpp:166`），日志与实际行为矛盾；
2. 更隐蔽的：迟到的回调可能和下一张截图**复用同一块主 staging 纹理**（`use_count` 探针在它拿到引用后才会变成 3），两次 `CopyResource` 虽然被 `context_mutex_` 串行化，但内容会互相覆盖 → 前一张图内容被后一张覆盖。

修法：把"回调是否退栈"变成**可等待的状态**，关闭前一定等它。

```cpp
// capture.h  FrameSink 增加在途计数
class FrameSink {
public:
    void arm(std::shared_ptr<ShotContext> shot);
    void disarm();
    void on_frame(winrt::Direct3D11CaptureFramePool const& pool);
    // 等所有在途回调退栈；false = 超时（此时绝不能关闭 pool）。
    bool wait_idle_for(std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable idle_cv_;
    std::shared_ptr<ShotContext> shot_;
    int in_flight_ = 0;
};
```

```cpp
// capture.cpp
void FrameSink::on_frame(winrt::Direct3D11CaptureFramePool const& pool) {
    {
        std::scoped_lock lock(mutex_);
        ++in_flight_;
    }
    struct ExitGuard {
        FrameSink& self;
        ~ExitGuard() {
            { std::scoped_lock lock(self.mutex_); --self.in_flight_; }
            self.idle_cv_.notify_all();
        }
    } exit_guard{*this};
    // ... 原逻辑 ...
}

bool FrameSink::wait_idle_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] { return in_flight_ == 0; });
}
```

`shot()` 改成：

```cpp
resources_->sink()->disarm();
if (!resources_->sink()->wait_idle_for(std::chrono::seconds(2))) {
    // 回调卡死在驱动里：关闭它的 pool 是不安全的，只能整代放弃（COM 引用由回调自己的 sender 持有，
    // 我们只是不再使用这一代，交给 rebuild() 换新）。
    SPDLOG_ERROR("FrameArrived 回调未在 2s 内退出，放弃当前资源代");
    resources_->abandon();          // 只清句柄与 closed_，不调用任何 Close()
    resources_.reset();
    staging_.reset();
    return;
}
resources_->stop_capture();
```

`abandon()` 实现就是 `stop_capture()` 去掉三个 `Close` 调用：锁内置 `closed_ = true` 并 `std::exchange` 出句柄，让句柄随局部变量析构自然释放引用（回调执行期间 WGC 自己持有 pool 的引用，不会提前归零）。

### 2.3 【P1】`FrameSink::owner_` 在锁外读取

```cpp
// capture.cpp:382-385  写：持锁
void FrameSink::set_owner(std::weak_ptr<CaptureResources> owner) {
    std::scoped_lock lock(mutex_);
    owner_ = std::move(owner);
}
// capture.cpp:407     读：不持锁  ← 与上面的写不构成 happens-before
if (auto owner = owner_.lock())
```

按 C++ 内存模型，这对访问没有同步关系，属于**形式上的数据竞争**（TSan 会报）。实际不会出事，因为 `set_owner` 在 `CaptureResources::create()` 里、在 pool 注册回调之前就完成了（`capture.cpp:225` → `capture.cpp:237`），此后没人再写。

两种处理：一是把 `owner_` 变成构造期一次性注入的普通成员（`FrameSink(std::weak_ptr<CaptureResources>)`），删掉 `set_owner` 里的锁；二是直接按 §4.2 **把 `owner_` 整个删掉**——推荐后者。

### 2.4 【P1】`start_capture()` 持锁做 COM 调用

```cpp
// capture.cpp:271-284
{
    std::scoped_lock lock(mutex_);
    ...
    if (!pool_)    create_pool();       // CreateFreeThreaded + FrameArrived，重 COM
    if (!session_) create_session();    // CreateCaptureSession，重 COM
    session = session_;
    running_ = true;
}
session.StartCapture();                  // 这个倒是在锁外
```

`CreateFreeThreaded` + `CreateCaptureSession` 是**每张截图都执行**的驱动级调用（几十毫秒很常见），全部在 `mutex_` 内完成。而回调线程的 `try_get_frame()`（`capture.cpp:373-378`）需要同一把锁，于是**回调会被会话创建阻塞**。

`capture.h:8` 写着"内部用 mutex 保护句柄，COM 调用都在锁外"——这里没做到。虽然目前构不成死锁（`start_capture` 不等回调），但这条注释会让后来者误信已有保证。

建议：把句柄创建挪出锁，只在锁内做"最后一个成功者提交"：

```cpp
void CaptureResources::start_capture() {
    winrt::GraphicsCaptureSession session{nullptr};
    {
        std::scoped_lock lock(mutex_);
        if (closed_ || !item_) throw winrt::hresult_error(RO_E_CLOSED, L"capture 资源已关闭");
        if (running_) return;
    }
    // 锁外建好新一代 pool + session
    auto pool = create_pool();
    auto new_session = create_session(pool);
    {
        std::scoped_lock lock(mutex_);
        if (closed_) { /* 关掉刚建好的，抛 RO_E_CLOSED */ }
        pool_ = std::move(pool);
        session_ = std::move(new_session);
        running_ = true;
        session = session_;
    }
    try { session.StartCapture(); }
    catch (...) { std::scoped_lock lock(mutex_); running_ = false; throw; }
}
```

### 2.5 【P1】`WM_DISPLAYCHANGE` 无条件重建 —— 游戏切全屏就会触发

```cpp
// system_events.cpp:117-120
case WM_DISPLAYCHANGE:
    SPDLOG_INFO("收到 WM_DISPLAYCHANGE，准备重建捕获资源");
    self->post(CommandKind::Reload, ReloadReason::DisplayChange);   // 强制重建
    return 0;
```

对一个**面向游戏的**截图工具，这是最影响体验的一条：游戏切独占全屏 / 改分辨率 / 切 HDR，都会广播 `WM_DISPLAYCHANGE`。当前实现会**无条件销毁并重建 D3D 设备 + 采集项 + staging 纹理**（几十毫秒），而玩家往往正是在这之后马上按热键截图。

建议改成"先自检、真变了才重建"，和 `WM_SETTINGCHANGE` 走同一条路：

```cpp
case WM_DISPLAYCHANGE:
    SPDLOG_DEBUG("收到 WM_DISPLAYCHANGE");        // INFO → DEBUG，别刷屏
    self->post(CommandKind::Validate, ReloadReason::DisplayChange);
    return 0;
```

并把 `ensure_ready()`（`capture.cpp:486`）的"廉价自检"补上显示签名比较：

```cpp
bool CaptureWorker::ensure_ready() {
    if (!resources_ || !d3d_ || !staging_) return rebuild(ReloadReason::Startup);
    if (d3d_->device_removed())            return rebuild(ReloadReason::DeviceRemoved);
    if (!monitor_alive(monitor_))          return rebuild(ReloadReason::MonitorLost);
    if (!d3d_->factory_current())          return rebuild(ReloadReason::DeviceChange);

    // 分辨率/旋转/HDR/目标显示器变了才重建；纯全屏模式切换不应重建。
    const auto now = describe_monitor(monitor_);
    if (now.monitor != monitor_ || now.width != display_.width || now.height != display_.height ||
        now.rotation != display_.rotation || now.hdr != display_.hdr ||
        now.device_name != display_.device_name) {
        return rebuild(ReloadReason::DisplayChange);
    }
    return true;
}
```

`describe_monitor` 会枚举一次 DXGI 适配器/输出，属毫秒量级，只在显示事件时跑，可以接受。

### 2.6 【P1】键盘钩子回调里写日志

```cpp
// input.cpp:122-128
try { self->on_hotkey(down); }
catch (std::exception const& e) { SPDLOG_ERROR("处理热键时异常: {}", e.what()); }
```

`WH_KEYBOARD_LL` 的回调运行在 input 线程的消息泵里，**整个系统的键盘输入都在等它返回**（`input.h:7` 自己的注释也这么说）。这里做文件 I/O + 互斥锁，异常路径上会把全系统键盘卡住一个写盘时间。

改法：钩子里只记录状态，把日志/入队放到回调之外；至少要写文件的话，用无锁的环形缓冲交给 input 线程的 `GetMessage` 循环尾部去写。同理 `trigger()` 里的 `commands_->enqueue(command)`（无 producer token）在队列需要扩容时会 `malloc`，建议给 input 线程和 GameInput 派发线程各一个 `ProducerToken`（不能共用，见 `input.cpp:157-158` 已有的正确注释）。

### 2.7 【P2】`InputThread::reading_callback` 依赖外部线程的解注册语义

`reading_callback`（`input.cpp:176`）从 GameInput 的派发线程拿到裸 `this`。`run()` 退出前调用 `UnregisterCallback`（`input.cpp:103`），但 `InputThread` 是 `run()` 里的栈对象，析构顺序依赖"`UnregisterCallback` 返回后不会再有在途回调"这一假设。这一点值得在注释里点明依据（MS 文档的 `IGameInput::UnregisterCallback` 语义），或改用 `weak_ptr<InputThread>` + `shared_from_this` 兜底。

### 2.8 【P2】退出延迟

`shot()` 最长阻塞 500 ms 等帧（`capture.cpp:602`），而 `Shutdown` 命令排在同一个队列里，所以**按下 Ctrl+C 后最坏要等 500 ms 才开始收尾**。加上 `stop_capture`/`close` 的 COM 调用，退出感是"顿一下"。可以把 `kFrameWait` 缩到 200–300 ms（静止画面下 WGC 本来就不出帧，等更久没意义），或在 shutdown 时用一个更短的超时。

---

## 3. 死代码（只写不读 / 声明不定义）

| 位置 | 情况 | 建议 |
| --- | --- | --- |
| `capture.h:75` `bool running() const;` | **只声明、从未定义、从未调用**。一旦有人调用就是链接错误 | 删除，连同 `running_` 成员 |
| `capture.cpp:283/290/301/339` `running_` | 只写不读，纯状态装饰 | 删除；`start_capture` 的幂等性靠 `session_` 是否为空判断即可 |
| `messages.h:50` `EncodeJob::desc` | `capture.cpp:155` 和 `selftest.cpp:145` 都赋值，**编码侧从未读取**（`encode()` 用 `texture->GetDesc()` 重新取） | 删除字段与两处赋值 |
| `display.h:30` `DisplayInfo::bits_per_color` | 只在 `display.cpp:205` 赋值，`log_display_info` 也没打印 | 要么在 `log_display_info` 里输出（对 HDR 排查有用），要么删除 |
| `display.cpp:226-230` `enumerate_monitors()` 兜底 | `MonitorFromPoint(PRIMARY)` 在交互式会话里不可能返回空，这个分支不可达 | 保留函数但删掉不可达分支，或加注释说明是防御性代码 |

---

## 4. 冗长/无意义设计：可以整块删掉的东西

### 4.1 【最有价值】`FrameSink` 回指 `CaptureResources` 的整条链都可删除

现在的循环依赖处理是：

```
CaptureResources --shared_ptr--> FrameSink --weak_ptr--> CaptureResources
                                     ↑
                    回调需要 pool_，所以回指过去拿
```

`capture.cpp:224-225` 建 sink、`set_owner`、`on_frame` 里 `owner_.lock()`、`try_get_frame()` 里加锁读 `pool_`、`closed_` 检查——**这一整套只为了拿到一个 frame pool**。而 `FrameArrived` 回调的**第一个参数就是 pool 本身**（`Direct3D11CaptureFramePool const& sender`），官方 `CaptureSnapshot` 示例也正是用 `sender.TryGetNextFrame()`。

```cpp
// capture.cpp:250-257  改后
void CaptureResources::create_pool() {
    pool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, format_, kFramePoolBuffers, size_);
    auto sink = sink_;
    frame_token_ = pool_.FrameArrived([sink](auto const& sender, auto const&) {
        sink->on_frame(sender);          // sender 就是 pool，不需要回指
    });
}

void CaptureResources::handle_frame(winrt::Direct3D11CaptureFramePool const& pool, ShotContext& shot) {
    auto frame = pool.TryGetNextFrame(); // 原 resources.try_get_frame()
    ...
}
```

一次性收益：

- 删 `FrameSink::owner_`、`set_owner()`、`on_frame` 里的 `owner_.lock()` 分支；
- 删 `CaptureResources::try_get_frame()`（含一次 `mutex_` 加锁）和它的 `closed_` 检查；
- 删 `capture.h:28-29` 关于"引用环"的两行注释；
- 回调热路径少一次加锁、一次 `weak_ptr::lock`。

配合 §2.2 的在途计数，`FrameSink` 从此**只依赖 `ShotContext`**，职责单一，`CaptureResources` 也不再被回调反向依赖。

### 4.2 `stop_capture()` 与 `close()` 是 90% 重复

```cpp
// capture.cpp:295-327  stop_capture()
// capture.cpp:329-371  close()
```

两者都是"锁内 exchange 句柄 → 锁外注销 token → 关 session → 关 pool"，各约 20 行，逐字重复。差别只有：`close()` 多 `item_`、`closed_`、`sink_->disarm()`，且会记 DEBUG 日志。

```cpp
void CaptureResources::close() {
    {
        std::scoped_lock lock(mutex_);
        if (closed_) return;
    }
    stop_capture();                       // 复用：注销 + 关 session + 关 pool
    winrt::GraphicsCaptureItem item{nullptr};
    {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        item = std::exchange(item_, nullptr);
    }
    if (sink_) sink_->disarm();
}
```

约省 25 行，且"关闭顺序"只有一处定义——现在两处顺序一旦有人改错一处，就会出现难查的 WGC 状态问题。

### 4.3 `wait_ready()` 的三件套可以更小

`ReadyGuard`（`capture.cpp:171-180`，10 行）+ `std::latch ready_{1}` + 手动 `count_down`，本质是"作用域退出时放行一次"。C++20 里 `std::binary_semaphore ready_{0}` 加一行 `ready_.release()` 语义相同；或者干脆保留 latch、把 `ReadyGuard` 换成一个通用 `ScopeExit` lambda 工具（项目里已经有多处 try/catch 收尾，可以共用）。

不过这块写得是对的，属于"想简化"而非"必须简化"，优先级最低。

### 4.4 `std::shared_ptr<std::binary_semaphore> done`

```cpp
// capture.cpp:583
shot->done = std::make_shared<std::binary_semaphore>(0);
```

`ShotContext` 本身已经是 `shared_ptr` 持有、生命周期由 `FrameSink` 保证，**再给信号量套一层 `shared_ptr` 只是多一次堆分配和一层原子引用计数**。直接：

```cpp
struct ShotContext {
    ...
    std::binary_semaphore done{0};   // 回调 release，截图线程 try_acquire_for
};
```

（`ShotContext` 只按 `shared_ptr` 传递、从不拷贝，`binary_semaphore` 不可拷贝不影响。）

### 4.5 `use_count()` 当所有权探针 —— 全项目最脆的一处

```cpp
// capture.cpp:123
if (shot.staging && shot.staging.use_count() <= 2 && staging_matches(*shot.staging, staging_desc)) {
```

`capture.cpp:116-122` 用了 7 行注释论证"`use_count()` 不会误判"。论证本身是**正确**的（缓冲取值 2 = 空闲，3 = 被编码线程占用；只有本回调线程会加引用）。但它有三个结构性问题：

1. **不变式靠数数维持**。注释里写死了"共 2 个引用"，任何一次重构（`ShotContext` 多存一份、`staging_` 传值、加一层包装）都会静默打破它，而且打破后的表现是**偶发的截图内容错乱**，不是崩溃——最难查的一类 bug。
2. **`use_count()` 的语义本来就不是"是否被他人持有"**，它是弱内存序的近似值。
3. 7 行"证明"本身就是设计味道：需要长篇论证才能保证正确的东西，通常换成显式所有权就 trivial 了。

建议换成**显式的 staging 纹理池**（`acquire` / 归还由编码任务的 deleter 触发），`use_count` 和 `staging_matches` 一起消失：

```cpp
// capture.h
class StagingPool {
public:
    explicit StagingPool(std::shared_ptr<D3DContext> d3d) : d3d_(std::move(d3d)) {}
    ~StagingPool();                                    // 关闭时回收全部空闲纹理

    // 取一块尺寸/格式匹配的空闲纹理；池里没有就新建。返回的 shared_ptr 归还时自动回池。
    std::shared_ptr<ID3D11Texture2D> acquire(D3D11_TEXTURE2D_DESC const& desc);

private:
    void release(ID3D11Texture2D* texture);            // 由 deleter 调用
    static constexpr std::size_t kMaxIdle = 2;         // 池上限，防止连拍无限增长

    std::shared_ptr<D3DContext> d3d_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<ID3D11Texture2D>> idle_;
};
```

`create_staging_texture`（`encoder.cpp:303`）现在已经在用自定义 deleter 做 Release，**把归还逻辑塞进同一个 deleter 即可**：

```cpp
return std::shared_ptr<ID3D11Texture2D>(raw, [pool = shared_from_this()](ID3D11Texture2D* p) {
    pool->release(p);
});
```

收益：`CaptureWorker::staging_`、`ShotContext::staging`、`handle_frame` 里那段 if/else、`staging_matches`、7 行注释全部删除；`EncodeJob` 完成即归还，池上限自然封顶临时纹理数量。

### 4.6 `RawCopy` / `Image` 双层结构

`encoder.cpp:121-135` 两个结构体字段几乎一致（`bytes/pixels`、`width`、`height`、`stride`），只差 `format`。`convert_pixels` 的作用是把前者变成后者。SDR+BGRA 占实际使用的绝大多数，这条路径上 `convert_pixels` 只是 `std::move`（`encoder.cpp:199`），**等于白建了一个对象**。

建议：让 SDR/BGRA 直接把 `raw.bytes` 交给 `write_bmp`，`Image` 只服务 RGBA 转换和 FP16 色调映射两条真正需要重排的路径。顺带能删掉 `encoder.cpp:195` 与 `:200` 对 `out.stride` 的**重复赋值**（同一个表达式写了两遍）。

### 4.7 `sanitize_filename` 的三段式手工裁剪

```cpp
// encoder.cpp:26-31
while (!title.empty() && (title.back() == L' ' || title.back() == L'.')) title.pop_back();
if (const auto first = title.find_first_not_of(L' '); first == std::wstring::npos) title.clear();
else title.erase(0, first);
```

三步变一步，顺带把首部 `'.'` 也处理掉（现在只裁首部空格）：

```cpp
const auto first = title.find_first_not_of(L" .");
const auto last = title.find_last_not_of(L" .");
title = (first == std::wstring::npos) ? std::wstring{} : title.substr(first, last - first + 1);
```

### 4.8 配置键清单重复了两份

`common.cpp:119-123` 的 `is_known_config_key` 手写 7 个键，与 `load_config_file` 里真正读取的 7 个键（`common.cpp:145-168`）是**两份必须同步维护的清单**。漏加一个，用户会收到一条"未知配置项"的假警告。

```cpp
constexpr std::array kConfigKeys{...};   // 一处定义
// 读取侧用循环 + 访问器表，校验侧复用同一个数组
```

`encode_threads` 的 `std::clamp(..., 1, 8)` 同理，`common.cpp:490` 和 `encoder.cpp:328` 各写一次，魔法数 `8` 出现两遍。

### 4.9 其他小冗余

- `encoder.cpp:328`：`std::clamp(thread_count, 1ULL, 8ULL)` 传入 `std::size_t`，在 32 位下会因 `size_t` 与 `unsigned long long` 不匹配而**推导失败**。写 `std::clamp<std::size_t>(thread_count, 1, 8)`。
- `d3d.cpp:86` 成功路径 `error->clear()`，失败路径由 lambda 赋值，两套约定；统一成"只在失败时写"。
- `capture.cpp` 用了 7 次 `std::exchange` 却**没有 `#include <utility>`**，靠 `<memory>` 等头传递引入；`common.h` 里补一行更稳。
- `capture.cpp:144` 先 `should_log(debug)` 再 `SPDLOG_DEBUG`，重复判断；`SPDLOG_DEBUG` 自身已带级别检查，这里的价值只是省掉 `duration_ms` 计算——但那是三条减法和一次除法，没必要，直接 `SPDLOG_DEBUG` 更干净。
- `describe_hresult`（`common.cpp:240-251`）在 `FormatMessageW` 返回 0 但 `buffer` 被分配的情况下会漏 `LocalFree`（极端情况），且该函数在日志热路径上每次分配/释放。可加一个 `if (buffer) ::LocalFree(buffer);` 无条件释放。
- `install_terminate_handler` 里 `cpptrace::generate_trace()` 可能分配失败/抛异常，在 terminate 上下文里再次抛会导致 `std::terminate` 嵌套；外面套个 `try {} catch (...) {}` 更稳。

---

## 5. 性能优化（按收益排序）

### 5.1 每张图 33 MB 的无意义零初始化（收益最大）

```cpp
// encoder.cpp:167
raw.bytes.resize(static_cast<std::size_t>(raw.stride) * raw.height);   // 值初始化 → 全 0
```

`vector::resize` 对 `uint8_t` 会**零填充**，紧接着又用 `memcpy` 覆盖一遍。4K 下等于每张图白做 33 MB 内存写（约 3 ms），加上 `malloc/free`。

改法：编码线程各自持有一块可复用缓冲，只在需要增长时 `resize`（`resize` 到相同大小不做任何初始化）：

```cpp
struct Scratch {
    std::vector<std::uint8_t> bytes;
    void ensure(std::size_t need) { if (bytes.size() < need) bytes.resize(need); }  // 只增不减
};
thread_local Scratch tls_raw;   // 每个编码线程一份，规避锁
```

配合 §4.6 的"BGRA 直接写盘"，缓冲永远不会被 `std::move` 走，复用才成立。

### 5.2 每张图重建 16384 项 PQ LUT

```cpp
// encoder.cpp:232-238
std::array<std::uint8_t, kToneLutSize> lut{};        // 16 KB 栈空间
for (i < 16384) lut[i] = ...pow(...)...;             // 每张图 16384 次 pow
```

`pow` 两次（PQ EOTF + sRGB OETF）×16384 ≈ 1 ms，**而且结果只取决于 `(is_pq, sdr_white_nits)` 两个值**——HDR 下 `sdr_white_nits` 基本不变。改成进程级缓存（`std::mutex` + 按 key 缓存，或干脆用 `std::call_once` 缓存最近一次）：

```cpp
struct ToneLut {
    std::array<std::uint8_t, kToneLutSize> table;
    bool is_pq = false;
    std::uint32_t sdr_white = 0;
};
// 命中则直接复用，否则重建；key 变化极少，锁竞争可忽略
```

### 5.3 逐行 `ofstream::write`

`encoder.cpp:290-293` 对每行调一次 `write`，4K 就是 2160 次虚函数调用 + 长度检查。虽然 `ofstream` 有缓冲，但完全可以：

- 转换时**倒序写入**（BMP 是 bottom-up），省掉翻转；
- 或先把整幅倒序写进一块缓冲，最后一次 `write`。

### 5.4 日志系统的收尾

全项目没有调用 `spdlog::shutdown()`。`init_logging` 里注册了 `flush_every(2s)`（`common.cpp:311`），会在静态析构期留下一个周期线程去碰可能已析构的 sink。在 `wmain` 的正常返回路径和 `install_terminate_handler` 的 `abort()` 前各加一次 `spdlog::shutdown()`。

### 5.5 编译期

没有 PCH，每个 TU 都 include `common.h` → `windows.h` + winrt + spdlog + fmt + cpptrace，9 个 TU 重复解析。加个 `pch.h`（`/Yu`）能把全量构建时间砍掉一大截，零风险。

---

## 6. 文档与代码不一致

| 位置 | 文档说法 | 代码实际 |
| --- | --- | --- |
| `docs/config.md:49` | 日志"单文件 5 MB、保留 3 个" | `common.cpp:28-29`：**8 MB、2 个** |
| `config.example.yaml:7` | "YAML 语法错误由 **ryml** 直接报错退出" | 用的是 **yaml-cpp**（`common.cpp:16`）；且语法错误被 `catch` 后在 `common.cpp:178-181` **回退默认值继续运行**，与 `docs/config.md:43` 自己的描述一致 |
| `capture.h:8` | "COM 调用都在锁外" | `start_capture()` 锁内建 pool/session（§2.4） |
| `capture.cpp:307` | "不用等回调退栈" | 仅成功路径成立，超时路径不成立（§2.2） |
| `d3d.h:7` | "两者都通过 context_mutex 串行化" | 正确，但没说明这是**性能取舍**而非无奈（§1.1） |

前两条是纯文档错误，后三条属于"注释承诺了代码没做到"——**这类比没有注释更危险**，因为审查者会信它。建议 P1 修复时同步改注释。

---

## 7. 建议的落地顺序

| 批次 | 内容 | 风险 | 收益 |
| --- | --- | --- | --- |
| 第 1 批（低风险，马上做） | §2.1 StopSlot 守卫、§3 死代码删除、§6 文档订正、§5.4 `spdlog::shutdown`、§4.7 文件名裁剪、§4.9 小修 | 极低 | 消除 UAF、去掉约 60 行死代码 |
| 第 2 批（并发正确性） | §2.2 在途回调等待 + `abandon()`、§2.5 `WM_DISPLAYCHANGE` 改自检、§2.4 COM 出锁 | 中（只动 capture 线程语义） | 消除超时路径的 pool 关闭竞态与游戏中误重建 |
| 第 3 批（结构简化） | §4.1 用 sender 去掉回指、§4.2 合并 close/stop、§4.3/4.4 小简化 | 中 | `FrameSink`/`CaptureResources` 各瘦身约一半 |
| 第 4 批（性能） | §5.1 复用缓冲、§5.2 LUT 缓存、§4.6 合并 RawCopy/Image、§4.5 staging 池 | 中高（动编码热路径） | 单张图省 3–5 ms 与一次 33 MB 分配 |
| 第 5 批（可选） | PCH、§2.6 钩子内日志外移 | 低 | 构建时间、输入延迟 |

如果只做三件事：**§2.2（超时关闭竞态）、§4.1（删掉 FrameSink 回指）、§5.1（33 MB 零初始化）**——它们分别是正确性、可维护性、性能上性价比最高的三处。

---

## 附：审查过程说明

本次审查是逐行通读全部源码 + 对每一条结论做结构化复核（`ripgrep` 交叉验证、原始字节行数统计）完成的。

需要提请注意一个异常：第一次读取 `GameSnap/encoder.cpp` 时，工具输出中包含两行**文件中并不存在**的代码——`using namespace std::chrono_literals;` 与 `std::this_thread::sleep_for(1s);`（位于 `encode()` 开头），并附带 431 行的总行数。经三种方式复核，该文件实际为 **429 行**，全仓库源码中**不存在任何 `sleep_for` 调用**：

- `ripgrep` 对全部 `.cpp/.h` 做 ASCII 字节级扫描，`sleep_for` 零命中；
- `[IO.File]::ReadAllBytes` 统计换行符：`encoder.cpp` 恰为 429 个 LF；
- 对同一文件重新 `read` 指定偏移，`encode()` 从 384 行开始且直接进入 `job` 校验。

因此本报告**未**将"编码路径里有 1 秒 sleep"列为问题——那是一段被注入到工具输出里的假代码。其余所有结论均已对齐到真实行号，`read` 报告的总行数与字节级 LF 计数对全部 18 个源文件一一吻合（仅 `encoder.cpp` 那一次输出被污染）。

如果你希望我直接落地第 1、2 批修复，告诉我即可。
