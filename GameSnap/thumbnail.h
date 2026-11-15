#pragma once

#include "d3d.h"
#include "ui_bridge.h"

namespace gamesnap {

    // 在 FrameArrived 回调中调用。只回读 320×180；输入必须仍然有效且不得并发写入。
    // 不改变 HDR 的传递函数或亮度，SDR 也不经过色调映射。
    // 上下文调用持锁，GPU 等待与 CPU 拷贝不持锁；失败只影响预览，不影响编码。
    std::shared_ptr<const UiBridge::Thumbnail> make_capture_thumbnail(
        D3DContext& d3d, ID3D11Texture2D* source, std::uint32_t sdr_white_nits, std::string& error);

}  // namespace gamesnap
