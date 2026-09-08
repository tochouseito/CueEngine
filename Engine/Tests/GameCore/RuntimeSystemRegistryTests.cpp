#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/Error.h>
#include <Cue/GameCore/RuntimeSystemRegistry.h>
#include <Cue/GameCore/RuntimeWorld.h>
#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Input/InputState.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Registry Test中の契約違反を固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Message付き契約違反を固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

class ScriptedClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief Update Testへ渡すSample列を非所有で保持する
    explicit ScriptedClock(std::span<const std::int64_t> a_samples) noexcept : m_samples(a_samples)
    {
    }

    /// @brief Sampleを登録順に返し列不足を診断する
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        if (m_nextIndex == m_samples.size())
        {
            return cue::Result<cue::game_core::MonotonicClockSample>::failure(
                cue::game_core::make_game_core_error(a_assertContext, cue::game_core::GameCoreError::InvalidClockSample,
                                                     "Registry test clock sample exhausted"));
        }

        return cue::Result<cue::game_core::MonotonicClockSample>::success(
            cue::game_core::MonotonicClockSample{m_samples[m_nextIndex++]});
    }

  private:
    std::span<const std::int64_t> m_samples;
    std::size_t m_nextIndex = 0;
};

enum class EventKind
{
    Start,
    Update,
    Stop
};

struct SystemEvent final
{
    int system = 0;
    EventKind kind = EventKind::Start;

    /// @brief SystemとCallback種別が一致する場合にtrueを返す
    [[nodiscard]] bool operator==(const SystemEvent &) const noexcept = default;
};

struct RecordingOptions final
{
    bool failsStart = false;
    bool failsUpdate = false;
    bool createsEntity = false;
    int remainingStopFailures = 0;
};

class RecordingSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief Callback順と注入失敗を共有Test状態へ記録するSystemを構築する
    RecordingSystem(int a_system, std::vector<SystemEvent> &a_events, const cue::AssertContext &a_assertContext,
                    RecordingOptions a_options = {}) noexcept
        : m_events(&a_events), m_assertContext(&a_assertContext), m_options(a_options), m_system(a_system)
    {
    }

    /// @brief Start順を記録し、指定時は副作用をCommitせず失敗する
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        m_events->push_back({m_system, EventKind::Start});
        if (m_options.failsStart)
        {
            return failure("Injected runtime system start failure");
        }
        return cue::Result<void>::success();
    }

    /// @brief Update順とFrame値を記録し、必要ならStructural Commandを一つ登録する
    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        m_events->push_back({m_system, EventKind::Update});
        m_observedFrameIndex = a_context.timing.timing().frameIndex;
        m_observedFocus = a_context.input.has_focus();
        if (m_options.failsUpdate)
        {
            return failure("Injected runtime system update failure");
        }
        if (m_options.createsEntity)
        {
            cue::Result<cue::game_core::PendingEntityId> pending = a_context.commands.create_entity();
            if (!pending)
            {
                return cue::Result<void>::failure(std::move(*pending.try_error()));
            }
        }
        return cue::Result<void>::success();
    }

    /// @brief Stop試行順を記録し、注入回数を消費した後だけ成功する
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        m_events->push_back({m_system, EventKind::Stop});
        if (m_options.remainingStopFailures > 0)
        {
            --m_options.remainingStopFailures;
            return failure("Injected runtime system stop failure");
        }
        return cue::Result<void>::success();
    }

    /// @brief 最後に観測したFrame Indexを返す
    [[nodiscard]] std::uint64_t observed_frame_index() const noexcept
    {
        return m_observedFrameIndex;
    }

    /// @brief 最後に観測したInput Focusを返す
    [[nodiscard]] bool observed_focus() const noexcept
    {
        return m_observedFocus;
    }

  private:
    /// @brief 注入失敗を既存GameCore Errorとして生成する
    [[nodiscard]] cue::Result<void> failure(std::string_view a_summary) noexcept
    {
        return cue::Result<void>::failure(cue::game_core::make_game_core_error(
            *m_assertContext, cue::game_core::GameCoreError::DependencyFailed, a_summary));
    }

    std::vector<SystemEvent> *m_events;
    const cue::AssertContext *m_assertContext;
    RecordingOptions m_options;
    std::uint64_t m_observedFrameIndex = 0;
    int m_system;
    bool m_observedFocus = false;
};

class ReentrantSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief 各Lifecycle Callbackから同じRegistryへの再入を試すSystemを構築する
    ReentrantSystem(cue::game_core::RuntimeSystemRegistry &a_registry, cue::game_core::RuntimeWorld &a_runtime) noexcept
        : m_registry(&a_registry), m_runtime(&a_runtime)
    {
    }

    /// @brief Start Callback中のStop再入が状態Errorとして拒否されたことを記録する
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        m_startRejected = is_reentry_rejected(m_registry->stop(*m_runtime));
        return cue::Result<void>::success();
    }

    /// @brief Update Callback中のStop再入が状態Errorとして拒否されたことを記録する
    [[nodiscard]] cue::Result<void> update(const cue::game_core::RuntimeSystemUpdateContext &) noexcept override
    {
        m_updateRejected = is_reentry_rejected(m_registry->stop(*m_runtime));
        return cue::Result<void>::success();
    }

    /// @brief Stop Callback中のStop再入が状態Errorとして拒否されたことを記録する
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        m_stopRejected = is_reentry_rejected(m_registry->stop(*m_runtime));
        return cue::Result<void>::success();
    }

    /// @brief 全Lifecycle Callbackの再入が拒否された場合にtrueを返す
    [[nodiscard]] bool rejected_all_reentry() const noexcept
    {
        return m_startRejected && m_updateRejected && m_stopRejected;
    }

  private:
    /// @brief 再入ResultがRegistry状態Errorの場合にtrueを返す
    [[nodiscard]] static bool is_reentry_rejected(const cue::Result<void> &a_result) noexcept
    {
        return !a_result && a_result.try_error()->code().domain() == "Cue.GameCore" &&
               a_result.try_error()->code().value() ==
                   static_cast<std::int64_t>(cue::game_core::GameCoreError::InvalidSystemRegistryState);
    }

    cue::game_core::RuntimeSystemRegistry *m_registry;
    cue::game_core::RuntimeWorld *m_runtime;
    bool m_startRejected = false;
    bool m_updateRejected = false;
    bool m_stopRejected = false;
};

/// @brief Test用Loggerを追加Sinkなしで生成する
[[nodiscard]] std::unique_ptr<cue::Logger> create_logger(TestFatalHandler &a_handler)
{
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    return std::make_unique<cue::Logger>(a_handler, std::move(sinks));
}

/// @brief TextからTest用の検証済みTypeIdを生成する
[[nodiscard]] cue::schema::TypeId make_type_id(std::string_view a_text, const cue::AssertContext &a_assertContext)
{
    cue::Result<cue::schema::TypeId> result = cue::schema::TypeId::parse(a_text, a_assertContext);
    if (!result)
    {
        std::_Exit(2);
    }
    return std::move(*result.try_value());
}

/// @brief Core Transformだけを登録したSeal済みSchema Registryを生成する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> create_schema_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::schema::TypeId &a_transformTypeId,
    const cue::AssertContext &a_assertContext)
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    cue::Result<cue::schema::SchemaVersion> version = cue::schema::SchemaVersion::create(1U, a_assertContext);
    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reserved;
    if (!version)
    {
        return nullptr;
    }

    auto descriptor =
        cue::schema::create_type_descriptor(a_transformTypeId, "Cue.Core.Transform", std::move(*version.try_value()),
                                            std::move(fields), std::move(reserved), a_assertContext);
    if (!descriptor || !builder.add_type(std::move(*descriptor.try_value())))
    {
        return nullptr;
    }

    auto registry = builder.seal();
    return registry ? std::move(*registry.try_value()) : nullptr;
}

/// @brief Headless RuntimeWorldを生成してRunningまで初期化する
[[nodiscard]] std::unique_ptr<cue::game_core::RuntimeWorld> create_runtime(
    cue::game_core::WorldIdentitySource &a_identitySource, const cue::schema::SchemaRegistry &a_registry,
    cue::schema::TypeId a_transformTypeId, const cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::game_core::RuntimeWorld> runtime = cue::game_core::RuntimeWorld::create(
        a_identitySource, a_registry, std::move(a_transformTypeId), a_assertContext);
    return runtime->initialize() ? std::move(runtime) : nullptr;
}

