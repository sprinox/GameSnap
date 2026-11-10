# GameSnap

专为游戏截图设计的截图工具

## 特性

- 截图低延迟：实测按键到得到屏幕信息只需小于 30ms（不会像 Xbox Game Bar 一样搞半天截不到图）
- 支持键盘钩子，快捷键按下后可以选择被软件消化，不会影响游戏（避免游戏内手柄键盘自动切换）
- 支持自动 HDR 截图（根据显示器设置判断）与 HDR -> SDR 自动映射，同时输出 HDR + SDR 两份图片
- 输出格式多样： AVIF / HEIC / JXL / WebP / PNG / JPEG / BMP；每种格式编码器参数（有损/无损、编码质量、色度采样……）都可调，自由地在画质/体积/速度之间权衡
- 支持连发截图，并行编码
- 高稳健性：显示器拓扑变化、显卡热插拔、TDR 后仍可运行
- 移植了 ReShade HDR Analysis 的部分功能，支持分析 HDR 截图的亮度信息
- 截图时播放内嵌提示音，右下角滑入半透明、鼠标穿透的 ImGui 信息面板，这个面板不会被截图捕获到（不会像 Nvidia App 截图一样截到弹窗）
- 托盘图标常驻：右键可退出；启动就绪后弹一条系统通知
- 支持定制文件名格式

## 运行

从 [Release](../../releases) 下载最新版。
要求 Windows 10 2004+ / Windows 11，x64，CPU 需支持 AVX2 + F16C。构建方式见 [docs/build.md](docs/build.md)。
双击启动无控制台；从终端启动时日志会同时输出到终端。
截图默认存到 `%USERPROFILE%\Pictures\GameSnap`（可在配置里改）。

手柄按 Xbox Share 键截图，右键托盘退出。截图不包含鼠标指针。
手柄热键需要 GameInput 运行时：Windows 11 自带，旧系统需安装 `GameInputRedist.msi`；

## 编码

使用更高效的编码格式和更激进的编码参数，可以极大压缩截图文件的体积。

| 项目                   | **Xbox Game Bar SDR 截图输出**   | **Nvidia HDR 截图输出**                 |
| ---------------------- | -------------------------------- | --------------------------------------- |
| 源                     | 24 张 PNG，8-bit sRGB，3840×2160 | 19 张 JXR，16-bit 浮点 scRGB，3840×2160 |
| 源体积                 | 273.82 MB                        | 514.10 MB                               |
| **JXL 无损输出**       | **115.16 MB**                    | **135.24 MB**                           |
| **压缩率**             | **2.38×**                        | **3.80×**                               |
| 体积占比               | 42.1%                            | 26.3%                                   |
| 单张压缩率 中位数      | 2.38×                            | 4.47×                                   |
| 单张压缩率 范围        | 1.74× ~ 3.09×                    | 2.55× ~ 5.63×                           |
| 单张体积 中位数 / 均值 | 4.80 MB / 4.80 MB                | 6.24 MB / 7.12 MB                       |
| 编码耗时 中位数 / 均值 | 4751 ms / 4704 ms                | 9503 ms / 9283 ms*                      |

\* HDR 那轮的耗时包含了同参数 SDR 副本的编码，只算 HDR 本体大约是 4.7–4.8 s/张（估算）。

**备注**

1. **HDR 的“无损”是相对 10-bit PQ 的无损**：源是 16-bit 浮点 scRGB，编码 JXL 之前会经过 16-bit → 10-bit PQ/BT.2020 的量化。
2. 参数两组相同：`jxl_lossless: true`、`distance 0`、`effort 5`、RGB 4:4:4。

## HDR / SDR 对比

| HDR（AVIF 10-bit PQ / BT.2020） | SDR（spline + IPT 色调映射） |
| :---: | :---: |
| [3_1080p_HDR.avif](assets/3_1080p_HDR.avif)（273 KB） | [3_1080p_SDR.avif](assets/3_1080p_SDR.avif)（254 KB） |

## 配置

启动时依次查找 `--config <路径>`、exe 目录 `config.yaml`、当前目录 `config.yaml`；命令行参数覆盖 YAML。
**首次启动若一处都没有，会在 exe 目录自动生成一份带注释的 `config.yaml`**，直接改完重启即可。
仓库根目录的 [config.example.yaml](config.example.yaml) 是同款模板。字段说明见 [docs/config.md](docs/config.md)。

## 文档

- [架构与并发模型](docs/architecture.md)
- [编码器基准](docs/benchmark.md)（开发者自用工具的测试数据，非受支持功能，可能已过时）
- [并发、锁与编码流水线审查](docs/concurrency.md)
- [构建、静态检查与自检](docs/build.md)
- [配置项与命令行参数](docs/config.md)

## 许可证

**GNU General Public License v3.0 or later**，见 [LICENSE](LICENSE)。

各依赖的许可证与静态链接合规说明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

感谢 [LINUX DO](https://linux.do/) 社区