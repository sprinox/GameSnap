# 构建与静态检查

## 工具链

- Visual Studio 2026 BuildTools（v145）、Windows SDK `10.0.28000.0`、C++23（`/std:c++latest`）、x64
- 依赖由 vcpkg manifest 管理（`vcpkg.json`）：spdlog、fmt、cpptrace、concurrentqueue、yaml-cpp、cxxopts、
  libavif（启用 `aom` feature）、libwebp、libheif、x265、spng、libjpeg-turbo、cpu-features
- Debug/Release 全局按 `/arch:AVX2` 编译；启动时仍用 `cpu-features` 检查 AVX2 / F16C。
- `tonemap.hlsl` 作为项目的 `FxCompile` 项，由 MSBuild 直接调用 Windows SDK 的 `fxc` 预编译成
  `$(TargetDir)tonemap_cs.h`（例如 `x64\Release\tonemap_cs.h`），不写入源码目录；
  运行时只 `CreateComputeShader`，不再链接 `d3dcompiler`。
- 不使用 AVX-512，避免在消费级 CPU 上的兼容性问题。
- 修改 `vcpkg.json` 后执行 `vcpkg install --triplet x64-windows-static`，确保 libavif 带 AV1 编码器
- spdlog 通过 `SPDLOG_FMT_EXTERNAL` 链接 vcpkg 的 fmt，不使用 bundled fmt；代码统一 `fmt::format`
- 手柄使用 NuGet 包 `Microsoft.GameInput` 4.0.306，直接链接 `GameInput.lib`

## 构建

```powershell
msbuild .\GameSnap\GameSnap.vcxproj /p:Configuration=Release /p:Platform=x64
```

使用预编译头：`pch.h`（`/Yu`）由 `pch.cpp`（`/Yc`）生成，所有 `.cpp` 第一行包含它，全量构建约 26 秒。

Release 默认启用 clang-tidy。机器上没有 `clang-tidy.exe` 时会在 `Microsoft.Cpp.ClangTidy.targets` 报 `MSB6004`，
加 `/p:EnableClangTidyCodeAnalysis=false` 即可；编译和链接本身不受影响。

## 应用清单

`GameSnap/GameSnap.manifest` 通过 `<AdditionalManifestFiles>` 合并进 exe，声明：

- `longPathAware: true`：配合系统 LongPathsEnabled，Win32 API 可处理最长约 32767 字符的路径；
- `dpiAware: True/PM`；
- `heapType: SegmentHeap`。

## 自检

```powershell
.\GameSnap\x64\Release\GameSnap.exe --selftest --out .\test
```

用合成纹理走完“staging → Map → 转换 → 编码 → 写盘”，不经过 WGC；输出格式取配置/命令行
（`--format-sdr` / `--format-hdr`）。采集失败仍需看真实运行的日志。

## clang-format

```powershell
$clangFormat = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-format.exe"
Push-Location .\GameSnap
Get-ChildItem *.h, *.cpp | ForEach-Object { & $clangFormat --style=file --fallback-style=none -i $_.Name }
Pop-Location
```

配置为 Google 基础风格 + 4 空格 + 不限列宽，并设置 `SortIncludes: Never`：
include 顺序有语义（`windows.h` 先于 WinRT、`GameInput.h` 在 `common.h` 之后、spdlog 宏在 `spdlog` 之前），自动重排会导致编译失败。

## clang-tidy

规则见 `GameSnap/.clang-tidy`：`bugprone-*`、`cert-*`、`clang-analyzer-*`、`concurrency-*` 为 error，
其余为 warning。BuildTools 不带 LLVM，需要改用装了 LLVM 组件的 VS Community：

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$llvm    = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin"
& $msbuild .\GameSnap\GameSnap.vcxproj /t:ClangTidy /p:Configuration=Release /p:Platform=x64 `
  /p:RunCodeAnalysis=false "/p:ClangTidyToolPath=$llvm" "/p:ClangTidyChecks="
```

`ClangTidyChecks` 置空是为了让 clang-tidy 读取项目内的 `.clang-tidy`，否则 VS 默认只跑 `clang-analyzer-*`。

命名约定：私有成员尾随下划线（`device_`），常量 `k` 前缀 camelBack（`kFramePoolBuffers`）。

代码不使用 `std::string` / `std::vector` / `std::array` 的 `at()`，统一用 `[]`，避免运行时边界检查；
`.clang-tidy` 相应关闭了 `cppcoreguidelines-pro-bounds-constant-array-index`，
以及会建议改为带检查访问的 `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`。

`// NOLINT(...)` 只用于无法避免的几类情况：Win32 的 `LPARAM` / `GWLP_USERDATA` 整数与指针转换、
析构与 `noexcept` 收尾路径里的空 catch、fmt 的配置宏。


## 一键构建脚本

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Release -Rebuild -Tidy -Selftest
```

- `-Format`：格式化 `GameSnap/*.h/*.cpp`
- `-Tidy`：运行 clang-tidy
- `-Selftest`：构建后运行 `--selftest`
- `-Rebuild`：全量重建

脚本会自动查找 MSBuild、clang-format、clang-tidy。