/// @brief Running RuntimeWorldを既存Safe Point契約で終了する
[[nodiscard]] bool shutdown_runtime(cue::game_core::RuntimeWorld &a_runtime) noexcept
{
    return a_runtime.request_stop() && a_runtime.tick() &&
           a_runtime.state() == cue::game_core::RuntimeWorldState::Shutdown;
}

/// @brief GameClock経由で検証済みのFrame更新値を生成する
[[nodiscard]] cue::Result<cue::game_core::UpdateContext> create_update_context(
    cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::array<std::int64_t, 2> k_samples = {100, 116};
    ScriptedClock source(k_samples);
    cue::Result<cue::game_core::GameClock> clockResult = cue::game_core::GameClock::create(source, 20, a_assertContext);
    if (!clockResult)
    {
        return cue::Result<cue::game_core::UpdateContext>::failure(std::move(*clockResult.try_error()));
    }
    cue::game_core::GameClock clock = std::move(*clockResult.try_value());
    cue::Result<void> reset = clock.reset();
    if (!reset)
    {
        return cue::Result<cue::game_core::UpdateContext>::failure(std::move(*reset.try_error()));
    }
    return clock.advance_frame();
}

/// @brief 簡潔な登録値を所有Descriptorへ変換する
[[nodiscard]] cue::game_core::RuntimeSystemDescriptor make_descriptor(
    std::string a_id, cue::game_core::RuntimeUpdatePhase a_phase, std::int32_t a_order,
    std::initializer_list<std::string_view> a_dependencies = {})
{
    cue::game_core::RuntimeSystemDescriptor descriptor;
    descriptor.id = std::move(a_id);
    descriptor.phase = a_phase;
    descriptor.order = a_order;
    for (std::string_view dependency : a_dependencies)
    {
        descriptor.dependencies.emplace_back(dependency);
    }
    return descriptor;
}

/// @brief RecordingSystemを生成してRegistryへ所有権を渡す
[[nodiscard]] cue::Result<void> register_system(cue::game_core::RuntimeSystemRegistry &a_registry,
                                                cue::game_core::RuntimeSystemDescriptor a_descriptor, int a_system,
                                                std::vector<SystemEvent> &a_events,
                                                const cue::AssertContext &a_assertContext,
                                                RecordingOptions a_options = {},
                                                RecordingSystem **a_registeredSystem = nullptr)
{
    auto system = std::make_unique<RecordingSystem>(a_system, a_events, a_assertContext, a_options);
    if (a_registeredSystem != nullptr)
    {
        *a_registeredSystem = system.get();
    }
    return a_registry.register_system(std::move(a_descriptor), std::move(system));
}

/// @brief Result Errorが指定GameCore Codeを持つ場合にtrueを返す
template <typename T>
[[nodiscard]] bool has_error_code(const cue::Result<T> &a_result, cue::game_core::GameCoreError a_code) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.GameCore" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Result ErrorのRoot Causeが指定GameCore Codeの場合にtrueを返す
template <typename T>
[[nodiscard]] bool has_root_error_code(const cue::Result<T> &a_result, cue::game_core::GameCoreError a_code) noexcept
{
    return !a_result && a_result.try_error()->root_code().domain() == "Cue.GameCore" &&
           a_result.try_error()->root_code().value() == static_cast<std::int64_t>(a_code);
}

