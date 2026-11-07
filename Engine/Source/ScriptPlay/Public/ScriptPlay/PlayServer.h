#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ScriptPlay/PlayClient.h>

namespace cue
{
/// @brief Session 検証済みの所有 Command。World の Owner が更新境界で消費する
struct PlayCommand final
{
    enum class Kind
    {
        Start,
        Stop,
        SetProperty,
        ContinueDebug
    };
    Kind kind = Kind::Stop;
    std::uint64_t requestId = 0;
    std::string sceneText;
    std::string objectId;
    std::string scriptInstanceId;
    ScriptPropertyValue value;
};

/// @brief 専用 Play Process が所有する通信端点。World / DLL の所有権は持たない
/// 全操作は生成 Thread。受信・送信は非待機で、poll を繰り返して送信を完了する
class PlayServer final
{
  public:
    struct State;
    class ConstructionKey final
    {
        friend class PlayServer;
        /// @brief 接続済み Factory 以外の生成を禁止する
        ConstructionKey() = default;
    };
    /// @brief Factory が接続した通信状態を所有する
    PlayServer(ConstructionKey, std::unique_ptr<State> a_state);
    /// @brief 親が明示した Local Pipe と Session に接続する
    [[nodiscard]] static Result<std::unique_ptr<PlayServer>> connect(std::string_view a_pipeName,
                                                                     std::uint64_t a_session, bool a_isDebug = false);
    /// @brief 所有通信 Handle を閉じる
    ~PlayServer();
    /// @brief 到着済み Command を所有値で返す。切断・不正入力では失敗する
    [[nodiscard]] Result<std::vector<PlayCommand>> poll();
    /// @brief DLL の Metadata を所有 Message に複写して送信予約する
    [[nodiscard]] Result<void> send_types(const std::vector<ScriptTypeDescriptor> &a_types);
    /// @brief World と表示の初期化完了を通知する
    [[nodiscard]] Result<void> send_ready();
    /// @brief DLL をまだ読まず接続待機に入ったことを親へ通知する。Debug 接続で一度だけ呼ぶ
    [[nodiscard]] Result<void> send_debug_waiting();
    /// @brief 変更の確定値または拒否理由を通知する
    [[nodiscard]] Result<void> send_ack(const PlayPropertyAck &a_ack);
    /// @brief 異常終了前に可能な範囲で診断を送信する
    [[nodiscard]] Result<void> send_error(std::string_view a_message);
    /// @brief 送信予約が残っているか返す。終了時の排出は上位が期限を設ける
    [[nodiscard]] bool has_pending_output() const noexcept;

  private:
    std::unique_ptr<State> m_state;
};
} // namespace cue
