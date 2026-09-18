#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Renderer/RenderSnapshot.h>
#include <Cue/Renderer/RendererSchema.h>

#include <cstdint>

namespace cue
{
class AssertContext;
}

namespace cue::scene
{
class SceneDocument;
}

namespace cue::renderer
{
/// @brief Authoring SceneからPointer非保持のRender Snapshotを生成する
[[nodiscard]] Result<RenderSnapshot> extract_render_snapshot(const scene::SceneDocument &a_document,
                                                             const RendererSchemaTypeIds &a_typeIds,
                                                             std::uint64_t a_generation,
                                                             const AssertContext &a_assertContext) noexcept;
} // namespace cue::renderer
