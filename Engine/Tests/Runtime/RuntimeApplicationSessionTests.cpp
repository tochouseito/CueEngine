#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/CommandBuffer.h>
#include <Cue/GameCore/RuntimeSystem.h>
#include <Cue/GameCore/World.h>
#include <Cue/Input/InputEventQueue.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeApplicationSession.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>
#include <Cue/Schema/Types.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_transformTypeId = "50000000-0000-4000-8000-000000000005";
constexpr std::string_view k_sceneObjectStateTypeId = "10000000-0000-4000-8000-000000000001";
constexpr std::string_view k_sceneAssetId = "70000000-0000-4000-8000-000000000001";

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Unit Test中の通常FatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Unit Test中のEmergency FatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

class TestClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief Testごとに独立した単調時刻を0から生成する
    TestClock() noexcept = default;

    /// @brief 固定16msずつ進む再現可能な単調Sampleを返す
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(const cue::AssertContext &) noexcept override
    {
        const cue::game_core::MonotonicClockSample sample = {m_nanoseconds};
        m_nanoseconds += 16'000'000;
        return cue::Result<cue::game_core::MonotonicClockSample>::success(cue::game_core::MonotonicClockSample{sample});
    }

  private:
    std::int64_t m_nanoseconds = 0;
};

struct SystemProbe final
{
    std::vector<std::string> calls;
    std::vector<std::string> *timeline = nullptr;
    std::size_t updateCount = 0;
    std::size_t stopCount = 0;
    std::size_t entityCountOnStop = 0;
    bool sawKeyA = false;
    bool failStart = false;
    bool failUpdate = false;
    bool createEntityOnUpdate = false;
    bool enqueueStaleDestroyOnStop = false;
    std::size_t stopFailuresRemaining = 0;
};

class RecordingSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief 外部ProbeへCallback順とFailure Injection状態を記録する
    RecordingSystem(std::string a_name, SystemProbe &a_probe, const cue::AssertContext &a_assertContext) noexcept
        : m_name(std::move(a_name)), m_probe(&a_probe), m_assertContext(&a_assertContext)
    {
    }

    /// @brief Start順を記録し指定時だけ副作用なしの失敗を返す
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        record_call(".start");
        if (m_probe->failStart)
        {
            return cue::Result<void>::failure(make_test_error(101, "Injected system start failure"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Frame順とPortable Inputを記録し指定時だけ回復可能失敗を返す
    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        record_call(".update");
        ++m_probe->updateCount;
        m_probe->sawKeyA = a_context.input.was_key_pressed(cue::InputKey::A);
        if (m_probe->failUpdate)
        {
            return cue::Result<void>::failure(make_test_error(102, "Injected system update failure"));
        }
        if (m_probe->createEntityOnUpdate)
        {
            cue::Result<cue::game_core::PendingEntityId> pending = a_context.commands.create_entity();
            if (!pending)
            {
                return cue::Result<void>::failure(std::move(*pending.try_error()));
            }
        }
        return cue::Result<void>::success();
    }

    /// @brief 逆順Stopと再試行回数を記録し指定回数だけ失敗する
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &a_context) noexcept override
    {
        record_call(".stop");
        ++m_probe->stopCount;
        m_probe->entityCountOnStop = a_context.world.entity_count();
        if (m_probe->stopFailuresRemaining != 0U)
        {
            --m_probe->stopFailuresRemaining;
            return cue::Result<void>::failure(make_test_error(103, "Injected system stop failure"));
        }
        if (m_probe->enqueueStaleDestroyOnStop)
        {
            cue::Result<cue::game_core::EntityHandle> entity = a_context.world.create_entity();
            if (!entity)
            {
                return cue::Result<void>::failure(std::move(*entity.try_error()));
            }
            const cue::game_core::EntityHandle stale = *entity.try_value();
            cue::Result<void> destroyed = a_context.world.destroy_entity(stale);
            if (!destroyed)
            {
                return destroyed;
            }
            cue::Result<void> queued = a_context.commands.destroy_entity(stale);
            if (!queued)
            {
                return queued;
            }
            m_probe->enqueueStaleDestroyOnStop = false;
        }
        return cue::Result<void>::success();
    }

  private:
    /// @brief System固有履歴と複数System共通Timelineへ同じCallbackを記録する
    void record_call(std::string_view a_suffix) noexcept
    {
        std::string call = m_name + std::string(a_suffix);
        m_probe->calls.push_back(call);
        if (m_probe->timeline != nullptr)
        {
            m_probe->timeline->push_back(std::move(call));
        }
    }

    /// @brief Failure Injectionを識別可能なTest Domain Errorへ変換する
    [[nodiscard]] cue::Error make_test_error(std::int64_t a_code, std::string_view a_summary) const noexcept
    {
        cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Runtime.Test", a_code);
        return cue::Error::create(m_assertContext->fatal_handler(), std::move(code), a_summary);
    }

    std::string m_name;
    SystemProbe *m_probe;
    const cue::AssertContext *m_assertContext;
};

