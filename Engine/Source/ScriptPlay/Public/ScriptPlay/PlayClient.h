#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Foundation/Path.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
class Scene;

/// @brief 子 Process の状態。停止要求後も終了確認までは新しい Session を開始しない
enum class PlayState
{
    Idle,
    Describing,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
    WaitingForDebugger
};

/// @brief 明示した実行 File と Game Module で子 Process を起動する。両 Path は絶対 Path
struct PlayClientConfig final
{
    Path executable;
    Path module;
    bool isHeadless = false;
    std::uint32_t maxTicks = 0;
};

/// @brief 子 World の確定境界で処理した Property 変更の応答。編集正本は変更しない
struct PlayPropertyAck final
{
    std::uint64_t requestId = 0;
    bool isAccepted = false;
    std::string objectId;
    std::string scriptInstanceId;
    ScriptPropertyValue value;
    std::string error;
};

/// @brief Game DLL を読み込まず、専用子 Process の Metadata と Play Session を管理する
/// 全操作・破棄は生成 Thread。poll は非待機で、Stop 後 2 秒の無応答では子 Job を終了する
/// 起動は所有 Scene Snapshot を転送し、Document / World / DLL Pointer を共有しない
class PlayClient final
{
  public:
    class ConstructionKey final
    {
        friend class PlayClient;
        /// @brief Factory 以外の空状態生成を禁止する
        ConstructionKey() = default;
    };
    /// @brief Factory が検証した起動設定を所有する
    PlayClient(ConstructionKey, PlayClientConfig a_config);
    /// @brief 空の Process 所有状態を作る。実行 File の存在は起動時に検証する
    [[nodiscard]] static Result<std::unique_ptr<PlayClient>> create(PlayClientConfig a_config);
    /// @brief 所有する子 Job を終了し、Pipe と Process Handle を閉じる
    ~PlayClient();
    PlayClient(const PlayClient &) = delete;
    PlayClient &operator=(const PlayClient &) = delete;
    /// @brief Metadata 専用の短命な子 Process を開始する。結果は poll 後の types で得る
    [[nodiscard]] Result<void> describe();
    /// @brief 未保存 Scene を複写して新しい Play Session を開始する
    [[nodiscard]] Result<void> start(const Scene &a_scene);
    /// @brief DLL 読込前に接続待機する子を起動し、所有 Scene を転送する。実行開始は continue_debug で許可する
    [[nodiscard]] Result<void> start_debug(const Scene &a_scene);
    /// @brief 待機中の所有子へ接続済み Debugger を確認し、Script 開始を予約する。未接続では状態を変えない
    [[nodiscard]] Result<void> continue_debug();
    /// @brief 所有する生存子の PID を返す。Owner 以外・終了後は 0。PID の寿命は次の poll / 停止まで
    [[nodiscard]] std::uint32_t process_id() const noexcept;
    /// @brief 現 Session が接続待機付きの Play か返す。Metadata 取得は含めない
    [[nodiscard]] bool is_debugging() const noexcept;
    /// @brief 所有する生存子に Debugger が接続されているか非待機で確認する。Owner 以外は false
    [[nodiscard]] bool is_debugger_attached() const noexcept;
    /// @brief 通信と終了状態を非待機で更新する。子の失敗は Failed と error に保持する
    [[nodiscard]] Result<void> poll();
    /// @brief 終了を予約する。完了確認まで poll を継続する。停止済みでは何もしない
    [[nodiscard]] Result<void> stop();
    /// @brief Session 付きの型付き変更要求を予約する。成功値は応答照合用 ID
    [[nodiscard]] Result<std::uint64_t> set_property(std::string_view a_objectId, std::string_view a_scriptInstanceId,
                                                     const ScriptPropertyValue &a_value);
    /// @brief 次の操作まで有効な状態値を返す
    [[nodiscard]] PlayState state() const noexcept;
    /// @brief 最後に子 Process から複写した Metadata を借用する。次の poll / 破棄で失効する
    [[nodiscard]] const std::vector<ScriptTypeDescriptor> &types() const noexcept;
    /// @brief 最終の Process / 通信失敗の説明を借用する
    [[nodiscard]] std::string_view error() const noexcept;
    /// @brief Process ごとに更新する非永続の Session 識別子を返す
    [[nodiscard]] std::uint64_t session_id() const noexcept;
    /// @brief 確定済み応答の所有権を移す。未応答要求は含めない
    [[nodiscard]] std::vector<PlayPropertyAck> take_acknowledgements();

  private:
    struct State;
    /// @brief Pipe Server と停止時回収する Job を作り、明示した子実行 File を起動する
    [[nodiscard]] Result<void> launch(bool a_describe, bool a_isDebug = false);
    /// @brief Scene を検証・複写してから指定した通常 / Debug 子を起動する
    [[nodiscard]] Result<void> start_session(const Scene &a_scene, bool a_isDebug);
    std::unique_ptr<State> m_state;
};
} // namespace cue
