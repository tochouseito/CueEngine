#pragma once

#include <span>
#include <string>
#include <string_view>

#include <Foundation/Result.h>

namespace cue
{
/// @brief ABI の構成を明示する。Platform の選択や PC 固有 Path と混同しない
enum class GameBuildConfiguration
{
    Debug,
    Development,
    Release
};

/// @brief 一回のゲーム Build の出力先。Project に固定せず Build 要求ごとに選ぶ所有値
/// 空値は未選択。利用前に create_game_build_target / validate_game_build_target で検証する
struct GameBuildTarget final
{
    std::string platform;
    std::string architecture;
    GameBuildConfiguration configuration = GameBuildConfiguration::Debug;
    /// @brief Build 対象の値を比較する。Toolchain の利用可否までは表さない
    [[nodiscard]] bool operator==(const GameBuildTarget &) const = default;
};

/// @brief 実装済み Platform が登録する成果物契約。Span / 文字列は Process の寿命中だけ借用できる
/// 登録は読み取り専用で Thread-safe。実際の Toolchain の利用可否は Build 起動側で検証する
struct GameBuildPlatform final
{
    std::string_view id;
    std::string_view displayName;
    std::span<const std::string_view> architectures;
    std::string_view moduleSuffix;
    std::string_view staticLibrarySuffix;
    std::string_view applicationSuffix;
};

/// @brief 登録された出力先だけを列挙する。現在利用可能な実装を架空の選択肢で補わない
[[nodiscard]] std::span<const GameBuildPlatform> supported_game_build_platforms() noexcept;
/// @brief Platform ID の完全一致で登録を探す。未選択 / 未対応では nullptr を返す
[[nodiscard]] const GameBuildPlatform *find_game_build_platform(std::string_view a_platform) noexcept;
/// @brief 明示選択を検証して所有する。未知 Platform / Architecture を別の対象へ置き換えない
[[nodiscard]] Result<GameBuildTarget> create_game_build_target(std::string_view a_platform,
                                                               std::string_view a_architecture,
                                                               GameBuildConfiguration a_configuration);
/// @brief 集約初期化された Build 要求も利用前に検証する。失敗時は入力を変更しない
[[nodiscard]] Result<void> validate_game_build_target(const GameBuildTarget &a_target);
/// @brief Editor / Play を実行する Native Binary 自身の対象を返す。製品用の選択から独立する
/// Native の実装が未登録なら失敗する。現在の Build 構成を含む所有値を返す
[[nodiscard]] Result<GameBuildTarget> current_native_game_build_target();
/// @brief CMake / Manifest と共通の構成名を返す。未知の列挙値では空を返す
[[nodiscard]] std::string_view game_build_configuration_name(GameBuildConfiguration a_configuration) noexcept;
/// @brief この Library を生成した構成を返し、異なる CRT / ABI の Game DLL を選ばない
[[nodiscard]] GameBuildConfiguration current_game_build_configuration() noexcept;
} // namespace cue
