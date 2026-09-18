#pragma once

namespace cue::editor
{
/// @brief CueEngine EditorでImGui Dockingを有効化する
/// @pre 呼び出しThreadに有効なImGui Contextが設定されていること
void enable_editor_docking() noexcept;

/// @brief Main Viewport全体を覆う固定Editor DockSpace Hostを開始する
/// @return Host Windowの内容を送信できる場合はtrue、それ以外はfalse
/// @pre 呼び出しThreadに有効なImGui Contextが設定され、ImGui Frameが開始済みであること
/// @note 戻り値に関係なく、同一Threadかつ同一Frameでend_editor_dockspace_hostを必ず1回呼ぶこと
[[nodiscard]] bool begin_editor_dockspace_host() noexcept;

/// @brief Editor DockSpaceを送信して固定Hostを終了する
/// @pre 同一Threadかつ同一Frameでbegin_editor_dockspace_hostが1回呼ばれていること
void end_editor_dockspace_host() noexcept;

/// @brief 次に開始するEditor Panelを初回表示時だけEditor DockSpaceへ配置する
/// @pre 呼び出しThreadに有効なImGui Contextが設定され、ImGui Frameが開始済みであること
void dock_editor_window_on_first_use() noexcept;
} // namespace cue::editor