/// @brief Phase、Order、登録順の決定性と明示Safe Point接続を検証する
[[nodiscard]] bool test_deterministic_update_and_safe_point(cue::game_core::WorldIdentitySource &a_identitySource,
                                                            const cue::schema::SchemaRegistry &a_schemaRegistry,
                                                            cue::schema::TypeId a_transformTypeId,
                                                            cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::game_core::RuntimeWorld> runtime =
        create_runtime(a_identitySource, a_schemaRegistry, std::move(a_transformTypeId), a_assertContext);
    if (runtime == nullptr)
    {
        return false;
    }

    std::vector<SystemEvent> events;
    cue::game_core::RuntimeSystemRegistry registry(a_assertContext);
    RecordingSystem *observedSystem = nullptr;
    auto lateA =
        register_system(registry, make_descriptor("late-a", cue::game_core::RuntimeUpdatePhase::Update, 20, {"pre"}), 2,
                        events, a_assertContext, {.createsEntity = true}, &observedSystem);
    auto pre = register_system(registry, make_descriptor("pre", cue::game_core::RuntimeUpdatePhase::PreUpdate, 0), 1,
                               events, a_assertContext);
    auto lateB =
        register_system(registry, make_descriptor("late-b", cue::game_core::RuntimeUpdatePhase::Update, 20, {"pre"}), 3,
                        events, a_assertContext);
    auto post = register_system(
        registry, make_descriptor("post", cue::game_core::RuntimeUpdatePhase::PostUpdate, -10, {"late-a"}), 4, events,
        a_assertContext);
    if (!lateA || !pre || !lateB || !post || !registry.seal() || !registry.start(*runtime))
    {
        return false;
    }

    const std::vector<SystemEvent> expectedStart = {
        {1, EventKind::Start}, {2, EventKind::Start}, {3, EventKind::Start}, {4, EventKind::Start}};
    if (events != expectedStart || registry.system_count() != 4 || registry.active_system_count() != 4)
    {
        return false;
    }

    cue::Result<cue::game_core::UpdateContext> timing = create_update_context(a_assertContext);
    cue::InputState inputState;
    inputState.begin_frame({});
    if (!timing || !registry.update(*runtime, *timing.try_value(), inputState.snapshot()))
    {
        return false;
    }

    const std::vector<SystemEvent> expectedUpdated = {
        {1, EventKind::Start},  {2, EventKind::Start},  {3, EventKind::Start},  {4, EventKind::Start},
        {1, EventKind::Update}, {2, EventKind::Update}, {3, EventKind::Update}, {4, EventKind::Update}};
    if (events != expectedUpdated || observedSystem == nullptr || observedSystem->observed_frame_index() != 0 ||
        !observedSystem->observed_focus() || runtime->try_world()->entity_count() != 0)
    {
        return false;
    }

    cue::Result<cue::game_core::StructuralCommandReport> tick = runtime->tick();
    if (!tick || tick.try_value()->results().size() != 1 || runtime->try_world()->entity_count() != 1)
    {
        return false;
    }

    if (!registry.stop(*runtime))
    {
        return false;
    }
    const std::vector<SystemEvent> expectedStopped = {
        {1, EventKind::Start},  {2, EventKind::Start},  {3, EventKind::Start},  {4, EventKind::Start},
        {1, EventKind::Update}, {2, EventKind::Update}, {3, EventKind::Update}, {4, EventKind::Update},
        {4, EventKind::Stop},   {3, EventKind::Stop},   {2, EventKind::Stop},   {1, EventKind::Stop}};
    return events == expectedStopped && registry.state() == cue::game_core::RuntimeSystemRegistryState::Stopped &&
           registry.active_system_count() == 0 && shutdown_runtime(*runtime);
}

