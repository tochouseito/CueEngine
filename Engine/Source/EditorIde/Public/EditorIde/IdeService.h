#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue
{
enum class IdeOperation
{
    None,
    OpenSolution,
    OpenSource,
    AttachDebugger
};
enum class IdeState
{
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled
};
/// @brief 次の Service 操作まで借用する、要求と処理結果の所有値
struct IdeStatus final
{
    IdeOperation operation = IdeOperation::None;
    IdeState state = IdeState::Idle;
    std::uint64_t requestId = 0;
    Path solution;
    Path source;
    std::uint32_t sourceLine = 1;
    std::uint32_t targetProcessId = 0;
    std::uint32_t ideProcessId = 0;
    std::optional<Error> error;
    std::string message;
};
/// @brief 外部 IDE の操作契約。全操作と破棄は生成 Thread で直列化し、再入しない
/// Path の借用は呼出中だけ。非同期処理は独立した所有値を保持する
class IEditorIdeService
{
  public:
    /// @brief Platform 実装を一意所有する
    virtual ~IEditorIdeService() = default;
    /// @brief 指定 Solution を開く。既存の別 Solution は置き換えない
    [[nodiscard]] virtual Result<void> open_solution(const Path &a_solution) = 0;
    /// @brief Project 内で解決済みの Source を指定行で開く
    [[nodiscard]] virtual Result<void> open_source(const Path &a_solution, const Path &a_source,
                                                   std::uint32_t a_line = 1) = 0;
    /// @brief 対象 Solution の IDE を選び、実行 File を照合した指定 Process へ Native 接続する
    [[nodiscard]] virtual Result<void> attach_debugger(const Path &a_solution, std::uint32_t a_processId,
                                                       const Path &a_expectedExecutable) = 0;
    /// @brief 出力と終了を期限付き・非待機で回収する
    [[nodiscard]] virtual Result<void> poll() = 0;
    /// @brief 所有 Bridge の取消を要求する。Visual Studio や接続先を終了しない
    [[nodiscard]] virtual Result<void> cancel() = 0;
    /// @brief 所有 Bridge を回収する。IDE / Play Process は所有しない
    [[nodiscard]] virtual Result<void> shutdown() = 0;
    /// @brief Owner Thread だけが現在値を借用する。Thread-safe な Snapshot ではない
    [[nodiscard]] virtual const IdeStatus &status() const noexcept = 0;
};
} // namespace cue
