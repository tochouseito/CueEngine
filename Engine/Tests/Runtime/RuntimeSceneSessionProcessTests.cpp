#include "RuntimeSceneSessionInternals.h"
#include "TestSupport/RuntimeSceneSessionProbe.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Error.h>
#include <Cue/GameCore/RuntimeWorld.h>
#include <Cue/Math/Transform.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Scene/Error.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr int k_expectedExitCode = 75;
constexpr int k_invalidDiagnosticExitCode = 76;
constexpr int k_emergencyExitCode = 77;
constexpr std::string_view k_transformTypeId = "50000000-0000-4000-8000-000000000005";
constexpr std::string_view k_sceneObjectStateTypeId = "10000000-0000-4000-8000-000000000001";

enum class ProcessMode
{
    OuterEndFailure,
    ReportEndFailure,
    WrongThread,
    LiveDestructor
};

struct ProcessState final
{
    ProcessMode mode = ProcessMode::OuterEndFailure;
    bool hasExpectedDiagnostic = false;
    bool didWrite = false;
    bool didFlush = false;
    const cue::game_core::RuntimeWorld *observedWorld = nullptr;
    int endCallCount = 0;
};

class ProcessFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Fatal診断のWriteとFlushが完了した場合だけ期待Exit Codeを返す
    explicit ProcessFatalHandler(ProcessState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 通常Fatalの診断完全性をProcess Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        const bool isValid = m_state->didWrite && m_state->didFlush && m_state->hasExpectedDiagnostic &&
                             m_state->observedWorld != nullptr &&
                             m_state->observedWorld->state() == cue::game_core::RuntimeWorldState::Running &&
                             m_state->endCallCount == 1;
        std::_Exit(isValid ? k_expectedExitCode : k_invalidDiagnosticExitCode);
    }

    /// @brief Loggerを通らないEmergency終端を別Exit Codeへ分離する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(k_emergencyExitCode);
    }

  private:
    ProcessState *m_state;
};

class ProgrammerErrorFatalHandler final : public cue::FatalHandler
{
  public:
    struct State final
    {
        bool isArmed = false;
        bool hasExpectedDiagnostic = false;
        std::string_view expectedMessage;
    };

    /// @brief Programmer Error検証状態を参照してHandlerを構築する
    explicit ProgrammerErrorFatalHandler(State &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief Assert経由のProgrammer Errorを期待Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        const bool isExpected = m_state->isArmed && m_state->hasExpectedDiagnostic;
        std::_Exit(isExpected ? k_expectedExitCode : k_invalidDiagnosticExitCode);
    }

    /// @brief 全構成の明示Programmer Errorを期待Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view a_message) noexcept override
    {
        const bool isExpected = m_state->isArmed && a_message == m_state->expectedMessage;
        std::_Exit(isExpected ? k_expectedExitCode : k_invalidDiagnosticExitCode);
    }

  private:
    State *m_state;
};

class ProgrammerErrorSink final : public cue::LogSink
{
  public:
    /// @brief Programmer Error検証状態を参照してSinkを構築する
    explicit ProgrammerErrorSink(ProgrammerErrorFatalHandler::State &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 対象操作が発行した期待Fatal診断だけを記録する
    [[nodiscard]] bool write(const cue::LogRecord &a_record) override
    {
        if (m_state->isArmed && a_record.level() == cue::LogLevel::Fatal &&
            a_record.message() == m_state->expectedMessage)
        {
            m_state->hasExpectedDiagnostic = true;
        }
        return true;
    }

    /// @brief 同期Process Testでは保留出力がないため成功を返す
    [[nodiscard]] bool flush() override
    {
        return true;
    }

  private:
    ProgrammerErrorFatalHandler::State *m_state;
};

class InspectingSink final : public cue::LogSink
{
  public:
    /// @brief 期待する失敗ModeのFatal Error構造を検証するSinkを構築する
    explicit InspectingSink(ProcessState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief Runtime Fatal Category、CauseまたはReport順序を同期検証する
    [[nodiscard]] bool write(const cue::LogRecord &a_record) override
    {
        m_state->didWrite = true;
        const cue::Error *error = a_record.try_error();
        if (a_record.level() != cue::LogLevel::Fatal || error == nullptr || error->code().domain() != "Cue.Runtime" ||
            error->code().value() != static_cast<std::int64_t>(cue::runtime::RuntimeError::SceneCleanupFailed))
        {
            return true;
        }

        if (m_state->mode == ProcessMode::OuterEndFailure)
        {
            m_state->hasExpectedDiagnostic =
                error->root_code().domain() == "Cue.Scene" &&
                error->root_code().value() == static_cast<std::int64_t>(cue::scene::SceneError::RuntimeWorldMismatch);
            return true;
        }

        const std::span<const cue::ErrorContext> contexts = error->contexts();
        m_state->hasExpectedDiagnostic = error->causes().empty() && contexts.size() >= 6U &&
                                         contexts[1].message() == "First injected scene entity cleanup failure" &&
                                         contexts[4].message() == "Second injected scene entity cleanup failure";
        return true;
    }

    /// @brief Fatal RecordのFlush完了をProcess状態へ記録する
    [[nodiscard]] bool flush() override
    {
        m_state->didFlush = true;
        return true;
    }

  private:
    ProcessState *m_state;
};

/// @brief 条件が偽ならFatal注入前の準備失敗としてProcessを終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::_Exit(2);
    }
}

