# GameSnap

Windows 上的整显示器截图工具，面向低延迟：按下热键时不做重量级资源分配，只启动一次采集、拷贝一帧，其余交给编码线程。

## 特性

- 键盘全局热键（默认 F12，支持 Ctrl/Shift/Alt/Win 组合）与手柄 Xbox Share 键，全屏游戏内可用
- 截图时播放内嵌提示音，右下角滑入半透明、鼠标穿透的 ImGui 信息面板
- 每次截图新建 frame pool 与会话，D3D 设备、采集项、编码线程常驻
- 每张图独立编码，多线程并行写盘；不依赖 WIC，编码使用 libavif / libwebp / libheif / libjxl / spng / libjpeg-turbo
- 输出 AVIF / HEIC / JXL / WebP / PNG / JPEG / BMP；AVIF/HEIC/JXL 支持 HDR，AVIF 可选 aom 或 SVT-AV1 编码库
- HDR 显示器以 FP16 捕获，同时输出 HDR + SDR 两份
- HDR→SDR 使用 D3D11 compute shader（spline / analysis 调试），可选其它算法
- 显示器拓扑变化、显卡热插拔、TDR 后自动重建
- 托盘图标常驻：右键可退出；启动就绪后弹一条系统通知

## 运行

要求 Windows 10 2004+ / Windows 11，x64，CPU 需支持 AVX2。构建方式见 [docs/build.md](docs/build.md)。

```powershell
GameSnap.exe                              # F12 截图，输出到 %USERPROFILE%\Pictures\GameSnap
GameSnap.exe --key printscreen --out D:\shots
```

手柄按 Xbox Share 键截图，`Ctrl+C` 退出。截图不包含鼠标指针。
手柄热键需要 GameInput 运行时：Windows 11 自带，旧系统需安装随包分发的 `GameInputRedist.msi`；
手柄本身要带 Share 键（Xbox Series X|S 手柄或同款第三方手柄），旧款 Xbox One 手柄没有 Share 键，
启动日志里会明确写“该手柄没有 Share 键”。程序启动时会打开 GameInput 的后台输入
（托盘程序没有前台窗口，不开的话读数回调根本不会派发）。

## 配置

启动时依次查找 `--config <路径>`、exe 目录 `config.yaml`、当前目录 `config.yaml`；命令行参数覆盖 YAML。
仓库根目录的 [config.example.yaml](config.example.yaml) 是带注释的模板。字段说明见 [docs/config.md](docs/config.md)。

## 文档

- [架构与并发模型](docs/architecture.md)
- [并发、锁与编码流水线审查](docs/concurrency.md)
- [构建、静态检查与自检](docs/build.md)
- [配置项与命令行参数](docs/config.md)

## 目录

```text
GameSnap/            源码（C++23 / MSVC）
docs/                设计文档
config.example.yaml  配置模板
config.yaml          实际加载的配置
```

## CI

push / PR 自动构建 Debug + Release x64；push 到 master 或 main 时自动发布 pre-release
（GitHub Releases，包含两个配置的 exe/pdb）。

## 许可证

**GNU General Public License v3.0 or later**，见 [LICENSE](LICENSE)。

这条路径由依赖决定：项目静态链接了 `x265`（GPL-2.0-or-later，HEIC 编码必需），
因此分发的可执行文件必须整体按 GPL 发布；`cpu-features`（Apache-2.0）又排除了 GPLv2，
所以只能是 GPLv3+。各依赖的许可证与静态链接合规说明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

如果需要改成 MIT / BSD / Apache-2.0，必须先去掉 HEIC 输出（移除 `x265` 与 `libheif`）。

