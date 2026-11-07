#pragma once

#include <string_view>

#include <Script/Abi.h>

namespace cue
{
/// @brief 製品 Target を生成した Platform Provider が渡す、実行先の不変 Identity
struct WindowsGameBuildIdentity final
{
    std::string_view platform;
    std::string_view architecture;
    std::string_view configuration;
};

/// @brief Windows の起動・配布設定・静的ゲーム登録を結び、通常終了まで所有する
/// Entry は同じ exe に静的 Link される。Editor / IPC / ゲーム DLL を起動・読込しない
[[nodiscard]] int run_windows_game(CueScriptGetModuleFunction a_entry,
                                   const WindowsGameBuildIdentity &a_identity) noexcept;
} // namespace cue