/// @brief Resultが失敗ならFatal注入前の準備失敗としてProcessを終了する
template <typename T> void require(const cue::Result<T> &a_result) noexcept
{
    require(a_result.has_value());
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> [[nodiscard]] T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result.has_value());
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

/// @brief Process Testで使うTransformとSceneObjectStateのSchemaを固定する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(make_type_descriptor(k_transformTypeId, "Cue.Core.Transform", a_assertContext)));
    require(builder.add_type(
        make_type_descriptor(k_sceneObjectStateTypeId, "Cue.Scene.SceneObjectState", a_assertContext)));
    return take_value(builder.seal());
}

/// @brief 二EntityのReport Failureを識別できるScene Snapshotを生成する
[[nodiscard]] cue::scene::SceneSnapshot make_snapshot(cue::scene::ObjectId &a_first, cue::scene::ObjectId &a_second,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    cue::scene::SceneDocument document = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::parse("80000000-0000-4000-8000-000000000001", a_assertContext)),
        a_assertContext);
    require(document.add_object(a_first, "First", true, std::nullopt, cue::math::Transform{}));
    require(document.add_object(a_second, "Second", true, std::nullopt, cue::math::Transform{}));
    return take_value(cue::scene::create_scene_snapshot(document, a_assertContext));
}

class InjectedEndOperation final : public cue::runtime::details::SceneEndOperation
{
  public:
    /// @brief 外側Result失敗または順序付きReport失敗を生成するOperationを構築する
    InjectedEndOperation(ProcessState &a_state, cue::scene::ObjectId a_first, cue::scene::ObjectId a_second) noexcept
        : m_state(&a_state), m_first(std::move(a_first)), m_second(std::move(a_second))
    {
    }

    /// @brief Production Normalizerへ指定したRaw Scene終了失敗を注入する
    [[nodiscard]] cue::Result<cue::scene::SceneInstanceEndReport> end(
        cue::scene::SceneInstance &a_instance, cue::game_core::RuntimeWorld &a_runtimeWorld,
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        ++m_state->endCallCount;
        m_state->observedWorld = &a_runtimeWorld;
        if (m_state->mode == ProcessMode::OuterEndFailure)
        {
            return cue::Result<cue::scene::SceneInstanceEndReport>::failure(cue::scene::make_scene_error(
                a_assertContext, cue::scene::SceneError::RuntimeWorldMismatch, "Injected outer scene end failure"));
        }

        const cue::game_core::EntityHandle *firstEntity = a_instance.find_entity(m_first);
        const cue::game_core::EntityHandle *secondEntity = a_instance.find_entity(m_second);
        require(firstEntity != nullptr && secondEntity != nullptr);
        std::vector<cue::scene::SceneInstanceEndFailure> failures;
        failures.emplace_back(*firstEntity, cue::game_core::make_game_core_error(
                                                a_assertContext, cue::game_core::GameCoreError::InvalidEntity,
                                                "First injected scene entity cleanup failure"));
        failures.emplace_back(*secondEntity, cue::game_core::make_game_core_error(
                                                 a_assertContext, cue::game_core::GameCoreError::InvalidEntity,
                                                 "Second injected scene entity cleanup failure"));
        return cue::Result<cue::scene::SceneInstanceEndReport>::success(
            cue::scene::SceneInstanceEndReport(std::move(failures)));
    }

