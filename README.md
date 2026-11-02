# GameSnap

Windows 上的整显示器截图工具，面向低延迟：按下热键时不做重量级资源分配，只启动一次采集、拷贝一帧，其余交给编码线程。

## 特性

- 键盘全局热键（默认 F12）与手柄 LB + RB，全屏游戏内可用
- 每次截图新建 frame pool 与会话，D3D 设备、采集项、编码线程常驻
- 每张图独立编码，多线程并行写盘，不依赖 WIC / 图像库
- HDR 显示器以 FP16 捕获并色调映射到 SDR
- 显示器拓扑变化、显卡热插拔、TDR 后自动重建
- 输出 32bpp BMP

## 运行

要求 Windows 10 2004+ / Windows 11，x64。构建方式见 [docs/build.md](docs/build.md)。

```powershell
GameSnap.exe                              # F12 截图，输出到 %USERPROFILE%\Pictures\GameSnap
GameSnap.exe --key printscreen --out D:\shots
```

手柄同时按下 LB + RB 截图，`Ctrl+C` 退出。截图不包含鼠标指针。
手柄热键需要 GameInput 运行时：Windows 11 自带，旧系统需安装随包分发的 `GameInputRedist.msi`。

## 配置

启动时依次查找 `--config <路径>`、exe 目录 `config.yaml`、当前目录 `config.yaml`；命令行参数覆盖 YAML。
仓库根目录的 [config.example.yaml](config.example.yaml) 是带注释的模板。字段说明见 [docs/config.md](docs/config.md)。

## 文档

- [架构与并发模型](docs/architecture.md)
- [构建、静态检查与自检](docs/build.md)
- [配置项与命令行参数](docs/config.md)

## 目录

```text
GameSnap/            源码（C++23 / MSVC）
docs/                设计文档
config.example.yaml  配置模板
config.yaml          实际加载的配置
```
