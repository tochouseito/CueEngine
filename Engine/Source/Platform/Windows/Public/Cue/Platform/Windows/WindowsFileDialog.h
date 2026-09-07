#pragma once

#include <Cue/Platform/FileDialog.h>
#include <Cue/Platform/Window.h>

#include <cstdint>
#include <memory>

namespace cue
{
class AssertContext;

/// @brief Windows File Dialog境界の安定した失敗分類
enum class WindowsFileDialogError : std::int64_t
{
    InvalidRequest = 13,
    OwnerUnavailable = 14,
    OwnerThreadViolation = 15,
    ComInitializationFailed = 16,
    DialogCreationFailed = 17,
    DialogConfigurationFailed = 18,
    DialogDisplayFailed = 19,
    SelectionFailed = 20,
    PathNormalizationFailed = 21,
};

/// @brief Windows Windowから短命なNative Dialog Owner Capabilityを発行する
///
/// Tokenはa_windowの寿命を延長しない。a_windowより先に破棄し、WindowのOwner Thread上でだけ使用する。
[[nodiscard]] Result<FileDialogOwnerToken> create_windows_file_dialog_owner(
    Window &a_window, const AssertContext &a_assertContext) noexcept;

/// @brief Project HubとEditorで共有するWindows Native File Dialog Serviceを生成する
///
/// a_assertContextと参照先は返されたServiceより長く生存させる。
[[nodiscard]] std::unique_ptr<FileDialogService> create_windows_file_dialog_service(
    const AssertContext &a_assertContext) noexcept;
} // namespace cue
