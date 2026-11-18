#include <ScriptRuntime/Windows/WindowsGameModule.h>

#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Foundation/Windows/UtfConversion.h>

namespace cue
{
namespace
{
/// @brief Play 子 Process の DLL ロード参照を一意に管理する
class WindowsGameModuleLibrary final : public IGameModuleLibrary
{
  public:
    /// @brief この Owner が取得した DLL の参照を解放する
    ~WindowsGameModuleLibrary() override
    {
        if (m_library)
            FreeLibrary(m_library);
    }

    /// @brief 明示 DLL の親と標準 Directory だけを検索し、CWD から依存 DLL を読み込まない
    [[nodiscard]] Result<void> load(const wchar_t *a_path)
    {
        m_library = LoadLibraryExW(a_path, nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_library)
            return Result<void>::failure({ErrorCategory::PlatformFailure, "GameModule.LoadLibraryExW", GetLastError()});
        return Result<void>::success();
    }

    /// @brief ロード参照の生存中だけ有効な C ABI 入口を取得する
    [[nodiscard]] CueScriptGetModuleFunction entry() const noexcept
    {
        return reinterpret_cast<CueScriptGetModuleFunction>(GetProcAddress(m_library, "CueScriptGetModule"));
    }

    /// @brief 未回収の Instance が持つ関数を無効化しないよう、OS の Process 回収まで参照を残す
    void retain_until_process_exit() noexcept override
    {
        m_library = nullptr;
    }

  private:
    HMODULE m_library = nullptr;
};
} // namespace

Result<std::unique_ptr<RuntimeGameModule>> load_windows_game_module(
    const Path &a_file, std::optional<ScriptConfiguration> a_configuration)
{
    using ModuleResult = Result<std::unique_ptr<RuntimeGameModule>>;
    if (a_configuration && !is_valid_script_configuration(*a_configuration))
        return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.configuration"});
    try
    {
        if (!a_file.is_absolute())
            return ModuleResult::failure({ErrorCategory::InvalidArgument, "GameModule.absolute_path_required"});
        auto wide = utf8_to_utf16(a_file.utf8());
        if (!wide.has_value())
            return ModuleResult::failure(*wide.try_error());
        auto library = std::make_unique<WindowsGameModuleLibrary>();
        auto loaded = library->load(wide.try_value()->c_str());
        if (!loaded.has_value())
            return ModuleResult::failure(*loaded.try_error());
        const auto entry = library->entry();
        return RuntimeGameModule::from_dynamic(entry, std::move(library), a_configuration);
    }
    catch (...)
    {
        return ModuleResult::failure({ErrorCategory::Fatal, "GameModule.load_exception"});
    }
}
} // namespace cue
