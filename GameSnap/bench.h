#pragma once

#include <filesystem>
#include <string>

#include "common.h"

// 编码基准默认不编译（Release / Debug 都关）：它只是选型工具，不属于截图链路。
// 需要时加 /DGAMESNAP_ENABLE_BENCH=1 重新编译。
#if defined(GAMESNAP_ENABLE_BENCH)
#define GAMESNAP_BENCH 1
#else
#define GAMESNAP_BENCH 0
#endif

namespace gamesnap {

#if GAMESNAP_BENCH
    // 编码器基准：把一张 PNG 用各种格式/参数重新编码，输出体积与耗时对比。
    // 8-bit 源走 SDR 矩阵，16-bit 源（PQ/BT.2020）走 HDR 矩阵。
    // 不经过屏幕捕获，也不碰 D3D——纯粹量编解码器本身。
    bool run_encode_bench(Config const& config);
#endif

}  // namespace gamesnap
