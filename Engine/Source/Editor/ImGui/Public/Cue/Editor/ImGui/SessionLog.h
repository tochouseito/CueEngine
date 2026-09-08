#pragma once

#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Result.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::editor
{
class EditorSessionLogBuffer;
class EditorSessionLogRouter;

/// @brief Console表示へ複製されたPointer非保持のLog値
struct EditorSessionLogEntry final
{
    LogLevel level;
    std::string message;
    std::uint64_t sessionGeneration = 0U;
};

/// @brief 一つのEditor Sessionだけが所有するProcess Logger購読Token
///
/// Token破棄はRouterから購読を同期解除し、以後のCallbackがSession UIへ到達しないことを保証する
class EditorSessionLogSubscription final
{
  public:
    /// @brief Routerだけが購読Token Constructorへ渡せる生成権限
    class ConstructionKey final
    {
      public:
        /// @brief Router内部で生成権限を値として複製する
        ConstructionKey(const ConstructionKey &) noexcept = default;
        /// @brief Router内部で生成権限を値として複製代入する
        ConstructionKey &operator=(const ConstructionKey &) noexcept = default;
        /// @brief Router内部で生成権限を値として移動する
        ConstructionKey(ConstructionKey &&) noexcept = default;
        /// @brief Router内部で生成権限を値として移動代入する
        ConstructionKey &operator=(ConstructionKey &&) noexcept = default;
        /// @brief bit_castによる権限生成を防ぐnon-trivialな破棄を行う
        ~ConstructionKey() noexcept
        {
        }

      private:
        friend class EditorSessionLogRouter;

        /// @brief EditorSessionLogRouterだけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief Router世代とSession-local Buffer所有権を一つのTokenへ固定する
    EditorSessionLogSubscription(ConstructionKey, EditorSessionLogRouter &a_router, std::uint64_t a_generation,
                                 std::shared_ptr<EditorSessionLogBuffer> a_buffer) noexcept;
    /// @brief 購読Tokenの複製による二重解除を禁止する
    EditorSessionLogSubscription(const EditorSessionLogSubscription &) = delete;
    /// @brief 購読Tokenの複製代入による二重解除を禁止する
    EditorSessionLogSubscription &operator=(const EditorSessionLogSubscription &) = delete;
    /// @brief Routerが追跡するToken Addressを安定させるためMove構築を禁止する
    EditorSessionLogSubscription(EditorSessionLogSubscription &&) = delete;
    /// @brief Routerが追跡するToken Addressを安定させるためMove代入を禁止する
    EditorSessionLogSubscription &operator=(EditorSessionLogSubscription &&) = delete;
    /// @brief Router購読を同期解除してSession-local Log値を破棄する
    ~EditorSessionLogSubscription() noexcept;

    /// @brief ASCII Case-insensitive Filterに一致する現在のLog値を所有Snapshotとして返す
    [[nodiscard]] std::vector<EditorSessionLogEntry> snapshot(std::string_view a_filter) const;
    /// @brief 以後のLog値へ付与するStable Play Session Generationを設定する
    void set_session_generation(std::uint64_t a_generation) noexcept;
    /// @brief 現在Sessionで収集したLog値をすべて破棄する
    void clear() noexcept;

  private:
    EditorSessionLogRouter *m_router;
    std::shared_ptr<EditorSessionLogBuffer> m_buffer;
    std::uint64_t m_generation;
};

/// @brief Process Loggerを一つのEditor Session購読へ同期配送するLog Sink
///
/// Router自体はProcess ScopeでLoggerが所有し、Session UIやPlay Controllerを所有しない
class EditorSessionLogRouter final : public LogSink
{
  public:
    /// @brief 購読を持たないProcess Scope Routerを生成する
    EditorSessionLogRouter() noexcept;
    /// @brief Logger破棄時に購読が解除済みであることを確認して内部状態を破棄する
    ~EditorSessionLogRouter() override;

    /// @brief Process Scope Routerの複製を禁止する
    EditorSessionLogRouter(const EditorSessionLogRouter &) = delete;
    /// @brief Process Scope Routerの複製代入を禁止する
    EditorSessionLogRouter &operator=(const EditorSessionLogRouter &) = delete;
    /// @brief Logger所有中のRouter Addressを安定させるためMove構築を禁止する
    EditorSessionLogRouter(EditorSessionLogRouter &&) = delete;
    /// @brief Logger所有中のRouter Addressを安定させるためMove代入を禁止する
    EditorSessionLogRouter &operator=(EditorSessionLogRouter &&) = delete;

    /// @brief 未購読Routerへ一つのSession-local購読Tokenを発行する
    [[nodiscard]] Result<std::unique_ptr<EditorSessionLogSubscription>> subscribe(
        const AssertContext &a_assertContext) noexcept;
    /// @brief 現在有効なSession購読が存在するか同期確認する
    [[nodiscard]] bool has_active_subscription() const noexcept;

    /// @brief 有効なSession購読へPointer非保持のLog値を同期複製する
    [[nodiscard]] bool write(const LogRecord &a_record) noexcept override;
    /// @brief Router自身に保留出力がないため常に成功する
    [[nodiscard]] bool flush() noexcept override;

  private:
    friend class EditorSessionLogSubscription;

    /// @brief 対応世代の購読だけを同期解除し、進行中write完了後の配送を停止する
    void unsubscribe(std::uint64_t a_generation) noexcept;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace cue::editor
