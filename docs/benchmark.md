# 编码器基准（体积 × 速度 × 保真度）

> **关于基准模式（`--bench`）**
>
> 这是开发者自用的测试工具，**不是受支持的产品功能**：默认不编入、参数和输出格式随时可能变动、
> 也不保证向后兼容。本文件里的数字同样只是一次快照——换硬件、换 vcpkg 库版本、甚至换 `aom` 的构建选项
> 都会让结果漂移，其中的结论**可能已经过时**，只适合用来判断量级和排序，不要当成承诺或规格。
> 需要当前数据时请自己用同一台机器重跑。

本文件记录用真实样张跑出的编码器选型数据，以及由此得到的默认参数建议。
数据是一次快照，换硬件/换库版本会有差异，趋势和排序比较稳定。

## 1. 方法

- 工具：项目内置的编码基准模式（`--bench`），走的是**和截图相同的编码代码路径**
  （`set_codec_options` + `write_sdr` / `write_hdr10` / `encode_avif_gainmap`），
  不含屏幕捕获与 D3D 开销。
- 构建：Release x64，单线程，每项只跑一次（含库初始化，占比很小）。
- 源图：
  - SDR：`测试样品\SDR\10.png`，3840×2160，RGBA 原始 33,177,600 字节；
  - HDR：`测试样品\HDR\2.avif` 解码成 rgb48（2048×2048，PQ / BT.2020），取高 10 位作为输入。
- 保真度：用 ffmpeg 解码输出，和源图统一到 `gbrp10le`（HDR）或 `rgb24`（SDR）后算 PSNR，
  `inf` 表示逐字节一致。

开启基准模式需要重新编译（默认不编入）。用 `CL=` 追加宏，**不要**用
`/p:PreprocessorDefinitions=...`——那会覆盖工程里原有的 `NDEBUG;_CONSOLE;%(PreprocessorDefinitions)`：

```
msbuild GameSnap.vcxproj /p:Configuration=Release /p:Platform=x64 ^
    /p:CL="/DGAMESNAP_ENABLE_BENCH=1"
GameSnap.exe --bench <源.png> --out <输出目录>
```

16-bit PNG 会自动走 HDR 矩阵，8-bit 走 SDR 矩阵；输出目录里会写 `bench.csv` / `bench_hdr.csv`。

## 2. SDR（4K，3840×2160）

| 编码 | 体积（字节） | 压缩比 | 耗时 | 保真度 |
|---|---:|---:|---:|---|
| bmp | 24,883,254 | 1.0× | 17 ms | 无损 |
| jpeg q=100 | 9,774,970 | 2.5× | 78 ms | 有损 51.9 dB |
| png c=1 | 12,645,203 | 2.0× | 788 ms | 无损 |
| png c=6 | 10,810,073 | 2.3× | 3.1 s | 无损 |
| png c=9 | 10,655,495 | 2.3× | 10.5 s | 无损 |
| webp method=0 | 10,058,222 | 2.5× | 7.1 s | 无损 |
| webp method=4 | 9,146,834 | 2.7× | 11.8 s | 无损 |
| webp method=6 | 8,871,374 | 2.8× | 83.0 s | 无损 |
| jxl effort=1 | 10,738,922 | 2.3× | 0.30 s | 无损 |
| jxl effort=3 | 8,703,622 | 2.9× | 1.40 s | 无损 |
| **jxl effort=5** | **8,033,420** | **3.1×** | **5.0 s** | **无损** |
| jxl effort=6 | 7,919,680 | 3.1× | 6.9 s | 无损 |
| jxl effort=7 | 7,683,030 | 3.2× | 11.1 s | 无损 |
| jxl effort=9 | 7,526,128 | 3.3× | 60.5 s | 无损 |
| avif aom speed=6 (YUV444) | 8,015,163 | 3.1× | 8.0 s | 52.9 dB |
| avif aom speed=10 (YUV444) | 8,638,876 | 2.9× | 1.9 s | 52.9 dB |
| avif svt speed=6 (YUV420) | 4,777,041 | 5.2× | 3.0 s | 55.0 dB（色度有损） |
| avif svt speed=10 (YUV420) | 5,030,985 | 4.9× | 1.4 s | 55.0 dB（色度有损） |
| heic preset=fast (YUV444) | 8,194,136 | 3.0× | 2.2 s | 52.9 dB |
| heic preset=medium | 8,194,878 | 3.0× | 2.2 s | 52.9 dB |
| heic preset=slow | 8,195,039 | 3.0× | 2.3 s | 52.9 dB |

要点：

- **只有 PNG / WebP / JXL 是逐字节无损**（PSNR = inf）。AVIF / HEIC 的 `lossless` 只保证
  **YUV 域**无损，8-bit RGB→YCbCr 往返必然丢精度，实测 52.9 dB。
- **WebP 无损被 JXL 全面压制**：即使是最慢的 method=6（83 s / 8.87 MB），也比 jxl effort=5
  （5 s / 8.03 MB）又慢又大。
- **HEIC 的 preset 在无损模式下基本没用**（见 §6 的复核）：本次基准全部跑无损，fast/medium/slow
  体积差 0.01%、时间也一样；但 preset **确实生效**，在有损模式下 veryslow 比 fast 小约 28%。
- **avif aom speed=6 不划算**：比 speed=10 只小 7%，慢 4 倍。
- **jxl effort 的边际收益**：e5→e6 省 1.4% 但要 +37% 时间（60 KB/s）；e6→e7 省 3.0% 要 +61%
  时间（55 KB/s）；e7→e9 只剩 3 KB/s。**e6 在 SDR 上没有性价比**，要么 e5 要么 e7（e7 只为极限体积）。
