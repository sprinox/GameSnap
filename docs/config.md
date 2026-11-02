# 配置与命令行

## 查找顺序

1. `--config <路径>`
2. exe 目录下的 `config.yaml`
3. 当前目录下的 `config.yaml`

YAML 先加载，命令行参数随后覆盖。仓库根的 [config.example.yaml](../config.example.yaml) 是带注释的模板，
复制成 `config.yaml` 再修改。

## YAML 字段

| 键 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `output_dir` | 字符串 | `%USERPROFILE%\Pictures\GameSnap` | 截图目录，不存在时自动创建 |
| `encode_threads` | 整数 1–8 | `2` | 编码线程数，决定并行写盘能力 |
| `hotkey` | 字符串 | `F12` | `F1..F24`、`printscreen`、`scrolllock`、`insert`、`home`、`end`、`0x7B` |
| `swallow` | 布尔 | `false` | 是否吞掉热键，`true` 时游戏收不到 |
| `monitor` | 字符串 | `primary` | `primary`：主显示器；`foreground`：前台窗口所在显示器 |
| `hdr` | 布尔 | `false` | HDR 显示器以 FP16 捕获并色调映射到 SDR |
| `verbose` | 布尔 | `false` | 输出 debug 级别日志 |

布尔值使用 yaml-cpp 的规则：`true/false`、`yes/no`、`on/off`、`y/n`（不区分大小写），不接受 `1/0`。

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--config <路径>` | 指定 YAML 配置文件 |
| `--key <名称>` | 截图热键，取值同 `hotkey` |
| `--out <目录>` | 输出目录 |
| `--threads <1-8>` | 编码线程数 |
| `--monitor <primary\|foreground>` | 截主显示器 / 前台窗口所在显示器 |
| `--swallow` | 吞掉热键 |
| `--hdr` | HDR 捕获并色调映射 |
| `--verbose` | 调试日志 |
| `--selftest` | 合成纹理自检编码链路，不经过 WGC |
| `--help` | 显示帮助 |

## 错误处理

配置值非法或 YAML 语法错误时，记录一条 error 日志，并整体回退到默认值；程序不会 abort。
未知键只打一条 warning，不影响加载。
空文件、顶层不是映射、缺少键都按默认值处理。

## 日志

文件位于 exe 目录的 `logs\GameSnap.log`，UTF-8，单文件 8 MB、保留 2 个。控制台同时输出。
格式：

```text
[时间] [级别] [文件:行 函数] 消息
```

`verbose: true` 时包含每张图的排队、取帧、拷贝、转换、写盘耗时。
