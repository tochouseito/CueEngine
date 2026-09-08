#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/UpdateContext.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cue
{
class FrameInputSnapshot;
}

namespace cue::game_core
{
class StructuralCommandBuffer;
class World;

/// @brief Portable Runtime System更新の大分類
enum class RuntimeUpdatePhase : std::uint8_t
{
    PreUpdate,
    Update,
    PostUpdate
};

/// @brief Registryが所有するSystem識別子、Phase、明示Order、必須先を保持する値
struct RuntimeSystemDescriptor final
{
    std::string id;
    RuntimeUpdatePhase phase = RuntimeUpdatePhase::Update;
    std::int32_t order = 0;
    std::vector<std::string> dependencies;
};

/// @brief Start／Stop Callback中だけ有効なWorld Scope非所有参照
struct RuntimeSystemContext final
{
    World &world;
    StructuralCommandBuffer &commands;
};

/// @brief Update Callback中だけ有効なWorld、時間、入力の非所有参照
struct RuntimeSystemUpdateContext final
{
    World &world;
    StructuralCommandBuffer &commands;
    const UpdateContext &timing;
    const FrameInputSnapshot &input;
};

/// @brief 単一Owner Threadで明示的に開始、更新、停止されるGame System境界
///
/// startは失敗時にCleanupを要する副作用を残さず、stopは未完了Substepだけを再試行可能にする
class RuntimeSystem
{
  public:
    /// @brief 派生SystemをRegistry所有Pointerから安全に破棄する
    virtual ~RuntimeSystem() noexcept = default;

    /// @brief World Scopeへの接続を完全に準備し、成功時だけStarted状態をCommitする
    [[nodiscard]] virtual Result<void> start(RuntimeSystemContext &a_context) noexcept = 0;
    /// @brief 一Frame不変の時間と入力を使用してSystem状態を更新する
    [[nodiscard]] virtual Result<void> update(const RuntimeSystemUpdateContext &a_context) noexcept = 0;
    /// @brief 完了済みSubstepを二重実行せずWorld Scopeから再試行可能に切り離す
    [[nodiscard]] virtual Result<void> stop(RuntimeSystemContext &a_context) noexcept = 0;

  protected:
    /// @brief 派生Systemだけが基底部分を構築できるようにする
    RuntimeSystem() noexcept = default;
    /// @brief Registryによる一意所有を保つため複製を禁止する
    RuntimeSystem(const RuntimeSystem &) = delete;
    /// @brief Registryによる一意所有を保つため複製代入を禁止する
    RuntimeSystem &operator=(const RuntimeSystem &) = delete;
    /// @brief System Addressを固定するためMove構築を禁止する
    RuntimeSystem(RuntimeSystem &&) = delete;
    /// @brief System Addressを固定するためMove代入を禁止する
    RuntimeSystem &operator=(RuntimeSystem &&) = delete;
};
} // namespace cue::game_core
