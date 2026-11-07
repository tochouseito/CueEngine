#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <Foundation/Path.h>

namespace cue
{
class IFileSystem;

/// @brief 移植可能な Game Target 名を検証し、CMake の予約名と Windows の Device 名を拒否する
/// FileSystem に触れず、Thread に依存しない。Target 名を Path や実行 Command として解釈しない
[[nodiscard]] bool is_valid_game_target(std::string_view a_name) noexcept;

/// @brief .cueproj v1 / v2 の永続設定を所有し、一時的な Runtime Handle や PC 固有 Path を含めない
struct ProjectDescriptor final
{
    std::uint32_t formatVersion = 1;
    std::string projectId;
    std::string name;
    std::string applicationName;
    Path sourceRoot;
    Path contentRoot;
    // 指定時は Content Root 内の相対 .cuescene File を開く
    std::optional<Path> startupScene;
    // 指定時は Source Root の CMakeLists.txt が定義する Game Target を使う。成果物 Path は保存しない
    std::optional<std::string> gameTarget;
    // v2 の指定 Engine。PC 固有の配置は Hub の登録情報で解決する
    std::optional<std::string> engineVersion;
};

/// @brief 検証済み Project 設定と、その File を基準に解決した配置を値で所有する
///
/// Editor が所有し、Runtime World や GPU Resource は所有しない。公開後の変更操作は提供しない
/// 借用 Getter は Project の破棄・Move・代入まで有効。同じ値の読み取りは複数 Thread から可能
class Project final
{
  public:
    /// @brief 引数 Path を一度だけ絶対化し、最大 64 KiB の設定と Source / Content Directory を検証する
    ///
    /// FileSystem は呼出中だけ借用し、戻り値は依存しない。作業 Directory を並行変更しない
    /// 失敗時は部分 Project を返さず FileSystem を変更しない。Allocation 失敗も Result で返す
    [[nodiscard]] static Result<Project> load(IFileSystem &a_files, const Path &a_file);

    /// @brief Project が所有する永続設定を借用する
    [[nodiscard]] const ProjectDescriptor &descriptor() const noexcept;
    /// @brief 読み込み時に固定した設定 File の絶対 Path を借用する
    [[nodiscard]] const Path &file() const noexcept;
    /// @brief 設定 File の親である Project Root を借用する
    [[nodiscard]] const Path &root() const noexcept;
    /// @brief Project Root を基準に解決した Source Directory を借用する
    [[nodiscard]] const Path &source_directory() const noexcept;
    /// @brief Project Root を基準に解決した Content Directory を借用する
    [[nodiscard]] const Path &content_directory() const noexcept;
    /// @brief Content 基準で解決した起動 Scene の絶対 Path を借用する。未指定なら nullptr
    [[nodiscard]] const Path *startup_scene_file() const noexcept;
    /// @brief Game Target 指定時の Source/CMakeLists.txt を絶対 Path で借用する。未指定なら nullptr
    /// 存在・種類は読込時点の検証であり、後の Build 成功を保証しない
    [[nodiscard]] const Path *game_cmake_file() const noexcept;

  private:
    /// @brief 全検証が成功した所有値だけで Project を構築する
    Project(ProjectDescriptor a_descriptor, Path a_file, Path a_source, Path a_content,
            std::optional<Path> a_startupScene, std::optional<Path> a_gameCmake);

    ProjectDescriptor m_descriptor;
    Path m_file;
    Path m_root;
    Path m_source;
    Path m_content;
    std::optional<Path> m_startupScene;
    std::optional<Path> m_gameCmake;
};
} // namespace cue
