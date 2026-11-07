#pragma once

#include <ECS/ECSManager.h>

#include <memory>
#include <vector>

namespace cue::tests
{
/// @brief 単一 Component の初期化とコピーを検証する値型
struct Counter final : ecs::IComponentTag
{
    int value = 0;
    int initializations = 0;

    /// @brief Component の追加が初期化 Hook を通ることを記録する
    void initialize() override
    {
        ++initializations;
    }
};

/// @brief 複数型を要求する System の Query 条件を検証する
struct Step final : ecs::IComponentTag
{
    int amount = 1;
};

/// @brief 複数インスタンスの格納とコピーを検証する
struct Tag final : ecs::IComponentTag
{
    int value = 0;
};

/// @brief 一時 Multi Component View から共有実体を操作する旧 Script と同じ利用を検証する
struct SharedCounter final : ecs::IComponentTag
{
    std::shared_ptr<int> value = std::make_shared<int>(0);
};

/// @brief 別翻訳単位から同じ型の Component ID を取得する
[[nodiscard]] ecs::CompID component_id_from_peer();
/// @brief Staging と Component View の構造変更・寿命を回帰検証する
[[nodiscard]] int run_ecs_regressions();
} // namespace cue::tests

namespace cue::ecs
{
template <> struct IsMultiComponent<tests::Tag> : std::true_type
{
};
template <> struct IsMultiComponent<tests::SharedCounter> : std::true_type
{
};
} // namespace cue::ecs