/// @brief 条件が偽ならRuntime Application Session Testを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::_Exit(2);
    }
}

/// @brief Resultが失敗ならRuntime Application Session Testを失敗終了する
template <typename T> void require(const cue::Result<T> &a_result) noexcept
{
    require(a_result.has_value());
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> [[nodiscard]] T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result);
    return std::move(*a_result.try_value());
}

/// @brief Canonical UUIDからTest用TypeIdを生成する
[[nodiscard]] cue::schema::TypeId make_type_id(std::string_view a_text,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse(a_text, a_assertContext));
}

/// @brief Fieldを持たないTest用Type Descriptorを生成する
[[nodiscard]] cue::schema::TypeDescriptor make_type_descriptor(std::string_view a_typeId, std::string_view a_name,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reserved;
    return take_value(
        cue::schema::create_type_descriptor(make_type_id(a_typeId, a_assertContext), a_name,
                                            take_value(cue::schema::SchemaVersion::create(1U, a_assertContext)),
                                            std::move(fields), std::move(reserved), a_assertContext));
}

/// @brief TransformとSceneObjectStateを登録したSeal済みRegistryを生成する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(make_type_descriptor(k_transformTypeId, "Cue.Core.Transform", a_assertContext)));
    require(builder.add_type(
        make_type_descriptor(k_sceneObjectStateTypeId, "Cue.Scene.SceneObjectState", a_assertContext)));
    return take_value(builder.seal());
}

/// @brief 空のAuthoring SceneからRuntime用の独立Snapshotを生成する
[[nodiscard]] cue::scene::SceneSnapshot make_snapshot(const cue::AssertContext &a_assertContext) noexcept
{
    cue::scene::SceneDocument document = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::parse(k_sceneAssetId, a_assertContext)), a_assertContext);
    return take_value(cue::scene::create_scene_snapshot(document, a_assertContext));
}

/// @brief 一Systemを指定順序でSessionへ登録する
void register_system(cue::runtime::RuntimeApplicationSession &a_session, std::string a_id, std::int32_t a_order,
                     SystemProbe &a_probe, const cue::AssertContext &a_assertContext) noexcept
{
    std::unique_ptr<RecordingSystem> system = std::make_unique<RecordingSystem>(a_id, a_probe, a_assertContext);
    cue::game_core::RuntimeSystemDescriptor descriptor = {
        std::move(a_id), cue::game_core::RuntimeUpdatePhase::Update, a_order, {}};
    require(a_session.register_system(std::move(descriptor), std::move(system)));
}

