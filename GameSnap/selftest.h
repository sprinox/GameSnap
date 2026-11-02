#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 自检模式（--selftest）：不经过 Windows.Graphics.Capture，用合成纹理走一遍
// “staging 纹理 → Map → 像素转换 → BMP 写盘”的完整链路。
//
// 用途：截图没有生成时，快速区分是【屏幕捕获权限/资源】问题还是【编码/写盘】问题。
// ─────────────────────────────────────────────────────────────────────────────

#include "common.h"

namespace gamesnap {

    bool run_selftest(Config const& config);

}  // namespace gamesnap
