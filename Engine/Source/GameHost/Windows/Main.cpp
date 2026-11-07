#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GameHost/Windows/WindowsGame.h>

extern "C" CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(std::uint32_t, CueScriptModuleApi *) noexcept;

/// @brief 選択 Platform の Host へ、同じ exe に組み込んだ Game Module の登録入口を渡す
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return cue::run_windows_game(CueScriptGetModule,
                                 {CUE_GAME_PLATFORM, CUE_GAME_ARCHITECTURE, CUE_GAME_CONFIGURATION});
}
