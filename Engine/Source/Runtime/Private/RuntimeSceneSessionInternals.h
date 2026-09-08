#pragma once

#include <Cue/Foundation/Result.h>

namespace cue
{
class AssertContext;
}

namespace cue::game_core
{
class RuntimeWorld;
}

namespace cue::scene
{
class SceneInstance;
class SceneInstanceEndReport;
} // namespace cue::scene

namespace cue::runtime::details
{
/// @brief ProductionとTestが同じScene終了正規化経路を通るための非公開Operation
class SceneEndOperation
{
  public:
    /// @brief 派生Operationを非所有基底参照から安全に扱う
    virtual ~SceneEndOperation() noexcept = default;

    /// @brief SceneInstance終了のRaw Resultを一度だけ生成する
    [[nodiscard]] virtual Result<scene::SceneInstanceEndReport> end(
        scene::SceneInstance &a_instance, game_core::RuntimeWorld &a_runtimeWorld,
        const AssertContext &a_assertContext) const noexcept = 0;

  protected:
    /// @brief Operation実装だけが基底部分を構築できるようにする
    SceneEndOperation() noexcept = default;
    /// @brief 注入Operationの所有権複製を禁止する
    SceneEndOperation(const SceneEndOperation &) = delete;
    /// @brief 注入Operationの所有権複製代入を禁止する
    SceneEndOperation &operator=(const SceneEndOperation &) = delete;
    /// @brief Operation Addressを固定するためMove構築を禁止する
    SceneEndOperation(SceneEndOperation &&) = delete;
    /// @brief Operation Addressを固定するためMove代入を禁止する
    SceneEndOperation &operator=(SceneEndOperation &&) = delete;
};
} // namespace cue::runtime::details
