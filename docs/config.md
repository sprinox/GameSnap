# 配置与命令行

## 查找顺序

1. `--config <路径>`
2. exe 目录下的 `config.yaml`
3. 当前目录下的 `config.yaml`

YAML 先加载，命令行参数随后覆盖。**首次启动**如果三处都没有配置文件，程序会在 exe 目录自动生成一份
带注释的 `config.yaml`（等价于 [config.example.yaml](../config.example.yaml)），直接改就行。

## YAML 字段

| 键 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `output_dir` | 字符串 | `%USERPROFILE%\Pictures\GameSnap` | 截图目录，不存在时自动创建 |
| `filename_pattern` | 字符串 | `{title}_{time}_{seq}` | 文件名模板，见下表 |
| `encode_threads` | 整数 1–8 | `2` | 编码线程数，决定并行写盘能力 |
| `hotkey` | 字符串 | `F12` | `F1..F24`、字母/数字、`printscreen`、`scrolllock`、`insert`、`home`、`end`、`space`、`0x7B`；可加 `ctrl+` / `shift+` / `alt+` / `win+` |
| `swallow` | 布尔 | `false` | 是否吞掉热键，`true` 时游戏收不到 |
| `monitor` | 字符串 | `primary` | `primary`：主显示器；`foreground`：前台窗口所在显示器 |
| `format_hdr` | 字符串 | `avif` | HDR 容器，仅 `avif` / `heic` / `jxl` |
| `format_sdr` | 字符串 | `jxl` | SDR 格式：`bmp` / `png` / `jpeg` / `webp` / `avif` / `heic` / `jxl`（默认 JXL 无损，见 `docs/benchmark.md`） |
| `tone_map` | 字符串 | `spline` | HDR→SDR 算法：`spline` / `analysis` |
| `tone_map_backend` | 字符串 | `auto` | HDR→SDR 计算后端：`auto` / `gpu` / `cpu` |
| `png_compression` | 整数 0–9 | `6` | PNG 压缩级别，越高越慢/越小 |
| `jpeg_quality` | 整数 1–100 | `95` | JPEG 质量 |
| `webp_quality` | 整数 0–100 | `90` | WebP 质量 |
| `webp_lossless` | 布尔 | `false` | WebP 无损 |
| `webp_method` | 整数 0–6 | `4` | WebP 编码方法，越高越慢/越小 |
| `avif_quality` | 整数 0–100 | `90` | AVIF 质量 |
| `avif_lossless` | 布尔 | `false` | AVIF 无损 |
| `avif_speed` | 整数 0–10 | `10` | AVIF 速度，越高越快 |
| `heic_quality` | 整数 0–100 | `90` | HEIC 质量 |
| `heic_lossless` | 布尔 | `false` | HEIC 无损 |
| `heic_preset` | 字符串 | `fast` | x265 预设，`ultrafast`..`veryslow` |
| `jxl_distance` | 浮点 0–15 | `0.0` | JXL 距离：0 数学无损（需 `jxl_lossless: true`），1.0 视觉无损，越大越糊、文件越小 |
| `jxl_lossless` | 布尔 | `true` | JXL 无损（modular）；逐字节无损，解码结果与输入完全一致 |
| `jxl_effort` | 整数 1–10 | `5` | JXL 编码强度，越高越慢、文件越小（4K 实测：1≈0.3 s/10.7 MB，5≈5 s/8.0 MB，7≈11 s/7.7 MB） |
| `av1_encoder` | 字符串 | `auto` | AVIF 用哪个 AV1 编码库：`auto`（libavif 自选）/ `aom` / `svt` |
| `aom_cpu_used` | 整数 -1–8 | `-1` | aom 的 `-cpu-used`；-1 表示由 `avif_speed` 推导 |
| `svt_preset` | 整数 -1–13 | `-1` | SVT-AV1 的 preset；-1 表示由 `avif_speed` 推导 |
| `chroma_subsampling` | 字符串 | `auto` | YUV 色度采样：`auto`（= 444）/ `444` / `422` / `420`，对 AVIF、HEIC、JPEG 生效 |
| `verbose` | 布尔 | `false` | 输出 debug 级别日志 |

布尔值使用 yaml-cpp 的规则：`true/false`、`yes/no`、`on/off`、`y/n`（不区分大小写），不接受 `1/0`。

编码器能力说明：BMP 固定无损且无参数；PNG 固定无损，`png_compression` 控制速度/体积；
JPEG 只有有损 quality，没有无损模式；WebP 支持 quality/lossless/method；
AVIF 支持 quality/lossless/speed；HEIC（x265）支持 quality/lossless/preset。

