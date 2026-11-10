#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <Foundation/Path.h>
#include <Foundation/Result.h>
#include <ProjectBuild/BuildTarget.h>

namespace cue
{
class Project;

/// @brief 一つの製品 Build 要求の進行。成功は Package の再検証完了を含む
enum class GameBuildState
{
    Idle,
    Configuring,
    Building,
    Succeeded,
    Failed,
    Cancelled
};

/// @brief ゲームを Root とする生成対象。IDE と Script は開発 Tree、製品は専用 Tree を使う
enum class GameBuildPurpose
{
    App,
    ScriptModule,
    IdeProject
};

/// @brief Editor が表示する現在値。Status の借用は次の Service 操作まで有効
struct GameBuildStatus final
{
    GameBuildState state = GameBuildState::Idle;
    GameBuildTarget target;
    std::optional<Error> error;
    std::optional<std::uint32_t> exitCode;
    Path outputDirectory;
    Path executable;
    // 標準出力と標準 Error の末尾を上限付きで保持し、毎 Frame の File 出力は行わない
    std::string log;
    bool isLogTruncated = false;
    bool isApplicationRunning = false;
    std::optional<std::uint32_t> applicationExitCode;
    GameBuildPurpose purpose = GameBuildPurpose::App;
    Path workspaceDirectory;
    Path moduleFile;
    Path symbolsFile;
};

/// @brief Build と製品起動を抽象化する。全操作・破棄は生成 Thread で直列化し、再入しない
/// Project / Target の借用は start 中だけ。Build 子孫は shutdown で停止し、製品の独立実行は停止しない
class IGameBuildService
{
  public:
    /// @brief Platform 実装の一意所有を許可する
    IGameBuildService() = default;
    /// @brief Build 子孫を回収し、製品 Process の監視 Handle だけを解放する
    virtual ~IGameBuildService() = default;
    IGameBuildService(const IGameBuildService &) = delete;
    IGameBuildService &operator=(const IGameBuildService &) = delete;
    /// @brief 入力を検証し非待機で開始する。二重実行・製品稼働中は拒否し、検証失敗では旧結果を維持する
    [[nodiscard]] virtual Result<void> start(const Project &a_project, const GameBuildTarget &a_target) = 0;
    /// @brief IDE 生成・開発 DLL Build を明示する。旧実装は製品だけを受け付ける互換入口
    [[nodiscard]] virtual Result<void> start(const Project &a_project, const GameBuildTarget &a_target,
                                             GameBuildPurpose a_purpose)
    {
        if (a_purpose != GameBuildPurpose::App)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "GameBuild.purpose_unsupported"});
        return start(a_project, a_target);
    }
    /// @brief 上限付き出力と終了を非待機で回収し、Configure 成功後だけ Build を開始する
    [[nodiscard]] virtual Result<void> poll() = 0;
    /// @brief Build 子孫へ取消を要求する。完了済みなら何も変更せず成功する
    [[nodiscard]] virtual Result<void> cancel() = 0;
    /// @brief 最後に成功した配布物を再検証し、製品 Process を独立起動する。終了は poll で回収する
    [[nodiscard]] virtual Result<void> launch() = 0;
    /// @brief Owner Thread で旧 Project の要求・成功成果物・表示値を破棄し、次の start を許可する
    /// Build / 製品の実行中と停止後は拒否し、失敗時は旧状態を維持する。Status の借用は成功時に失効する
    [[nodiscard]] virtual Result<void> reset_project()
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "GameBuild.reset_project_unsupported"});
    }
    /// @brief Build 子孫を停止して Service を閉じる。複数回呼べ、製品は終了させない
    [[nodiscard]] virtual Result<void> shutdown() = 0;
    /// @brief Owner Thread が現在の表示値を借用する。Thread-safe な Snapshot ではない
    [[nodiscard]] virtual const GameBuildStatus &status() const noexcept = 0;
};
} // namespace cue
