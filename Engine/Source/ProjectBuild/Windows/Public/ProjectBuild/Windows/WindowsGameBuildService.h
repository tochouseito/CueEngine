#pragma once

#include <memory>

#include <Foundation/Path.h>
#include <Foundation/Result.h>
#include <ProjectBuild/GameBuildService.h>

namespace cue
{
/// @brief Authoring Tool の配置。指定する Path は絶対値とし、起動 Shell の環境や CWD を前提にしない
/// 配布版 Engine は同梱した固定依存を使うため、toolchainFile / vcpkgRoot を空にできる
struct GameBuildTools final
{
    Path engineRoot;
    Path cmakeExecutable;
    Path toolchainFile;
    Path vcpkgRoot;
};

/// @brief Windows Process / Job による非同期 Build を生成する。Native 型は公開しない
/// 呼出 Thread を Owner とする。Tool の存在と絶対 Path を検証し、生成だけでは Build しない
[[nodiscard]] Result<std::unique_ptr<IGameBuildService>> create_windows_game_build_service(GameBuildTools a_tools);
} // namespace cue
