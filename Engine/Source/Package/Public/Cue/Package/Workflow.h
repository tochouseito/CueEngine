#pragma once

#include <Cue/Build/Service.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/Package/Publisher.h>
#include <Cue/Package/RuntimeData.h>
#include <Cue/Platform/Process.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::package
{
/// @brief Editorが観測するBuild、Package、Run全体の進行状態
enum class PackageWorkflowState : std::uint8_t
{
    Idle,
    Building,
    Packaging,
    PackageReady,
    Running,
    RunSucceeded,
    Failed,
    Cancelled
};

/// @brief Workflowが現在実行している意味Stage
enum class PackageWorkflowStage : std::uint8_t
{
    None,
    Build,
    Package,
    Run
};

/// @brief Standalone Runtime起動方法
enum class PackageRunMode : std::uint8_t
{
    Interactive,
    SmokeTest
};

/// @brief 公開済みPackageをManifest、Build Artifact、起動先と対応付けるSnapshot
struct PublishedRuntimePackageSnapshot final
{
    std::string operationId;
    std::string artifactId;
    std::string destination;
    std::string executable;
    PackageManifestSummary manifest;
};

/// @brief UIがLockなしで保持できるBuild・Package・Run全体の所有Snapshot
struct PackageWorkflowSnapshot final
{
    PackageWorkflowState state = PackageWorkflowState::Idle;
    PackageWorkflowStage activeStage = PackageWorkflowStage::None;
    BuildOperationSnapshot build;
    std::optional<PublishedRuntimePackageSnapshot> package;
    std::optional<PublishedRuntimePackageSnapshot> latestSuccessfulPackage;
    std::vector<ChildProcessOutputChunk> runOutput;
    std::string message;
};

/// @brief Game Build成功ArtifactからStandalone Package公開と起動を直列化するApplication Service
///
/// `start`、`retry`、`advance`、`run`、`wait_for_package`、`wait_for_run_completion`はcreate呼出Threadだけで
/// 使用する。`snapshot`、`request_cancel`、`stop`は任意Threadから呼べる。Build Serviceは借用し、本Serviceより
/// 長く生存させる。FilesystemとRun用Process RunnerはServiceが所有する。Destructorは進行中Build、Package、
/// Runtime Processを取消してJoinし、子Processを残さない。
class GamePackageWorkflowService final
{
  public:
    GamePackageWorkflowService(const GamePackageWorkflowService &) = delete;
    GamePackageWorkflowService &operator=(const GamePackageWorkflowService &) = delete;
    GamePackageWorkflowService(GamePackageWorkflowService &&) = delete;
    GamePackageWorkflowService &operator=(GamePackageWorkflowService &&) = delete;
    /// @brief 進行中Operationを取消し、所有Workerと子Processの終了を待つ
    ~GamePackageWorkflowService();

    /// @brief Build Service、Root限定Filesystem、Run用Process Runnerを一つのWorkflow Ownerへ束ねる
    [[nodiscard]] static Result<std::unique_ptr<GamePackageWorkflowService>> create(
        GameBuildService &a_buildService, std::unique_ptr<FilesystemRoot> a_projectFilesystem,
        std::unique_ptr<FilesystemRoot> a_engineBinaryFilesystem,
        std::unique_ptr<ChildProcessRunner> a_runProcessRunner, std::string a_projectRoot,
        std::vector<ChildProcessEnvironmentEntry> a_runEnvironment, const AssertContext &a_assertContext) noexcept;

    /// @brief Runtime Dataを保持してGame Buildを開始し、成功後だけPackage公開へ進む
    [[nodiscard]] Result<void> start(BuildRequest a_buildRequest, CMakeConfigureMode a_configureMode,
                                     EngineVersion a_engineVersion, std::string a_projectId,
                                     MinimalRuntimeDataPublication a_runtimeData) noexcept;
    /// @brief 最後の入力を新Operation IDでBuildから再実行する
    [[nodiscard]] Result<void> retry(std::string a_operationId) noexcept;
    /// @brief Build完了を観測し、Package Worker開始または完了WorkerのJoinを行う
    void advance() noexcept;
    /// @brief 現在のBuildまたはPackageまたはRuntime Processへ取消を通知する
    [[nodiscard]] Result<void> request_cancel() noexcept;
    /// @brief 公開済みPackageのRuntime Hostを別Workerで起動する
    [[nodiscard]] Result<void> run(PackageRunMode a_mode) noexcept;
    /// @brief 起動中Runtime Processへ停止を通知する
    [[nodiscard]] Result<void> stop() noexcept;
    /// @brief Owner ThreadでBuildまたはPackageの完了まで待つ
    [[nodiscard]] Result<void> wait_for_package() noexcept;
    /// @brief Owner ThreadでRuntime Processの完了まで待つ
    [[nodiscard]] Result<void> wait_for_run_completion() noexcept;
    /// @brief Build、Package、Runの一時点を所有Copyとして返す
    [[nodiscard]] PackageWorkflowSnapshot snapshot() const noexcept;

  private:
    struct Impl;
    /// @brief 構築済み実装の所有権を取得してServiceを構築する
    explicit GamePackageWorkflowService(std::unique_ptr<Impl> a_impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};
} // namespace cue::package
