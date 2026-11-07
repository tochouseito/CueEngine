#pragma once

#include <string>
#include <string_view>

#include <Foundation/Path.h>
#include <ProjectBuild/BuildTarget.h>

namespace cue
{
class IFileSystem;
class Project;

/// @brief 検証時点で完成したゲーム成果物の所有 Path。DLL 自体は読み込まず、後の変更は保証しない
struct GameBuildArtifacts final
{
    Path directory;
    Path manifestFile;
    Path moduleFile;
    Path staticLibraryFile;
    std::string gameTarget;
    GameBuildConfiguration configuration = GameBuildConfiguration::Debug;
    GameBuildTarget buildTarget;
};

/// @brief 明示選択した対象から Project の Build/<Platform>/<Architecture>/<Configuration> を解決する
/// File の作成は行わない。未指定 / 未対応と gameTarget のない Project を拒否し、別対象へ戻らない
[[nodiscard]] Result<Path> game_build_directory(const Project &a_project, const GameBuildTarget &a_target);
/// @brief Native の Editor / Play 用の互換入口。製品 Build は明示対象の Overload を使う
[[nodiscard]] Result<Path> game_build_directory(const Project &a_project, GameBuildConfiguration a_configuration);
/// @brief Manifest の Project / Target / 構成 / ABI と SHA-256 を検証し、完成済み成果物だけを返す
/// FileSystem を呼出中だけ借りる。同期の読込は起動 / Reload / Play 前に行い、毎 Frame は呼ばない
/// 存在確認後の変更を防ぐ Lock は持たない。実行時も子 Process 内で DLL の ABI を再検証する
[[nodiscard]] Result<GameBuildArtifacts> load_game_build_artifacts(IFileSystem &a_files, const Project &a_project,
                                                                   const GameBuildTarget &a_target);
/// @brief Native の Editor / Play 用の互換入口。成果物の現在構成も呼出側が明示する
[[nodiscard]] Result<GameBuildArtifacts> load_game_build_artifacts(IFileSystem &a_files, const Project &a_project,
                                                                   GameBuildConfiguration a_configuration);
} // namespace cue
