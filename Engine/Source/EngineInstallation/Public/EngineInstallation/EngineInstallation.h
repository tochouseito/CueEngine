#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <Foundation/Path.h>

namespace cue
{
class IFile;
class IFileSystem;

/// @brief 実行元 Engine の配置と配布識別を値で所有する
/// 配布版だけが useLeaseId を持ち、開発 checkout を Hub の更新対象と混同しない
struct EngineInstallation final
{
    Path root;
    Path editorExecutable;
    Path projectToolExecutable;
    std::string version;
    std::string buildId;
    std::string useLeaseId;
    bool isPackaged = false;
};

/// @brief Project と配布 Descriptor に保存できる最大 128 Byte の SemVer を検証する
/// 入力は呼出中だけ借用し、Allocation と共有状態を使わず任意 Thread で呼べる
[[nodiscard]] bool is_valid_engine_version(std::string_view a_version) noexcept;

/// @brief 指定 Root の配布 Descriptor と相対配置を検証し、所有値を返す
/// FileSystem は呼出中だけ借用する。未知形式、欠損、Link、Root 外参照では部分値を公開しない
[[nodiscard]] Result<EngineInstallation> load_engine_installation(IFileSystem &a_files, const Path &a_root);

/// @brief 実行 File の祖先から配布 Root を探索し、配布版がなければ指定 checkout を使う
/// 配布用 Bin/Editor・Bin/Tools 配置で Descriptor が欠ける場合は開発 Path へ戻さず失敗する
[[nodiscard]] Result<EngineInstallation> resolve_engine_installation(IFileSystem &a_files,
                                                                     const Path &a_developmentRoot);

/// @brief 配布 Engine の共有利用 Lease をユーザー保存領域で取得する
/// 呼出側は Engine に触れる子孫の終了まで File を所有する。開発版は空の成功値を返す
/// FileSystem は呼出中だけ借用する。排他中、Link、入出力失敗では処理を開始しない
[[nodiscard]] Result<std::unique_ptr<IFile>> acquire_engine_use_lease(IFileSystem &a_files,
                                                                      const EngineInstallation &a_installation);

/// @brief 配布版で使用する CMake を明示設定・同梱配置・標準導入先の順で解決する
/// CUE_CMAKE_EXECUTABLE の絶対 Path を利用者指定として扱い、Shell の PATH や CWD を検索しない
[[nodiscard]] Result<Path> locate_engine_cmake(IFileSystem &a_files, const EngineInstallation &a_installation);
} // namespace cue
