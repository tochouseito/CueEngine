#pragma once

#include <cstdint>

namespace cue
{
/// @brief GPU 用描画枠の容量と未完了 Frame の上限を独立して指定する
struct GpuFrameConfig final
{
    // 同時に GPU が参照できる物理資源の循環枠数
    std::uint32_t resourceFrameCount = 2;
    // CPU が GPU に提出してよい未完了 Frame 数の上限
    std::uint32_t maxFramesInFlight = 2;

    /// @brief GPU 設定を値で比較する
    bool operator==(const GpuFrameConfig &) const = default;
};
} // namespace cue
