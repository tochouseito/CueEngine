#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/RuntimeSystem.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace cue
{
class AssertContext;
class FrameInputSnapshot;
} // namespace cue

namespace cue::game_core
{
class RuntimeWorld;

/// @brief Runtime System Registryの明示的な寿命状態
enum class RuntimeSystemRegistryState
{
    Registering,
    Sealed,
    Started,
    StopPending,
    Stopped
};

/// @brief System所有権、決定的実行順、再試行可能な逆順停止を管理する単一Thread Registry
///
/// 全公開APIとDestructorは構築Threadから呼び、StartedまたはStopPendingのまま破棄しない
class RuntimeSystemRegistry final
{
  public:
    /// @brief 呼出しThreadをOwnerとする空の登録受付中Registryを構築する
    explicit RuntimeSystemRegistry(const AssertContext &a_assertContext) noexcept;
    /// @brief System所有権の複製を禁止する
    RuntimeSystemRegistry(const RuntimeSystemRegistry &) = delete;
    /// @brief System所有権の複製代入を禁止する
    RuntimeSystemRegistry &operator=(const RuntimeSystemRegistry &) = delete;
    /// @brief System AddressとOwner Threadを固定するためMove構築を禁止する
    RuntimeSystemRegistry(RuntimeSystemRegistry &&) = delete;
    /// @brief System AddressとOwner Threadを固定するためMove代入を禁止する
    RuntimeSystemRegistry &operator=(RuntimeSystemRegistry &&) = delete;
    /// @brief Owner Thread上で開始済みSystemが残っていないことを検証して所有物を破棄する
    ~RuntimeSystemRegistry() noexcept;

    /// @brief 一意IDを持つSystemの所有権と順序定義を登録する
    /// @details 失敗時もSystem所有権を消費し、Registryの登録集合は変更しない
    [[nodiscard]] Result<void> register_system(RuntimeSystemDescriptor a_descriptor,
                                               std::unique_ptr<RuntimeSystem> a_system) noexcept;
    /// @brief Phase、Order、登録順から実行順を固定し、必須Systemが先行することを検証する
    [[nodiscard]] Result<void> seal() noexcept;
    /// @brief Seal済み順序でSystemを開始し、途中失敗では開始済みSystemだけを逆順停止する
    /// @details 成功後は全 System の Stop 完了まで開始時 RuntimeWorld の終了と破棄を固定する
    [[nodiscard]] Result<void> start(RuntimeWorld &a_runtimeWorld) noexcept;
    /// @brief Started Systemを固定順で更新し、Structural CommandのFlushは呼出し側へ委ねる
    [[nodiscard]] Result<void> update(RuntimeWorld &a_runtimeWorld, const UpdateContext &a_timing,
                                      const FrameInputSnapshot &a_input) noexcept;
    /// @brief 依存先を保持しながら開始済みSystemを逆順停止し、未完了Systemだけを再試行する
    [[nodiscard]] Result<void> stop(RuntimeWorld &a_runtimeWorld) noexcept;

    /// @brief 現在のRegistry寿命状態を返す
    [[nodiscard]] RuntimeSystemRegistryState state() const noexcept;
    /// @brief 登録済みSystem数を返す
    [[nodiscard]] std::size_t system_count() const noexcept;
    /// @brief Start済みまたはStop再試行待ちのSystem数を返す
    [[nodiscard]] std::size_t active_system_count() const noexcept;

  private:
    class Entry;

    /// @brief 現在ThreadがRegistry Ownerであることを全構成で検証する
    void assert_owner_thread() const noexcept;
    /// @brief RuntimeWorldが要求状態と開始時Instance条件を満たしWorld Scope参照を公開できることを検証する
    [[nodiscard]] Result<void> validate_runtime_world(RuntimeWorld &a_runtimeWorld, bool a_requiresRunning,
                                                      const RuntimeWorld *a_expectedRuntimeWorld) const noexcept;
    /// @brief 開始済みSystemを依存先保持付きで逆順停止する
    [[nodiscard]] Result<void> stop_started(RuntimeWorld &a_runtimeWorld, RuntimeSystemContext &a_context) noexcept;
    /// @brief 指定Systemを必要とする未停止Systemが残る場合にtrueを返す
    [[nodiscard]] bool has_live_dependent(std::string_view a_systemId) const noexcept;
    /// @brief Assertを再入せず現在の開始済みSystem数を数える
    [[nodiscard]] std::size_t count_active_systems() const noexcept;
    /// @brief Registry自身の状態違反を診断するErrorを生成する
    [[nodiscard]] Error make_state_error(std::string_view a_summary) const noexcept;
    /// @brief Allocation失敗をEngineのFatal契約へ変換する
    [[noreturn]] void terminate_allocation() const noexcept;
    /// @brief 予期しない例外をEngineのFatal契約へ変換する
    [[noreturn]] void terminate_exception() const noexcept;

    const AssertContext *m_assertContext;
    std::thread::id m_ownerThread;
    RuntimeSystemRegistryState m_state = RuntimeSystemRegistryState::Registering;
    RuntimeWorld *m_runtimeWorld = nullptr;
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::vector<std::size_t> m_executionOrder;
    bool m_isInvokingCallback = false;
};
} // namespace cue::game_core