/// @brief Scene、Input、Clock、System、Safe Point、Window Close停止の最小正常経路を検証する
void test_application_lifecycle(const cue::schema::SchemaRegistry &a_registry,
                                cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                const cue::scene::SceneSnapshot &a_snapshot,
                                const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto session = take_value(cue::runtime::RuntimeApplicationSession::create(7U, clock, 100'000'000, a_assertContext));
    std::vector<std::string> timeline;
    SystemProbe first;
    SystemProbe second;
    first.timeline = &timeline;
    first.createEntityOnUpdate = true;
    second.timeline = &timeline;
    register_system(*session, "First", 10, first, a_assertContext);
    register_system(*session, "Second", 20, second, a_assertContext);
    require(session->start(a_snapshot, a_worldIdentitySource, a_registry,
                           make_type_id(k_transformTypeId, a_assertContext),
                           make_type_id(k_sceneObjectStateTypeId, a_assertContext)));
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::Running);
    require(session->generation() == 7U);
    require(session->world_id() != 0U);
    require(session->system_count() == 2U);

    require(session->input_events().push({cue::InputEventType::KeyDown, cue::InputKey::A}));
    require(session->advance_frame({}));
    require(session->next_frame_index() == 1U);
    require(first.sawKeyA && second.sawKeyA);
    require(first.updateCount == 1U && second.updateCount == 1U);

    require(session->request_stop(cue::runtime::RuntimeApplicationStopReason::WindowClosed));
    require(session->request_stop(cue::runtime::RuntimeApplicationStopReason::Requested));
    require(session->stop_reason() == cue::runtime::RuntimeApplicationStopReason::WindowClosed);
    require(session->stop());
    require(session->stop());
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::Stopped);
    require(first.stopCount == 1U && second.stopCount == 1U);
    require(first.entityCountOnStop == 1U && second.entityCountOnStop == 1U);
    require(timeline == std::vector<std::string>({"First.start", "Second.start", "First.update", "Second.update",
                                                  "Second.stop", "First.stop"}));
    require(first.calls == std::vector<std::string>({"First.start", "First.update", "First.stop"}));
    require(second.calls == std::vector<std::string>({"Second.start", "Second.update", "Second.stop"}));
    require(session->try_failure() == nullptr);
}

/// @brief System Start失敗で開始済みSystemだけが逆順停止されSessionを再利用しないことを検証する
void test_start_failure_rollback(const cue::schema::SchemaRegistry &a_registry,
                                 cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                 const cue::scene::SceneSnapshot &a_snapshot,
                                 const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto session = take_value(cue::runtime::RuntimeApplicationSession::create(8U, clock, 100'000'000, a_assertContext));
    SystemProbe first;
    SystemProbe second;
    second.failStart = true;
    register_system(*session, "First", 10, first, a_assertContext);
    register_system(*session, "Second", 20, second, a_assertContext);
    cue::Result<void> started =
        session->start(a_snapshot, a_worldIdentitySource, a_registry, make_type_id(k_transformTypeId, a_assertContext),
                       make_type_id(k_sceneObjectStateTypeId, a_assertContext));
    require(!started);
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::Stopped);
    require(first.stopCount == 1U && second.stopCount == 0U);
    require(session->try_failure() != nullptr);
    require(session->try_failure()->root_code().domain() == "Cue.Runtime.Test");
    require(session->try_failure()->root_code().value() == 101);
    require(!session->start(a_snapshot, a_worldIdentitySource, a_registry,
                            make_type_id(k_transformTypeId, a_assertContext),
                            make_type_id(k_sceneObjectStateTypeId, a_assertContext)));
}

/// @brief System Update失敗をFatalにせず診断保持と安全停止へ変換することを検証する
void test_update_failure(const cue::schema::SchemaRegistry &a_registry,
                         cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                         const cue::scene::SceneSnapshot &a_snapshot,
                         const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto session = take_value(cue::runtime::RuntimeApplicationSession::create(9U, clock, 100'000'000, a_assertContext));
    SystemProbe probe;
    probe.failUpdate = true;
    register_system(*session, "Failure", 0, probe, a_assertContext);
    require(session->start(a_snapshot, a_worldIdentitySource, a_registry,
                           make_type_id(k_transformTypeId, a_assertContext),
                           make_type_id(k_sceneObjectStateTypeId, a_assertContext)));
    require(!session->advance_frame({}));
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::StopRequested);
    require(session->stop_reason() == cue::runtime::RuntimeApplicationStopReason::RuntimeFailure);
    require(session->try_failure() != nullptr);
    require(session->try_failure()->root_code().domain() == "Cue.Runtime.Test");
    require(session->try_failure()->root_code().value() == 102);
    require(session->stop());

    auto failure = session->take_failure();
    require(failure);
    require(failure.try_value()->has_value());
    require(failure.try_value()->value().root_code().value() == 102);
    require(session->try_failure() == nullptr);
}

