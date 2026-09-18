#pragma once

namespace cue::editor
{
/// @brief CueEngine EditorでImGui Dockingを有効化する
void enable_editor_docking() noexcept;

/// @brief Main Viewport全体を覆う固定Editor DockSpace Hostを開始する
[[nodiscard]] bool begin_editor_dockspace_host() noexcept;

/// @brief Editor DockSpaceを送信して固定Hostを終了する
void end_editor_dockspace_host() noexcept;

/// @brief 次に開始するEditor Panelを初回表示時だけEditor DockSpaceへ配置する
void dock_editor_window_on_first_use() noexcept;
} // namespace cue::editor
