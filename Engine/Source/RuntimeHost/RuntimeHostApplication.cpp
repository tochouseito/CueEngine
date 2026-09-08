#include "RuntimeHostApplication.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/World.h>
#include <Cue/Input/Windows/WindowsInputMessageSink.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>
#include <Cue/Schema/Types.h>

#include <exception>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_transformTypeId = "50000000-0000-4000-8000-000000000005";
constexpr std::string_view k_sceneObjectStateTypeId = "10000000-0000-4000-8000-000000000001";
constexpr std::string_view k_startupSceneAssetId = "70000000-0000-4000-8000-000000000001";
constexpr std::uint64_t k_sessionGeneration = 1U;
constexpr std::int64_t k_maxDeltaNanoseconds = 100'000'000;

/// @brief Canonical UUIDをStandalone Runtime用Schema Typeへ変換する
[[nodiscard]] cue::Result<cue::schema::TypeId> parse_type_id(std::string_view a_text,
                                                             const cue::AssertContext &a_assertContext) noexcept
{
    return cue::schema::TypeId::parse(a_text, a_assertContext);
}

/// @brief Fieldを持たないM14 Core Type Descriptorを生成する
[[nodiscard]] cue::Result<cue::schema::TypeDescriptor> make_type_descriptor(
    std::string_view a_typeId, std::string_view a_name, const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::schema::TypeId> typeId = parse_type_id(a_typeId, a_assertContext);
    if (!typeId)
    {
        return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*typeId.try_error()));
    }
    cue::Result<cue::schema::SchemaVersion> version = cue::schema::SchemaVersion::create(1U, a_assertContext);
    if (!version)
    {
        return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*version.try_error()));
    }

    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reserved;
    return cue::schema::create_type_descriptor(std::move(*typeId.try_value()), a_name, std::move(*version.try_value()),
                                               std::move(fields), std::move(reserved), a_assertContext);
}

/// @brief Standalone RuntimeのTransformとSceneObjectStateを持つ不変Registryを生成する
[[nodiscard]] cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>> make_schema_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    cue::Result<cue::schema::TypeDescriptor> transform =
        make_type_descriptor(k_transformTypeId, "Cue.Core.Transform", a_assertContext);
    if (!transform)
    {
        return cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>>::failure(std::move(*transform.try_error()));
    }
    cue::Result<void> addedTransform = builder.add_type(std::move(*transform.try_value()));
    if (!addedTransform)
    {
        return cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>>::failure(
            std::move(*addedTransform.try_error()));
    }

    cue::Result<cue::schema::TypeDescriptor> sceneObjectState =
        make_type_descriptor(k_sceneObjectStateTypeId, "Cue.Scene.SceneObjectState", a_assertContext);
    if (!sceneObjectState)
    {
        return cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>>::failure(
            std::move(*sceneObjectState.try_error()));
    }
    cue::Result<void> addedSceneObjectState = builder.add_type(std::move(*sceneObjectState.try_value()));
    if (!addedSceneObjectState)
    {
        return cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>>::failure(
            std::move(*addedSceneObjectState.try_error()));
    }
    return builder.seal();
}

/// @brief M16のPackage発見まで使用する固定空Sceneを独立Snapshotへ変換する
[[nodiscard]] cue::Result<cue::scene::SceneSnapshot> make_startup_scene(
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::scene::SceneAssetId> sceneId =
        cue::scene::SceneAssetId::parse(k_startupSceneAssetId, a_assertContext);
    if (!sceneId)
    {
        return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*sceneId.try_error()));
    }
    cue::scene::SceneDocument document =
        cue::scene::SceneDocument::create(std::move(*sceneId.try_value()), a_assertContext);
    return cue::scene::create_scene_snapshot(document, a_assertContext);
}
} // namespace

namespace cue::runtime_host
{
class RuntimeHostApplication::State final
{
  public:
    /// @brief Windowと診断Contextを非所有参照として固定した空のHost Compositionを構築する
    State(Window &a_window, const AssertContext &a_assertContext) noexcept
        : window(&a_window), assertContext(&a_assertContext)
    {
    }

