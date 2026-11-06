// 峰值归约的入口：复用 tonemap.hlsl 里的常量、PQ 函数和 `peak` 入口。
// 单独一个文件是为了给 fxc 传不同的入口点（MSBuild 的一个 FxCompile 项只有一个 EntryPointName）。
#include "tonemap.hlsl"