  private:
    ProcessState *m_state;
    cue::scene::ObjectId m_first;
    cue::scene::ObjectId m_second;
};

/// @brief 指定ModeのScene End失敗を共通Normalizerへ通してFatal終端する
[[noreturn]] void run_failure_mode(ProcessMode a_mode) noexcept
{
    ProcessState state{a_mode};
    ProcessFatalHandler fatalHandler(state);
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    sinks.push_back(std::make_unique<InspectingSink>(state));
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_registry(schemaIdentitySource, assertContext);
    cue::game_core::WorldIdentitySource worldIdentitySource;
    cue::scene::ObjectId first =
        take_value(cue::scene::ObjectId::parse("80000000-0000-4000-8000-000000000002", assertContext));
    cue::scene::ObjectId second =
        take_value(cue::scene::ObjectId::parse("80000000-0000-4000-8000-000000000003", assertContext));
    cue::scene::SceneSnapshot snapshot = make_snapshot(first, second, assertContext);
    InjectedEndOperation operation(state, first, second);
    auto started = cue::runtime::test_support::RuntimeSceneSessionProbe::start(
        snapshot, worldIdentitySource, *registry, make_type_id(k_transformTypeId, assertContext),
        make_type_id(k_sceneObjectStateTypeId, assertContext), assertContext, operation);
    require(started.has_value());
    static_cast<void>((*started.try_value())->end());
    std::_Exit(3);
}

/// @brief RuntimeSceneSession自身のOwner Threadまたはlive Destructor違反を全構成でFatal終端する
[[noreturn]] void run_programmer_error_mode(ProcessMode a_mode) noexcept
{
    ProgrammerErrorFatalHandler::State programmerErrorState;
    ProgrammerErrorFatalHandler fatalHandler(programmerErrorState);
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    sinks.push_back(std::make_unique<ProgrammerErrorSink>(programmerErrorState));
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_registry(schemaIdentitySource, assertContext);
    cue::game_core::WorldIdentitySource worldIdentitySource;
    cue::scene::ObjectId first =
        take_value(cue::scene::ObjectId::parse("80000000-0000-4000-8000-000000000002", assertContext));
    cue::scene::ObjectId second =
        take_value(cue::scene::ObjectId::parse("80000000-0000-4000-8000-000000000003", assertContext));
    cue::scene::SceneSnapshot snapshot = make_snapshot(first, second, assertContext);
    auto session = cue::runtime::RuntimeSceneSession::start(
        snapshot, worldIdentitySource, *registry, make_type_id(k_transformTypeId, assertContext),
        make_type_id(k_sceneObjectStateTypeId, assertContext), assertContext);
    require(session.has_value());

    if (a_mode == ProcessMode::WrongThread)
    {
        programmerErrorState.expectedMessage = "Cue.Runtime scene session API requires its owner thread";
        programmerErrorState.isArmed = true;
        /// @brief Session Owner以外のThreadから状態取得してThread契約違反を発生させる
        std::thread foreignThread([&session]() noexcept { static_cast<void>((*session.try_value())->state()); });
        foreignThread.join();
        std::_Exit(3);
    }

    programmerErrorState.expectedMessage =
        "Cue.Runtime scene session destruction requires completed scene and world cleanup";
    programmerErrorState.isArmed = true;
    session.try_value()->reset();
    std::_Exit(3);
}
} // namespace

/// @brief Scene終了失敗のCauseまたはFIFO Report診断を子Processで検証する
int main(int a_argumentCount, char **a_arguments)
{
    if (a_argumentCount != 2)
    {
        return 1;
    }
    const std::string_view mode(a_arguments[1]);
    if (mode == "OuterEndFailure")
    {
        run_failure_mode(ProcessMode::OuterEndFailure);
    }
    if (mode == "ReportEndFailure")
    {
        run_failure_mode(ProcessMode::ReportEndFailure);
    }
    if (mode == "WrongThread")
    {
        run_programmer_error_mode(ProcessMode::WrongThread);
    }
    if (mode == "LiveDestructor")
    {
        run_programmer_error_mode(ProcessMode::LiveDestructor);
    }
    return 1;
}
