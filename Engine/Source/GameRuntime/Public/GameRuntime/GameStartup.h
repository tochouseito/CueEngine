#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <Foundation/Path.h>

namespace cue
{
class IFileSystem;

/// @brief 配布アプリの起動設定。Project の Source Path や Editor の状態を含めない
/// Platform / Architecture は Build 時に選んだ識別子。起動 Host が自分の Identity と照合する
struct GameStartupConfig final
{
    std::string projectId;
    std::string gameTarget;
    std::string applicationName;
    std::string platform;
    std::string architecture;
    std::string configuration;
    Path startupScene;
    std::string windowTitle;
    std::uint32_t windowWidth = 1280;
    std::uint32_t windowHeight = 720;
    std::uint32_t targetFramesPerSecond = 60;
};

/// @brief 上限 64 KiB の所有設定を解釈する。未知・重複・欠損・不正値を拒否する
/// Platform の対応状況を推測せず識別子を保持する。File / Window / World は生成しない
[[nodiscard]] Result<GameStartupConfig> parse_game_startup(std::string_view a_text);
/// @brief 実行 File の Directory から完成 Package と設定を検証する。CWD や Repository を使わない
/// GameStartup.cueapp と GamePackage.cuepackage の識別が一致し、Content 内の Scene が通常 File であることを確認する
/// FileSystem は呼出中だけ借用し、戻り値は独立した所有値。起動後の外部書換えは保証しない
[[nodiscard]] Result<GameStartupConfig> load_game_startup(IFileSystem &a_files, const Path &a_executableDirectory);
} // namespace cue