/// @brief 重複ID、依存不足、依存重複、逆向き依存をSeal前に診断する
[[nodiscard]] bool test_registration_diagnostics(cue::AssertContext &a_assertContext)
{
    std::vector<SystemEvent> events;
    cue::game_core::RuntimeSystemRegistry invalid(a_assertContext);
    auto emptyId = register_system(invalid, make_descriptor("", cue::game_core::RuntimeUpdatePhase::Update, 0), 0,
                                   events, a_assertContext);
    cue::Result<void> nullSystem =
        invalid.register_system(make_descriptor("null", cue::game_core::RuntimeUpdatePhase::Update, 0), nullptr);
    if (!has_error_code(emptyId, cue::game_core::GameCoreError::InvalidSystemRegistration) ||
        !has_error_code(nullSystem, cue::game_core::GameCoreError::InvalidSystemRegistration))
    {
        return false;
    }

    cue::game_core::RuntimeSystemRegistry duplicate(a_assertContext);
    auto first = register_system(duplicate, make_descriptor("same", cue::game_core::RuntimeUpdatePhase::Update, 0), 1,
                                 events, a_assertContext);
    auto second = register_system(duplicate, make_descriptor("same", cue::game_core::RuntimeUpdatePhase::Update, 1), 2,
                                  events, a_assertContext);
    if (!first || !has_error_code(second, cue::game_core::GameCoreError::DuplicateSystem) ||
        duplicate.system_count() != 1 || !duplicate.seal())
    {
        return false;
    }
    auto afterSeal = register_system(duplicate, make_descriptor("late", cue::game_core::RuntimeUpdatePhase::Update, 2),
                                     8, events, a_assertContext);
    if (!has_error_code(afterSeal, cue::game_core::GameCoreError::InvalidSystemRegistryState))
    {
        return false;
    }

    cue::game_core::RuntimeSystemRegistry missing(a_assertContext);
    auto missingRegistration = register_system(
        missing, make_descriptor("dependent", cue::game_core::RuntimeUpdatePhase::Update, 0, {"missing"}), 3, events,
        a_assertContext);
    cue::Result<void> missingSeal = missing.seal();
    if (!missingRegistration || !has_error_code(missingSeal, cue::game_core::GameCoreError::MissingSystemDependency) ||
        missing.state() != cue::game_core::RuntimeSystemRegistryState::Registering)
    {
        return false;
    }

    cue::game_core::RuntimeSystemRegistry repeated(a_assertContext);
    auto base = register_system(repeated, make_descriptor("base", cue::game_core::RuntimeUpdatePhase::PreUpdate, 0), 4,
                                events, a_assertContext);
    auto repeatedRegistration = register_system(
        repeated, make_descriptor("repeated", cue::game_core::RuntimeUpdatePhase::Update, 0, {"base", "base"}), 5,
        events, a_assertContext);
    cue::Result<void> repeatedSeal = repeated.seal();
    if (!base || !repeatedRegistration ||
        !has_error_code(repeatedSeal, cue::game_core::GameCoreError::InvalidSystemDependency))
    {
        return false;
    }

    cue::game_core::RuntimeSystemRegistry reverse(a_assertContext);
    auto dependent = register_system(
        reverse, make_descriptor("dependent", cue::game_core::RuntimeUpdatePhase::PreUpdate, 0, {"late"}), 6, events,
        a_assertContext);
    auto late = register_system(reverse, make_descriptor("late", cue::game_core::RuntimeUpdatePhase::PostUpdate, 0), 7,
                                events, a_assertContext);
    cue::Result<void> reverseSeal = reverse.seal();
    return dependent && late && has_error_code(reverseSeal, cue::game_core::GameCoreError::InvalidSystemDependency);
}

/// @brief Start途中失敗で成功済みSystemだけが逆順停止されることを検証する
[[nodiscard]] bool test_start_failure_rollback(cue::game_core::WorldIdentitySource &a_identitySource,
                                               const cue::schema::SchemaRegistry &a_schemaRegistry,
                                               cue::schema::TypeId a_transformTypeId,
                                               cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::game_core::RuntimeWorld> runtime =
        create_runtime(a_identitySource, a_schemaRegistry, std::move(a_transformTypeId), a_assertContext);
    if (runtime == nullptr)
    {
        return false;
    }

    std::vector<SystemEvent> events;
    cue::game_core::RuntimeSystemRegistry registry(a_assertContext);
    auto first = register_system(registry, make_descriptor("first", cue::game_core::RuntimeUpdatePhase::PreUpdate, 0),
                                 1, events, a_assertContext);
    auto second =
        register_system(registry, make_descriptor("second", cue::game_core::RuntimeUpdatePhase::Update, 0, {"first"}),
                        2, events, a_assertContext);
    auto failing = register_system(
        registry, make_descriptor("failing", cue::game_core::RuntimeUpdatePhase::PostUpdate, 0, {"second"}), 3, events,
        a_assertContext, {.failsStart = true});
    auto neverStarted =
        register_system(registry, make_descriptor("never", cue::game_core::RuntimeUpdatePhase::PostUpdate, 1), 4,
                        events, a_assertContext);
    if (!first || !second || !failing || !neverStarted || !registry.seal())
    {
        return false;
    }

    cue::Result<void> start = registry.start(*runtime);
    const std::vector<SystemEvent> expected = {{1, EventKind::Start},
                                               {2, EventKind::Start},
                                               {3, EventKind::Start},
                                               {2, EventKind::Stop},
                                               {1, EventKind::Stop}};
    return has_error_code(start, cue::game_core::GameCoreError::SystemStartFailed) &&
           has_root_error_code(start, cue::game_core::GameCoreError::DependencyFailed) && events == expected &&
           registry.state() == cue::game_core::RuntimeSystemRegistryState::Stopped &&
           registry.active_system_count() == 0 && shutdown_runtime(*runtime);
}