/// @brief Stop失敗時に依存閉包を保持し同じSystemの未完了Cleanupだけを再試行することを検証する
void test_stop_retry(const cue::schema::SchemaRegistry &a_registry,
                     cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                     const cue::scene::SceneSnapshot &a_snapshot, const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto session =
        take_value(cue::runtime::RuntimeApplicationSession::create(10U, clock, 100'000'000, a_assertContext));
    SystemProbe probe;
    probe.stopFailuresRemaining = 1U;
    register_system(*session, "Retry", 0, probe, a_assertContext);
    require(session->start(a_snapshot, a_worldIdentitySource, a_registry,
                           make_type_id(k_transformTypeId, a_assertContext),
                           make_type_id(k_sceneObjectStateTypeId, a_assertContext)));
    require(session->request_stop(cue::runtime::RuntimeApplicationStopReason::Requested));
    require(!session->stop());
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::CleanupFailed);
    require(probe.stopCount == 1U);
    require(!session->advance_frame({}));
    require(session->stop());
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::Stopped);
    require(probe.stopCount == 2U);
    require(session->try_failure() != nullptr);
    require(session->try_failure()->root_code().value() == 103);
}

/// @brief Stop Commandの個別失敗後も所有物を終了しつつstop ResultへCleanup失敗を返すことを検証する
void test_stop_command_failure(const cue::schema::SchemaRegistry &a_registry,
                               cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                               const cue::scene::SceneSnapshot &a_snapshot,
                               const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto session =
        take_value(cue::runtime::RuntimeApplicationSession::create(11U, clock, 100'000'000, a_assertContext));
    SystemProbe probe;
    probe.enqueueStaleDestroyOnStop = true;
    register_system(*session, "CommandFailure", 0, probe, a_assertContext);
    require(session->start(a_snapshot, a_worldIdentitySource, a_registry,
                           make_type_id(k_transformTypeId, a_assertContext),
                           make_type_id(k_sceneObjectStateTypeId, a_assertContext)));
    require(session->request_stop(cue::runtime::RuntimeApplicationStopReason::Requested));

    cue::Result<void> stopped = session->stop();
    require(!stopped);
    require(stopped.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::runtime::RuntimeError::ApplicationSessionCleanupFailed));
    require(session->state() == cue::runtime::RuntimeApplicationSessionState::Stopped);
    require(session->try_failure() != nullptr);
    require(session->try_failure()->code().value() ==
            static_cast<std::int64_t>(cue::runtime::RuntimeError::StructuralCommandFailed));

    auto failure = session->take_failure();
    require(failure && failure.try_value()->has_value());
    require(session->try_failure() == nullptr);
}
} // namespace

/// @brief Runtime Application SessionのHeadless Frame順、失敗、再Cleanup契約を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_registry(schemaIdentitySource, assertContext);
    cue::game_core::WorldIdentitySource worldIdentitySource;
    cue::scene::SceneSnapshot snapshot = make_snapshot(assertContext);

    test_application_lifecycle(*registry, worldIdentitySource, snapshot, assertContext);
    test_start_failure_rollback(*registry, worldIdentitySource, snapshot, assertContext);
    test_update_failure(*registry, worldIdentitySource, snapshot, assertContext);
    test_stop_retry(*registry, worldIdentitySource, snapshot, assertContext);
    test_stop_command_failure(*registry, worldIdentitySource, snapshot, assertContext);
    return 0;
}
