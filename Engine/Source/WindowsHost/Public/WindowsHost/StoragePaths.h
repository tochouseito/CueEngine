#pragma once

#include <string>

#include <Platform/FileSystem.h>

namespace cue
{
enum class StorageMode
{
    Development,
    Product,
    Portable
};

/// @brief Build 構成と独立して保存先を選ぶ Host の所有設定
struct StoragePathsConfig final
{
    StorageMode mode = StorageMode::Product;
    std::string companyName = "CueEngine";
    std::string applicationName = "CueEngine";
    Path repositoryRoot;
    // 指定する場合は絶対 Path。未指定の用途は各 Mode の既定を使う
    Path dataRootOverride;
    Path logsOverride;
    Path shaderCacheOverride;
    Path psoCacheOverride;
};

/// @brief 各利用者へ渡す絶対保存先を値で所有する
struct StoragePaths final
{
    Path dataRoot;
    Path logs;
    Path shaderCache;
    Path psoCache;
};

/// @brief Host の実行 Mode と設定から保存先を解決する。Directory はまだ作成しない
///
/// FileSystem / Config は呼出中だけ借用し、戻り値は独立して所有する。再入しない
/// Company / Application 名の検証、取得失敗、相対 Override は Error にする。CWD に Fallback しない
[[nodiscard]] Result<StoragePaths> resolve_storage_paths(IFileSystem &a_files, const StoragePathsConfig &a_config);
/// @brief 解決済み保存先を作成する。途中失敗では作成済み Directory が残り得る
[[nodiscard]] Result<void> create_storage_directories(IFileSystem &a_files, const StoragePaths &a_paths);
} // namespace cue
