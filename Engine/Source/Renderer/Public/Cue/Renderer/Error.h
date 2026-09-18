#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::renderer
{
/// @brief Portable Renderer境界の回復可能な失敗を分類するCode
enum class RendererError : std::int64_t
{
    InvalidCamera = 1,
    UnsupportedMesh = 2,
    InvalidRuntimeBinding = 3,
    SnapshotExtractionFailed = 4
};

/// @brief Renderer Errorを診断Summaryと共に生成する
[[nodiscard]] Error make_renderer_error(const AssertContext &a_assertContext, RendererError a_code,
                                        std::string_view a_summary) noexcept;
} // namespace cue::renderer