## 文件名模板

`filename_pattern` 支持这些占位符（未知的 `{…}` 原样保留）：

| 占位符 | 内容 |
| --- | --- |
| `{title}` | 截图时前台窗口的标题 |
| `{exe}` | 前台进程的 exe 名，**不含扩展名** |
| `{process}` | 前台进程的显示名：exe 版本信息里的 `FileDescription`（任务管理器“描述”那列），取不到时等于 `{exe}` |
| `{time}` | 时间戳 `YYYYMMDD_HHMMSS_mmm` |
| `{seq}` | 本次运行的截图序号（4 位，从 0001 开始） |

要点：

- 模板里**最好带 `{time}` 或 `{seq}`**，否则同一秒内的多张截图会重名覆盖；
- **模板里没出现的占位符不会去取**：不写 `{title}` 就不读窗口标题，不写 `{exe}`/`{process}` 就不
  `OpenProcess`，不写 `{process}` 就不读 exe 的版本资源，不写 `{time}` 就不查时区——都省在截图的关键路径上；
- 替换后还会做一次文件名清洗：非法字符（`< > : " / \ | ? *`）换成 `_`，去掉首尾空格和点，超长截断；
- 结果为空时回退成 `GameSnap`；HDR 截图仍会自动加 `_HDR` / `_SDR` 后缀；
- 命令行也可以用 `--filename-pattern` 临时覆盖。

示例：`{title}_{time}_{seq}`（默认）→ `Game_20261110_204005_363_0001.jxl`；
`{process} {time}` → `Sora No Kiseki the 1st 20261110_204005_363.jxl`。

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--config <路径>` | 指定 YAML 配置文件 |
| `--key <名称>` | 截图热键，取值同 `hotkey` |
| `--out <目录>` | 输出目录 |
| `--filename-pattern <模板>` | 文件名模板，占位符见上 |
| `--threads <1-8>` | 编码线程数 |
| `--monitor <primary\|foreground>` | 截主显示器 / 前台窗口所在显示器 |
| `--swallow` | 吞掉热键 |
| `--format-hdr <avif\|heic>` | HDR 输出容器 |
| `--format-sdr <格式>` | SDR 输出格式 |
| `--tone-map <spline\|analysis>` | 色调映射算法 |
| `--verbose` | 调试日志 |
| `--selftest` | 合成纹理自检编码链路，不经过 WGC |
| `--help` | 显示帮助 |

## 错误处理

配置值非法或 YAML 语法错误时，记录一条 error 日志，并整体回退到默认值；程序不会 abort。
未知键只打一条 warning，不影响加载。
空文件、顶层不是映射、缺少键都按默认值处理。

## 日志

文件位于 exe 目录的 `logs\GameSnap.log`，UTF-8，单文件 8 MB、保留 2 个。

控制台输出是**按运行时有没有控制台**自动开关的：

| 场景 | 控制台输出 |
|---|---|
| 双击启动（Windows 子系统，没有控制台） | 无，只写文件 |
| 从 cmd / PowerShell 启动 | 有：`AttachConsole(ATTACH_PARENT_PROCESS)` 挂上父控制台并重定向标准流 |
| stdout 已重定向到文件/管道 | 有，且写进重定向目标（不会去抢控制台） |

程序是 Windows 子系统，双击不会弹出控制台窗口；启动期的致命错误（CPU 不支持 AVX2、配置解析失败）
在没有控制台时会用 MessageBox 提示，不会静默退出。

格式：

```text
[时间] [级别] [文件:行 函数] 消息
```

`verbose: true` 时包含每张图的排队、取帧、拷贝、转换、写盘耗时。

## 输出格式
HDR 截图固定写两份：`<名字>_HDR.<format_hdr>` 与 `<名字>_SDR.<format_sdr>`；非 HDR 只写 SDR。
当 SDR 格式为 AVIF 时，SDR 文件内嵌 gain map（由 SDR 基图与 GPU 产出的 HDR PQ 帧计算）；HEIC 当前不写 gain map。
默认：PNG 无损 compression=6；JPEG q95；WebP q90 有损；AVIF q90/speed=10 有损；HEIC q90/preset=fast。各格式质量/无损/速度可在 YAML 中配置。
HDR 输出为 10-bit PQ / BT.2020（AVIF）；HEIC 在 x265 只有 8-bit 时自动退化为 8-bit PQ。
