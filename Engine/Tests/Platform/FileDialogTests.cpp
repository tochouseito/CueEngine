#include <Cue/Platform/FileDialog.h>

namespace
{
/// @brief SelectedとCancelledが相互排他的な所有結果として解釈できることを検証する
[[nodiscard]] bool test_dialog_result_semantics()
{
    cue::FileDialogResult selected = cue::FileDialogResult::selected("C:\\Project\\Assets\\Scene.cue");
    if (selected.outcome() != cue::FileDialogOutcome::Selected || !selected.selected_path().has_value() ||
        *selected.selected_path() != "C:\\Project\\Assets\\Scene.cue")
    {
        return false;
    }

    cue::FileDialogResult cancelled = cue::FileDialogResult::cancelled();
    return cancelled.outcome() == cue::FileDialogOutcome::Cancelled && !cancelled.selected_path().has_value();
}
} // namespace

/// @brief File Dialog ResultのPortable状態契約を検証する
int main()
{
    return test_dialog_result_semantics() ? 0 : 1;
}
