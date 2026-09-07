#include <Cue/Platform/FileDialog.h>

namespace
{
/// @brief File Dialog公開型をPlatform非依存Headerだけで参照できることを検証する
void verify_file_dialog_header_compiles()
{
    static_cast<void>(sizeof(cue::FileDialogFilter));
    static_cast<void>(sizeof(cue::FileDialogResult));
}
} // namespace