    Window *window;
    const AssertContext *assertContext;
    schema::SchemaRegistryIdentitySource schemaIdentitySource;
    game_core::WorldIdentitySource worldIdentitySource;
    game_core::SteadyMonotonicClock clock;
    std::unique_ptr<schema::SchemaRegistry> schemaRegistry;
    std::unique_ptr<runtime::RuntimeApplicationSession> session;
    std::unique_ptr<WindowsInputMessageSink> inputSink;
    bool isInputSinkAttached = false;
};

Result<std::unique_ptr<RuntimeHostApplication>> RuntimeHostApplication::start(
    Window &a_window, const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::unique_ptr<State> state = std::make_unique<State>(a_window, a_assertContext);
        Result<std::unique_ptr<schema::SchemaRegistry>> registry =
            make_schema_registry(state->schemaIdentitySource, a_assertContext);
        if (!registry)
        {
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*registry.try_error()));
        }
        state->schemaRegistry = std::move(*registry.try_value());

        Result<std::unique_ptr<runtime::RuntimeApplicationSession>> session =
            runtime::RuntimeApplicationSession::create(k_sessionGeneration, state->clock, k_maxDeltaNanoseconds,
                                                       a_assertContext);
        if (!session)
        {
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*session.try_error()));
        }
        state->session = std::move(*session.try_value());
        state->inputSink = std::make_unique<WindowsInputMessageSink>(state->session->input_events());
        std::unique_ptr<RuntimeHostApplication> application =
            std::make_unique<RuntimeHostApplication>(ConstructionKey{}, std::move(state));

        Result<void> attached =
            attach_windows_message_sink(a_window, *application->m_state->inputSink, a_assertContext);
        if (!attached)
        {
            Result<void> stopped = application->m_state->session->stop();
            if (!stopped)
            {
                report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                             "Runtime Host could not cleanup an unattached Runtime Application",
                             std::move(*stopped.try_error()));
            }
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*attached.try_error()));
        }
        application->m_state->isInputSinkAttached = true;

        Result<scene::SceneSnapshot> snapshot = make_startup_scene(a_assertContext);
        if (!snapshot)
        {
            report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                         "Runtime Host could not create its fixed startup Scene after attaching Input",
                         std::move(*snapshot.try_error()));
        }
        Result<schema::TypeId> transformTypeId = parse_type_id(k_transformTypeId, a_assertContext);
        Result<schema::TypeId> sceneObjectStateTypeId = parse_type_id(k_sceneObjectStateTypeId, a_assertContext);
        if (!transformTypeId || !sceneObjectStateTypeId)
        {
            report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                         "Runtime Host fixed Schema Type parsing failed after attaching Input",
                         transformTypeId ? std::move(*sceneObjectStateTypeId.try_error())
                                         : std::move(*transformTypeId.try_error()));
        }

        Result<void> started = application->m_state->session->start(
            *snapshot.try_value(), application->m_state->worldIdentitySource, *application->m_state->schemaRegistry,
            std::move(*transformTypeId.try_value()), std::move(*sceneObjectStateTypeId.try_value()));
        if (!started)
        {
            if (application->m_state->session->state() == runtime::RuntimeApplicationSessionState::CleanupFailed)
            {
                Result<void> cleanup = application->m_state->session->stop();
                if (!cleanup)
                {
                    report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                                 "Runtime Host could not cleanup a failed Runtime Application start",
                                 std::move(*cleanup.try_error()));
                }
            }

            Result<std::optional<Error>> retained = application->m_state->session->take_failure();
            Error primary = retained && retained.try_value()->has_value() ? std::move(retained.try_value()->value())
                                                                          : std::move(*started.try_error());
            Result<void> detached =
                detach_windows_message_sink(a_window, *application->m_state->inputSink, a_assertContext);
            if (!detached)
            {
                report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                             "Runtime Host could not detach Input after failed Runtime Application start",
                             std::move(*detached.try_error()));
            }
            application->m_state->isInputSinkAttached = false;
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(primary));
        }

        return Result<std::unique_ptr<RuntimeHostApplication>>::success(std::move(application));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Runtime Host application composition allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Runtime Host application composition caught an unexpected exception");
    }

    std::terminate();
}

RuntimeHostApplication::RuntimeHostApplication(ConstructionKey, std::unique_ptr<State> a_state) noexcept
    : m_state(std::move(a_state))
{
}

RuntimeHostApplication::~RuntimeHostApplication() noexcept
{
    const bool isComplete = is_cleanup_complete();
    CUE_ASSERT(*m_state->assertContext, isComplete,
               "Runtime Host application destruction requires stopped Runtime and detached Input Sink");
    if (!isComplete)
    {
        m_state->assertContext->fatal_handler().terminate(
            "Runtime Host application destruction requires stopped Runtime and detached Input Sink");
    }
}

Result<void> RuntimeHostApplication::advance_frame() noexcept
{
    return m_state->session->advance_frame({});
}

Result<void> RuntimeHostApplication::stop(runtime::RuntimeApplicationStopReason a_reason) noexcept
{
    if (is_cleanup_complete())
    {
        return Result<void>::success();
    }

    if (m_state->session->state() == runtime::RuntimeApplicationSessionState::Running)
    {
        Result<void> requested = m_state->session->request_stop(a_reason);
        if (!requested)
        {
            return requested;
        }
    }

    Result<void> stopped = m_state->session->stop();
    if (!stopped)
    {
        return stopped;
    }

    Result<void> detached = detach_windows_message_sink(*m_state->window, *m_state->inputSink, *m_state->assertContext);
    if (!detached)
    {
        return detached;
    }
    m_state->isInputSinkAttached = false;

    Result<std::optional<Error>> retained = m_state->session->take_failure();
    if (!retained)
    {
        return Result<void>::failure(std::move(*retained.try_error()));
    }
    if (retained.try_value()->has_value())
    {
        return Result<void>::failure(std::move(retained.try_value()->value()));
    }
    return Result<void>::success();
}

bool RuntimeHostApplication::is_cleanup_complete() const noexcept
{
    return !m_state->isInputSinkAttached &&
           m_state->session->state() == runtime::RuntimeApplicationSessionState::Stopped;
}

std::uint64_t RuntimeHostApplication::generation() const noexcept
{
    return m_state->session->generation();
}

std::uint64_t RuntimeHostApplication::world_id() const noexcept
{
    return m_state->session->world_id();
}

std::uint64_t RuntimeHostApplication::frame_count() const noexcept
{
    return m_state->session->next_frame_index();
}

runtime::RuntimeApplicationStopReason RuntimeHostApplication::stop_reason() const noexcept
{
    return m_state->session->stop_reason();
}
} // namespace cue::runtime_host
