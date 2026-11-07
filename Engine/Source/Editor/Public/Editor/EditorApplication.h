#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Editor/SceneDocument.h>
#include <Platform/FileSystem.h>
#include <Runtime/WorldHandle.h>

namespace cue
{
class World;

/// @brief Host が与える配置と ID 発行サービス。Native API は公開しない
struct EditorApplicationConfig final
{
    std::optional<Path> sceneFile;
    std::optional<Path> contentRoot;
    std::function<Result<std::string>()> generateId;
    // 未指定は World::create。Test は候補 World の生成失敗をここへ注入できる
    std::function<Result<std::unique_ptr<World>>()> createWorld;
};

/// @brief 編集操作を検証し、SceneDocument と派生 Preview World を Main の確定境界で更新する
/// 全操作・破棄は生成 Thread で直列化する。借用先は次の構造変更・読込・停止で失効する
/// UI はこの操作 API を呼び、World / FileSystem を直接変更しない
class EditorApplication final
{
    class CreateToken final
    {
        friend class EditorApplication;
        /// @brief Factory 以外から空の Application を生成させない
        CreateToken() = default;

      public:
        /// @brief make_unique の Constructor 引数へ Key を移す
        CreateToken(CreateToken &&) = default;
    };

  public:
    /// @brief Factory だけが空の所有状態を生成する
    explicit EditorApplication(CreateToken) noexcept;
    /// @brief Scene と Preview を用意する。未指定 Scene は Camera を持つ新規 Scene とする
    /// 候補の生成・実体化失敗では部分 Document を公開しない
    [[nodiscard]] static Result<std::unique_ptr<EditorApplication>> create(std::optional<Scene> a_initialScene,
                                                                           EditorApplicationConfig a_config = {});
    /// @brief Owner Thread で Preview を停止してから Document を解放する
    ~EditorApplication();
    /// @brief World 所有を複製しない
    EditorApplication(const EditorApplication &) = delete;
    /// @brief World 所有を複製しない
    EditorApplication &operator=(const EditorApplication &) = delete;
    /// @brief Owner Thread から不変の編集正本を借用する。停止前だけ利用する
    [[nodiscard]] const SceneDocument &document() const noexcept;
    /// @brief Owner Thread だけに Preview を借用する。外部の構造変更・System 追加は禁止
    [[nodiscard]] World *preview_world() noexcept;
    /// @brief 現在の Preview Instance を Owner Thread へ返す
    [[nodiscard]] std::optional<SceneInstanceHandle> preview_instance() const noexcept;
    /// @brief Camera を持つ未保存 Scene へ切り替える。未保存確認は Host の責務
    [[nodiscard]] Result<void> new_scene(std::string_view a_name = "Untitled");
    /// @brief Content 内の Scene と候補 World を準備してから切り替える。未保存確認は Host が行う
    [[nodiscard]] Result<void> open_scene(IFileSystem &a_files, const Path &a_file);
    /// @brief 原子的な公開経路で保存する。公開前の失敗は保存先・保存基準を維持する
    /// 公開成功で保存基準を更新する。電源断への Durability 未確認は成功値で別途伝える
    [[nodiscard]] Result<FileSaveOutcome> save_scene(IFileSystem &a_files, const Path &a_file);
    /// @brief UUID を発行して Cube を追加・選択し、作成した ID を返す
    [[nodiscard]] Result<std::string> add_cube();
    /// @brief 永続 Object ID で選択する。空文字列は選択解除
    [[nodiscard]] Result<void> select_object(std::string_view a_objectId);
    /// @brief 一回の Transform 操作として確定する。失敗時は Data と履歴を維持する
    [[nodiscard]] Result<void> set_transform(std::string_view a_objectId, const math::floatTransform &a_transform);
    /// @brief Script の割当・設定を一回の Undo 操作で置き換える。未解決型も保存し、Preview では実行しない
    /// 候補の検証・Preview 生成失敗では正本・履歴を維持する。Transform Draft 中は拒否する
    [[nodiscard]] Result<void> set_scripts(std::string_view a_objectId, std::vector<ScriptDescriptor> a_scripts);
    /// @brief Transform 編集の開始値を保持する。二重開始は拒否する
    [[nodiscard]] Result<void> begin_transform_edit(std::string_view a_objectId);
    /// @brief Draft と既存 Entity を更新する。毎回の World 再生成は行わない
    [[nodiscard]] Result<void> preview_transform(const math::floatTransform &a_transform);
    /// @brief 最終値だけを一回の Undo 操作として記録する
    [[nodiscard]] Result<void> commit_transform_edit();
    /// @brief 開始前の Transform へ復元し、履歴を増やさず編集を終える
    [[nodiscard]] Result<void> cancel_transform_edit();
    /// @brief 一回の確定操作を戻す。失敗時は履歴位置を進めない
    [[nodiscard]] Result<void> undo();
    /// @brief 戻した操作を同じ永続 ID で再適用する
    [[nodiscard]] Result<void> redo();
    /// @brief Snapshot 抽出前に Preview の Transform Cache を同期する
    [[nodiscard]] Result<void> synchronize_preview();
    /// @brief Preview を停止する。停止後の変更操作を拒否し、繰返し呼べる
    [[nodiscard]] Result<void> shutdown();

  private:
    struct PreparedPreview;
    /// @brief Owner / 停止 / 再入と Revision の前提を検証する
    [[nodiscard]] Result<void> check_access() const;
    /// @brief 指定 Scene の候補 World を部分公開せずに作る
    [[nodiscard]] Result<PreparedPreview> prepare_preview(const Scene &a_scene);
    /// @brief 完成済み候補を公開し、旧 World を停止・回収する
    void replace_preview(PreparedPreview a_preview) noexcept;
    /// @brief 検証済み Scene の一つの Transform を既存 Preview へ適用する
    [[nodiscard]] Result<void> apply_preview_transform(std::string_view a_id, const math::floatTransform &a_value);
    /// @brief 生成サービスから Scene / Object に使える UUID を取得する
    [[nodiscard]] Result<std::string> generate_id();
    /// @brief 新規 Scene を検証済みの所有値として用意する
    [[nodiscard]] Result<Scene> make_new_scene(std::string_view a_name);
    /// @brief Path を絶対化し、Content 境界・Link・拡張子を検証する
    [[nodiscard]] Result<Path> validate_file(IFileSystem &a_files, const Path &a_file, bool a_isSaving);
    /// @brief Transform の候補を準備し、Preview と Data を確定する
    [[nodiscard]] Result<void> change_transform(std::string_view a_id, const math::floatTransform &a_value);
    /// @brief Undo / Redo の方向で操作候補を構築して確定する
    [[nodiscard]] Result<void> apply_history(bool a_isRedo);

    std::thread::id m_owner;
    EditorApplicationConfig m_config;
    std::unique_ptr<SceneDocument> m_document;
    std::unique_ptr<World> m_world;
    std::optional<SceneInstanceHandle> m_instance;
    bool m_isStopped = false;
    bool m_isMutating = false;
};
} // namespace cue