/// @brief Stop失敗時に必須先を保持し未完了Systemだけを逆順再試行することを検証する
[[nodiscard]] bool test_stop_retry_preserves_dependencies(cue::game_core::WorldIdentitySource &a_identitySource,
                                                          const cue::schema::SchemaRegistry &a_schemaRegistry,
                                                          cue::schema::TypeId a_transformTypeId,
                                                          cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::game_core::RuntimeWorld> runtime =
        create_runtime(a_identitySource, a_schemaRegistry, std::move(a_transformTypeId), a_assertContext);
    if (runtime == nullptr)
    {
        return false;
    }

    std::vector<SystemEvent> events;
    cue::game_core::RuntimeSystemRegistry registry(a_assertContext);
    auto base = register_system(registry, make_descriptor("base", cue::game_core::RuntimeUpdatePhase::PreUpdate, 0), 1,
                                events, a_assertContext);
    auto dependent =
        register_system(registry, make_descriptor("dependent", cue::game_core::RuntimeUpdatePhase::Update, 0, {"base"}),
                        2, events, a_assertContext, {.remainingStopFailures = 1});
    auto independent =
        register_system(registry, make_descriptor("independent", cue::game_core::RuntimeUpdatePhase::PostUpdate, 0), 3,
                        events, a_assertContext);
    if (!base || !dependent || !independent || !registry.seal() || !registry.start(*runtime))
    {
        return false;
    }

    events.clear();
    cue::Result<void> firstStop = registry.stop(*runtime);
    const std::vector<SystemEvent> expectedFirst = {{3, EventKind::Stop}, {2, EventKind::Stop}};
    if (!has_error_code(firstStop, cue::game_core::GameCoreError::SystemStopFailed) ||
        !has_root_error_code(firstStop, cue::game_core::GameCoreError::DependencyFailed) || events != expectedFirst ||
        registry.state() != cue::game_core::RuntimeSystemRegistryState::StopPending ||
        registry.active_system_count() != 2)
    {
        return false;
    }

    events.clear();
    cue::Result<void> secondStop = registry.stop(*runtime);
    const std::vector<SystemEvent> expectedSecond = {{2, EventKind::Stop}, {1, EventKind::Stop}};
    return secondStop && events == expectedSecond && registry.active_system_count() == 0 &&
           registry.state() == cue::game_core::RuntimeSystemRegistryState::Stopped && shutdown_runtime(*runtime);
}

/// @brief Update失敗で後続Systemを実行せずCauseを保持することを検証する
[[nodiscard]] bool test_update_failure(cue::game_core::WorldIdentitySource &a_identitySource,
                                       const cue::schema::SchemaRegistry &a_schemaRegistry,
                                       cue::schema::TypeId a_transformTypeId, cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::game_core::RuntimeWorld> runtime =
        create_runtime(a_identitySource, a_schemaRegistry, std::move(a_transformTypeId), a_assertContext);
    if (runtime == nullptr)
    {
        return false;
    }

    std::vector<SystemEvent> events;
    cue::game_core::RuntimeSystemRegistry registry(a_assertContext);
    auto first = register_system(registry, make_descriptor("first", cue::game_core::RuntimeUpdatePhase::Update, 0), 1,
                                 events, a_assertContext);
    auto failing = register_system(registry, make_descriptor("failing", cue::game_core::RuntimeUpdatePhase::Update, 1),
                                   2, events, a_assertContext, {.failsUpdate = true});
    auto neverUpdated = register_system(
        registry, make_descriptor("never", cue::game_core::RuntimeUpdatePhase::Update, 2), 3, events, a_assertContext);
    if (!first || !failing || !neverUpdated || !registry.seal() || !registry.start(*runtime))
    {
        return false;
    }

    events.clear();
    cue::Result<cue::game_core::UpdateContext> timing = create_update_context(a_assertContext);
    cue::InputState inputState;
    inputState.begin_frame({});
    if (!timing)
    {
        return false;
    }
    cue::Result<void> update = registry.update(*runtime, *timing.try_value(), inputState.snapshot());
    const std::vector<SystemEvent> expected = {{1, EventKind::Update}, {2, EventKind::Update}};
    const bool isExpected = has_error_code(update, cue::game_core::GameCoreError::SystemUpdateFailed) &&
                            has_root_error_code(update, cue::game_core::GameCoreError::DependencyFailed) &&
                            events == expected;
    return isExpected && registry.stop(*runtime) && shutdown_runtime(*runtime);
}

