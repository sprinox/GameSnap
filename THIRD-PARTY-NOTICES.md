# 第三方组件与许可证

GameSnap 本体以 **GPL-3.0-or-later** 发布（见 [LICENSE](LICENSE)）。这是由依赖决定的，不是偏好：

- `x265` 是 **GPL-2.0-or-later**，本项目**静态链接**它（`x265-static.lib`，用于 HEIC 编码）。GPL 的传染性覆盖整个可执行文件，因此分发的二进制只能是 GPL 兼容许可。
- `cpu-features` 是 **Apache-2.0**，与 GPLv2 不兼容，所以只能用 **GPLv3**（而非 GPLv2）。
- `libheif` 是 **LGPL-3.0**，与 GPLv3 兼容。

要改用 MIT / BSD / Apache-2.0，必须先移除 `x265` 与 `libheif`（即去掉 HEIC 输出），只保留 AVIF/WebP/PNG/JPEG/BMP。

## 静态链接的组件

| 组件 | 版本来源 | 许可证 | 用途 |
| --- | --- | --- | --- |
| x265 | vcpkg | GPL-2.0-or-later（另有商业授权） | HEIC（HEVC）编码 |
| libheif | vcpkg | LGPL-3.0（库）/ MIT（示例与 wrapper） | HEIC 容器 |
| libde265 | vcpkg | LGPL-3.0 | HEIC 解码（随 libheif） |
| libavif | vcpkg | BSD-2-Clause | AVIF 容器/编码 |
| libaom (aom) | vcpkg | BSD-3-Clause + AOM Patent License | AV1 编码 |
| libwebp | vcpkg | BSD-3-Clause | WebP 编码 |
| libjpeg-turbo | vcpkg | BSD-3-Clause / IJG / zlib | JPEG 编码 |
| libspng | vcpkg | BSD-2-Clause | PNG 编码 |
| zlib | vcpkg | zlib | PNG/HEIC 依赖 |
| imgui | vcpkg | MIT | 提示面板 |
| spdlog | vcpkg | MIT | 日志 |
| fmt | vcpkg | MIT | 格式化 |
| cpptrace | vcpkg | MIT | 崩溃栈 |
| concurrentqueue | vcpkg | BSD-2-Clause（其一文件为 zlib） | 队列 |
| yaml-cpp | vcpkg | MIT | 配置解析 |
| cxxopts | vcpkg | MIT | 命令行解析 |
| cpu-features | vcpkg | Apache-2.0 | CPU 特性检测 |

## 随包分发的运行时

| 组件 | 许可证 | 说明 |
| --- | --- | --- |
| Microsoft GameInput Redistributable | Microsoft 软件许可条款（可再分发） | 手柄 Share 键；Windows 11 自带，旧系统需安装 `GameInputRedist.msi` |
| Microsoft.Windows.CppWinRT | MIT | 构建期工具 |

## 合规注意事项

1. **LGPL-3.0 静态链接（libheif / libde265）**：按 LGPLv3 §4，分发二进制时需要让接收者能够重新链接到修改后的库版本。实践做法是同时在发布包中提供目标文件（`.obj`）或对应的库源码与构建步骤。
2. **GPLv3 完整对应源码**：分发 exe 时必须同时提供（或书面承诺提供）构建该 exe 的完整源码，包含 `vcpkg.json` 锁定的依赖版本。
3. **`GameSnap/screenshot.mp3`（截图提示音）不在本仓库的授权范围内**，其版权与许可是仓库维护者单独负责的部分；如来源不明，建议替换为自制或 CC0 音频。
4. 各许可证全文位于 `vcpkg_installed/<triplet>/share/<port>/copyright`，发布二进制时建议一并附上。
