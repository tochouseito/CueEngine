#pragma once

#include <Cue/Project/Descriptor.h>

#include <string_view>

namespace cue
{
class AssertContext;
class FilesystemRoot;

/// @brief Blank Project が要求する Engine 互換範囲だけを保持する最小 Template
struct BlankProjectTemplate final
{
    EngineCompatibility engineCompatibility;
};

/// @brief Project を Operation 所有 Staging で完成させ、既存 Directory を上書きせず Atomic に公開する
///
/// Project 名は Portable な単一 Directory Segment だけを受理する
/// 成功時は検証済み Descriptor を返し、Publish 前失敗時は最終 Directory を作らず Staging を Rollback する
/// Publish 後の Durability 失敗では ProjectError::IoFailure を返すが最終 Directory は公開済みとなる
/// 呼び出し側は Error::root_code() の Cue.IO／IoError::DurabilityUnknown で判別し、再 Open して検証する
/// この状態で同名生成の再試行または完成 Directory の自動削除を行わない
[[nodiscard]] Result<ProjectDescriptor> generate_blank_project(
    FilesystemRoot &a_parentFilesystem, std::string_view a_projectName, std::string_view a_displayName,
    const ProjectId &a_projectId, BlankProjectTemplate a_template, const AssertContext &a_assertContext) noexcept;

/// @brief 既存 Project に不足する Game Source と CMake Workspace File だけを追加する
///
/// CueProject.json が期待 Descriptor と一致する場合だけ処理する。既存 File が Generator Template と一致すれば保持し、
/// 内容が異なる場合は一切上書きせず失敗する。初回 Publish 後は User 所有となるため、途中失敗でも作成済み File を
/// 自動削除しない。再実行は一致済み File を保持して不足分だけを作成する。
[[nodiscard]] Result<void> ensure_project_game_workspace(FilesystemRoot &a_projectFilesystem,
                                                         const ProjectDescriptor &a_expectedDescriptor,
                                                         const AssertContext &a_assertContext) noexcept;
} // namespace cue
