#pragma once

#include <Cue/Renderer/RHI/Backend.h>

#include <memory>

namespace cue
{
/// @brief 現在の Platform で使用する RHI Backend を生成する
/// @details Native Window は生成した Backend より長く生存させる。具体実装は Factory 側へ閉じ込める
[[nodiscard]] Result<std::unique_ptr<IBackend>> create_backend(void* a_nativeWindow,
                                                               WindowSize a_clientSize);
} // namespace cue
