#include <Cue/Platform/Windows/WindowsFileDialog.h>

namespace
{
/// @brief Windows File Dialog公開境界をWindows Headerなしで参照できることを検証する
void verify_windows_file_dialog_header_compiles()
{
    static_cast<void>(sizeof(cue::WindowsFileDialogError));
}
} // namespace