- **avif svt 4:2:0 体积最小（4.78 MB）**，但那是色度抽样损失换来的，不属于"无损"。

## 3. HDR（2048×2048，10-bit PQ）

| 编码 | 体积（字节） | 耗时 | 保真度（vs 10-bit 源） |
|---|---:|---:|---|
| **jxl effort=7** | **1,022,814** | 2.9 s | **inf（逐字节一致）** |
| jxl effort=6 | 1,110,742 | 2.3 s | inf |
| jxl effort=5 | 1,152,220 | 2.0 s | inf |
| avif svt speed=10 (YUV420) | 1,155,721 | 0.8 s | 65.9 dB |
| jxl effort=3 | 1,222,351 | 0.8 s | inf |
| avif aom speed=6 (YUV444) | 1,261,123 | 4.1 s | 66.5 dB |
| **avif aom speed=10 (YUV444)** | **1,372,267** | **0.5 s** | 66.5 dB |
| heic preset=fast（**8-bit PQ**） | 955,494 | 0.8 s | **51.8 dB** |
| avif aom speed=10 + gain map | 2,342,548 | 1.8 s | 双流（SDR 底图 + HDR 增益） |

要点：

- **HDR 只有 JXL 能做到逐字节无损**（10-bit RGB）。AVIF 的 66.5 dB 是 10-bit YUV 往返的结果，
  肉眼看不出，但不是无损。
- **HEIC 不要用于 HDR**：当前 vcpkg 的 x265 是 8-bit，HDR 只能写 8-bit PQ，实测只有 51.8 dB，
  暗部会有分层。体积最小（955 KB）是因为它真的只存了 8 bit。
- **avif aom 必须用 speed=10**：speed=6 只小 8%，慢 8 倍。
- **gain map 是兼容方案不是压缩方案**：2.34 MB 同时装 SDR 底图和 HDR 增益，适合要给 SDR/HDR
  播放器都用的场景。
- HDR 的 jxl 边际收益比 SDR 好（e5→e6 省 3.6% / +14% 时间，e6→e7 省 7.9% / +27%），
  因为 2048² 的绝对耗时低。**4K HDR 大致是这里的 3.5 倍耗时**。

## 4. 推荐配置

### 默认（体积/速度平衡）

```yaml
format_sdr: jxl
jxl_lossless: true
jxl_effort: 5          # 4K 约 5 s；8.03 MB

format_hdr: avif
avif_speed: 10         # 1.37 MB / 0.5 s（2048²）；YUV 域无损
```

### 追求响应速度

```yaml
format_sdr: jxl
jxl_lossless: true
jxl_effort: 3          # 1.4 s / 8.70 MB（要更快用 1：0.3 s / 10.7 MB）
```

### 追求体积（真无损）

```yaml
format_sdr: jxl
jxl_lossless: true
jxl_effort: 7          # 11 s / 7.68 MB
format_hdr: jxl        # HDR 也走 JXL：2.9 s / 1.02 MB，逐字节无损
```

### 兼容性优先

```yaml
format_sdr: png
png_compression: 6     # 3.1 s / 10.81 MB
```

### 明确不推荐

| 项 | 原因 |
|---|---|
| `webp` 无损 | 同体积被 JXL 全面压制（更慢且更大） |
| `jxl_effort: 9` | 比 e7 只小 2%，时间 5.5 倍 |
| `heic` 作为 HDR 容器 | 只能是 8-bit PQ，51.8 dB，暗部会分层 |
| `avif_speed: 6` | 比 10 只小 8%，慢 4~8 倍 |
| `heic_preset: slow/medium`（**仅无损模式**） | 无损模式下体积/时间与 fast 几乎无差别（有损模式仍值得调） |

## 5. 已知限制

- AVIF / HEIC 的 `*_lossless` 是 **YUV 域**无损：RGB→YCbCr 的量化误差无法避免。想让 AVIF 逐字节无损
  需要 identity 矩阵 + 4:4:4，但那条路在这套代码里已知会让颜色偏（之前修过一个红蓝/色偏问题），
  所以没有采用。
- 上面的耗时是单张、冷启动后第一次编码，重复编码会略快（库内部有缓存）。
- SDR/HDR 各只测了一张样张；`7.png` 复测过排序一致，换图可能导致个别排序变化（尤其 jxl e3 与 heic 接近）。
- **gain map 的体积只能看量级**：基准里的 SDR 底图是 `pq_to_nits / 203 nit` 直接截断 + sRGB 编码，
  没有走真实的 spline 色调映射；真实链路里 base 与 alt 的差异更小，文件会略小。

## 6. HEIC preset 复核（2026-11 追加）

第 2 节的“preset 基本没用”容易被误读成“preset 没传进去”，所以单独复核了一遍：

1. `heif_encoder_list_parameters()` 输出：`quality, lossless, preset, tune, tu-intra-depth, complexity, chroma`
   → `preset` 在列表里，名字正确。
2. 设置后 `heif_encoder_get_parameter(encoder, "preset", ...)` 回读：请求 `veryslow` → 回读 `veryslow`；
   请求 `fast` → 回读 `fast` → **参数确实被接受**。
3. 有损（quality=90）对比：`fast` 5,920 字节，`veryslow` 4,262 字节 → **小 28%**，preset 生效。
4. 无损（`heic_lossless: true`）对比：fast 10,841 / veryslow 10,752（0.8%），个别用例甚至 veryslow 更大。

结论：x265 的 preset 主要影响有损量化下的率失真搜索，`qp=0` 时熵编码占主导，preset 收益被抹平。
基准全部跑无损，所以看起来“没用”。**有损 HEIC 场景仍然值得把 preset 调到 slower/veryslow**；
无损场景用 fast 就行。
