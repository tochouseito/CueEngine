#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include <Foundation/Logging.h>

namespace cue
{
/// @brief Sink を所有し、同じ整形結果を登録した全出力先へ直列配送する
/// write / flush / shutdown は Thread-safe。Sink から Logger への再入は拒否する
/// 利用者を止めて shutdown の Result を確認してから破棄する。Destructor は最後の回収だけを行う
/// Allocation 失敗は例外として伝播する。Sink の失敗後も残る Sink への配送を試みる
class Logger final : public ILogger
{
    struct CreateToken final
    {
    };

  public:
    /// @brief Factory が検証した出力先の所有権を受け取る
    Logger(CreateToken, std::vector<std::unique_ptr<ILogSink>> a_sinks, LogLevel a_minimum);
    /// @brief 明示停止されていない出力先を回収する
    ~Logger() override;
    /// @brief 少なくとも一つの Sink と有効な Level を検証し、成功時だけ Logger を公開する
    [[nodiscard]] static Result<std::unique_ptr<Logger>> create(std::vector<std::unique_ptr<ILogSink>> a_sinks,
                                                                LogLevel a_minimum = LogLevel::Info);
    /// @brief Level で選別して配送する。フィルタされた Record は成功する
    [[nodiscard]] Result<void> write(const LogRecord &a_record) override;
    /// @brief 全 Sink を Flush し、最初の失敗を返す
    [[nodiscard]] Result<void> flush() override;
    /// @brief 全 Sink の Flush / Close を試み、以後の書込みを拒否する
    /// Close 失敗は次の shutdown で再試行する。部分配送済み Record の再送は行わない
    [[nodiscard]] Result<void> shutdown();

  private:
    std::mutex m_mutex;
    std::vector<std::unique_ptr<ILogSink>> m_sinks;
    LogLevel m_minimum;
    bool m_isStopped = false;
};
} // namespace cue
