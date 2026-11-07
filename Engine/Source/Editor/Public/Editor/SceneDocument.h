#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Scene/Scene.h>

namespace cue
{
class EditorApplication;

/// @brief 編集中 Scene・永続 Object 選択・保存基準・操作履歴の正本を所有する
/// EditorApplication だけが変更する。全参照は Owner Thread の次の編集・読込・破棄まで有効
/// Runtime Entity / World / GPU Resource は所有しない
class SceneDocument final
{
    class ConstructionKey final
    {
        friend class EditorApplication;
        /// @brief 検証と Preview の準備を迂回した構築を禁止する
        ConstructionKey() = default;

      public:
        /// @brief make_unique の Constructor 引数へ Key を移す
        ConstructionKey(ConstructionKey &&) = default;
    };

  public:
    /// @brief EditorApplication の検証済み候補からだけ構築する
    SceneDocument(ConstructionKey, Scene a_scene, std::string a_text, std::optional<Path> a_file,
                  bool a_hasSavedBaseline);
    static constexpr std::size_t k_historyLimit = 128;
    /// @brief Scene の不変な正本を借用する。Transform 編集中は現在の有効な Draft を返す
    [[nodiscard]] const Scene &scene() const noexcept;
    /// @brief 保存先を借用する。新規 Document は nullptr
    [[nodiscard]] const Path *file() const noexcept;
    /// @brief 選択中の永続 ID を返す。空は未選択
    [[nodiscard]] std::string_view selected_object_id() const noexcept;
    /// @brief 最後に File へ公開した保存内容と異なるか返す。電源断への耐久性とは分離する
    [[nodiscard]] bool is_dirty() const noexcept;
    /// @brief Draft 編集中を除き Undo できるか返す
    [[nodiscard]] bool can_undo() const noexcept;
    /// @brief Draft 編集中を除き Redo できるか返す
    [[nodiscard]] bool can_redo() const noexcept;
    /// @brief 一回の Transform 操作をまとめているか返す
    [[nodiscard]] bool is_transform_editing() const noexcept;
    /// @brief 成功した Data 変更の単調 Revision を返す。選択や保存だけでは進めない
    [[nodiscard]] std::uint64_t revision() const noexcept;

  private:
    friend class EditorApplication;
    enum class CommandKind
    {
        AddCube,
        Transform,
        Scripts
    };
    struct Command final
    {
        CommandKind kind = CommandKind::Transform;
        SceneObjectDescriptor object;
        math::floatTransform before;
        math::floatTransform after;
        std::vector<ScriptDescriptor> scriptsBefore;
        std::vector<ScriptDescriptor> scriptsAfter;
        std::uint32_t formatBefore = 1;
        std::uint32_t formatAfter = 1;
        std::string selectedBefore;
        std::string selectedAfter;
    };
    struct TransformEdit final
    {
        std::string objectId;
        math::floatTransform before;
    };
    /// @brief 事前に容量を確保した履歴へ一回の操作を記録する
    void append_command(Command a_command) noexcept;

    Scene m_scene;
    std::string m_text;
    std::optional<Path> m_file;
    std::string m_selected;
    std::string m_savedText;
    bool m_hasSavedBaseline = false;
    std::uint64_t m_revision = 0;
    std::vector<Command> m_history;
    std::size_t m_cursor = 0;
    std::optional<TransformEdit> m_edit;
};
} // namespace cue