/// @brief Lifecycle Callback再入拒否とStart時RuntimeWorld固定を検証する
[[nodiscard]] bool test_reentry_and_runtime_world_binding(cue::game_core::WorldIdentitySource &a_identitySource,
                                                          const cue::schema::SchemaRegistry &a_schemaRegistry,
                                                          cue::schema::TypeId a_transformTypeId,
                                                          cue::AssertContext &a_assertContext)
{
    std::unique_ptr<cue::game_core::RuntimeWorld> startedRuntime =
        create_runtime(a_identitySource, a_schemaRegistry, a_transformTypeId, a_assertContext);
    std::unique_ptr<cue::game_core::RuntimeWorld> otherRuntime =
        create_runtime(a_identitySource, a_schemaRegistry, std::move(a_transformTypeId), a_assertContext);
    if (startedRuntime == nullptr || otherRuntime == nullptr)
    {
        return false;
    }

    cue::game_core::RuntimeSystemRegistry registry(a_assertContext);
    auto system = std::make_unique<ReentrantSystem>(registry, *startedRuntime);
    ReentrantSystem *observedSystem = system.get();
    if (!registry.register_system(make_descriptor("reentrant", cue::game_core::RuntimeUpdatePhase::Update, 0),
                                  std::move(system)) ||
        !registry.seal() || !registry.start(*startedRuntime))
    {
        return false;
    }

    cue::Result<cue::game_core::UpdateContext> timing = create_update_context(a_assertContext);
    cue::InputState inputState;
    inputState.begin_frame({});
    if (!timing || !registry.update(*startedRuntime, *timing.try_value(), inputState.snapshot()))
    {
        return false;
    }

    cue::Result<void> foreignUpdate = registry.update(*otherRuntime, *timing.try_value(), inputState.snapshot());
    cue::Result<void> foreignStop = registry.stop(*otherRuntime);
    const bool rejectedForeignRuntime =
        has_error_code(foreignUpdate, cue::game_core::GameCoreError::InvalidSystemRegistryState) &&
        has_error_code(foreignStop, cue::game_core::GameCoreError::InvalidSystemRegistryState) &&
        registry.state() == cue::game_core::RuntimeSystemRegistryState::Started && registry.active_system_count() == 1;

    return rejectedForeignRuntime && registry.stop(*startedRuntime) && observedSystem->rejected_all_reentry() &&
           shutdown_runtime(*startedRuntime) && shutdown_runtime(*otherRuntime);
}
} // namespace

/// @brief Runtime System Registryの順序、診断、Rollback、再試行、Safe Point接続を検証する
int main()
{
    TestFatalHandler handler;
    std::unique_ptr<cue::Logger> logger = create_logger(handler);
    cue::AssertContext assertContext(*logger, handler);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    const cue::schema::TypeId transformTypeId = make_type_id("50000000-0000-4000-8000-000000000005", assertContext);
    std::unique_ptr<cue::schema::SchemaRegistry> schemaRegistry =
        create_schema_registry(schemaIdentitySource, transformTypeId, assertContext);
    if (schemaRegistry == nullptr)
    {
        return 1;
    }

    cue::game_core::WorldIdentitySource worldIdentitySource;
    return test_deterministic_update_and_safe_point(worldIdentitySource, *schemaRegistry, transformTypeId,
                                                    assertContext) &&
                   test_registration_diagnostics(assertContext) &&
                   test_start_failure_rollback(worldIdentitySource, *schemaRegistry, transformTypeId, assertContext) &&
                   test_stop_retry_preserves_dependencies(worldIdentitySource, *schemaRegistry, transformTypeId,
                                                          assertContext) &&
                   test_update_failure(worldIdentitySource, *schemaRegistry, transformTypeId, assertContext) &&
                   test_reentry_and_runtime_world_binding(worldIdentitySource, *schemaRegistry, transformTypeId,
                                                          assertContext)
               ? 0
               : 2;
}
