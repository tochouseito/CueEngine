#include <Windows.h>

#include <Cue/Editor/ImGui/EditorPresenter.h>
#include <Cue/Editor/ImGui/FilesPresenter.h>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<cue::editor::EditorPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::EditorPresenter>);
static_assert(!std::is_copy_constructible_v<cue::editor::FilesPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::FilesPresenter>);

/// @brief Editor ImGui公開Headerが単独利用できることを検証する
int main()
{
    return 0;
}
