#pragma once

#include <Cue/Build/Service.h>

#include <cstdint>
#include <memory>
#include <string>

namespace cue
{
class AssertContext;
class ProjectDescriptor;

/// @brief Windows Game Module Artifact公開の安定した失敗分類
enum class WindowsBuildArtifactError : std::int64_t
{
    InvalidSettings = 1,
    WorkspaceLockFailed,
    CandidateInvalid,
    ModuleContractMismatch,
    ArtifactLockFailed,
    ArtifactAlreadyExists,
    CurrentManifestFailed,
    CurrentManifestDurabilityUnknown
};

/// @brief Project契約を所有するWindows Build Artifact Publisherを構築する
///
/// Project RootとDescriptorは呼出中だけ借用し、必要なProject IDとCompatibilityを返却PublisherへCopyする。
/// Publisherは一つのGameBuildService Workerから直列使用し、Build／Artifact LockをProcess間で共有する。
/// 回復可能なPath、Lock、Metadata初期化失敗はErrorを返し、Allocation等の回復不能例外はFatalHandlerへ渡す。
[[nodiscard]] Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept;
} // namespace cue
