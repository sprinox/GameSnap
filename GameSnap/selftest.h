#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 自检模式（--selftest）：不经过 Windows.Graphics.Capture，用合成纹理走一遍
// “staging 纹理 → Map → 像素转换 → 配置格式写盘”的完整链路。
//
// 用途：截图没有生成时，快速区分是【屏幕捕获权限/资源】问题还是【编码/写盘】问题。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"

// 自检只在 Debug 构建里编进去：Release 去掉那一整条合成纹理生成 + 独立编码链路能省二进制。
// 需要时可以用 /DGAMESNAP_ENABLE_SELFTEST=1 在 Release 里强制打开。
#if defined(_DEBUG) || defined(GAMESNAP_ENABLE_SELFTEST)
#define GAMESNAP_SELFTEST 1
#else
#define GAMESNAP_SELFTEST 0
#endif

namespace gamesnap {

#if GAMESNAP_SELFTEST
    bool run_selftest(Config const& config);
#endif

}  // namespace gamesnap
