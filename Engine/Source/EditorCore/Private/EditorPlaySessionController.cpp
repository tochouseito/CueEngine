#include <Cue/EditorCore/EditorPlaySessionController.h>

#include <Cue/EditorCore/EditorController.h>
#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/World.h>
#include <Cue/Input/InputEventQueue.h>
#include <Cue/Runtime/RuntimeApplicationSession.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Registry.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace cue::editor_core
{
Result<std::unique_ptr<EditorPlaySessionController>> EditorPlaySessionController::create(
    const ProjectWorkspaceSession &a_workspaceSession, game_core::WorldIdentitySource &a_worldIdentitySource,
    game_core::MonotonicClock &a_clock, const schema::SchemaRegistry &a_schemaRegistry,
    std::span<const runtime::RuntimeSystemFactory *const> a_systemFactories, schema::TypeId a_transformTypeId,
    schema::TypeId a_sceneObjectStateTypeId, std::uint64_t a_firstGeneration, std::int64_t a_maxDeltaNanoseconds,
    const AssertContext &a_assertContext) noexcept
{
    if (a_firstGeneration == 0U || a_maxDeltaNanoseconds <= 0)
    {
        return Result<std::unique_ptr<EditorPlaySessionController>>::failure(make_editor_core_error(
            a_assertContext, EditorCoreError::InvalidPlayConfiguration,
            "Editor Play Session configuration requires non-zero generation and positive delta"));
    }
    if (std::find(a_systemFactories.begin(), a_systemFactories.end(), nullptr) != a_systemFactories.end())
    {
        return Result<std::unique_ptr<EditorPlaySessionController>>::failure(
            make_editor_core_error(a_assertContext, EditorCoreError::InvalidPlayConfiguration,
                                   "Editor Play Session configuration contains a null Runtime System Factory"));
    }

    try
    {
        std::vector<const runtime::RuntimeSystemFactory *> systemFactories(a_systemFactories.begin(),
                                                                           a_systemFactories.end());
        return Result<std::unique_ptr<EditorPlaySessionController>>::success(
            std::make_unique<EditorPlaySessionController>(
                ConstructionKey{}, a_workspaceSession, a_worldIdentitySource, a_clock, a_schemaRegistry,
                std::move(systemFactories), std::move(a_transformTypeId), std::move(a_sceneObjectStateTypeId),
                a_firstGeneration, a_maxDeltaNanoseconds, a_assertContext));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore Play Session Controller allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Cue.EditorCore Play Session Controller caught an unexpected exception");
    }

    std::terminate();
}

EditorPlaySessionController::EditorPlaySessionController(
    ConstructionKey, const ProjectWorkspaceSession &a_workspaceSession,
    game_core::WorldIdentitySource &a_worldIdentitySource, game_core::MonotonicClock &a_clock,
    const schema::SchemaRegistry &a_schemaRegistry,
    std::vector<const runtime::RuntimeSystemFactory *> a_systemFactories, schema::TypeId a_transformTypeId,
    schema::TypeId a_sceneObjectStateTypeId, std::uint64_t a_firstGeneration, std::int64_t a_maxDeltaNanoseconds,
    const AssertContext &a_assertContext) noexcept
    : m_workspaceSession(&a_workspaceSession), m_worldIdentitySource(&a_worldIdentitySource), m_clock(&a_clock),
      m_schemaRegistry(&a_schemaRegistry), m_assertContext(&a_assertContext), m_ownerThread(std::this_thread::get_id()),
      m_systemFactories(std::move(a_systemFactories)), m_transformTypeId(std::move(a_transformTypeId)),
      m_sceneObjectStateTypeId(std::move(a_sceneObjectStateTypeId)), m_maxDeltaNanoseconds(a_maxDeltaNanoseconds),
      m_nextGeneration(a_firstGeneration)
{
}

EditorPlaySessionController::~EditorPlaySessionController() noexcept
{
    assert_owner_thread();
    const bool hasLiveSession = m_session != nullptr;
    CUE_ASSERT(*m_assertContext, !hasLiveSession,
               "Cue.EditorCore Play Session Controller destruction requires completed Runtime cleanup");
    if (hasLiveSession)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.EditorCore Play Session Controller destruction requires completed Runtime cleanup");
    }
}

Result<void> EditorPlaySessionController::start(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    if (m_session != nullptr)
    {
        return Result<void>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::InvalidPlayState,
            "Editor Play Session cannot start while another Runtime Session is active", a_documentId.value()));
    }

    const EditorDocument *document = m_workspaceSession->find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor Play source document was not found",
                                                                a_documentId.value()));
    }

    Result<scene::SceneSnapshot> sceneSnapshot =
        scene::create_scene_snapshot(document->scene_document(), *m_assertContext);
    if (!sceneSnapshot)
    {
        Error error = std::move(*sceneSnapshot.try_error());
        add_document_context(error, a_documentId);
        return Result<void>::failure(std::move(error));
    }

    Result<std::uint64_t> generation = issue_generation();
    if (!generation)
    {
        Error error = std::move(*generation.try_error());
        add_document_context(error, a_documentId);
        return Result<void>::failure(std::move(error));
    }

    Result<std::unique_ptr<runtime::RuntimeApplicationSession>> created = runtime::RuntimeApplicationSession::create(
        *generation.try_value(), *m_clock, m_maxDeltaNanoseconds, *m_assertContext);
    if (!created)
    {
        Error error = std::move(*created.try_error());
        add_document_context(error, a_documentId);
        return Result<void>::failure(std::move(error));
    }

    std::unique_ptr<runtime::RuntimeApplicationSession> candidate = std::move(*created.try_value());
    m_snapshot = {EditorPlaySessionState::Stopped, a_documentId, candidate->generation(), 0U, 0U, false};
    for (const runtime::RuntimeSystemFactory *factory : m_systemFactories)
    {
        Result<runtime::RuntimeSystemRegistration> system = factory->create_system(*m_assertContext);
        if (!system)
        {
            Error error = std::move(*system.try_error());
            m_snapshot.hasFailure = true;
            add_document_context(error, a_documentId);
            return Result<void>::failure(std::move(error));
        }
        Result<void> registered = candidate->register_system(std::move(system.try_value()->descriptor),
                                                             std::move(system.try_value()->system));
        if (!registered)
        {
            Error error = std::move(*registered.try_error());
            m_snapshot.hasFailure = true;
            add_document_context(error, a_documentId);
            return Result<void>::failure(std::move(error));
        }
    }
    Result<void> started = candidate->start(*sceneSnapshot.try_value(), *m_worldIdentitySource, *m_schemaRegistry,
                                            m_transformTypeId, m_sceneObjectStateTypeId);
    if (!started)
    {
        m_snapshot.worldId = candidate->world_id();
        m_snapshot.frameCount = candidate->next_frame_index();
        m_snapshot.hasFailure = candidate->try_failure() != nullptr;
        if (candidate->state() == runtime::RuntimeApplicationSessionState::CleanupFailed)
        {
            m_session = std::move(candidate);
            m_snapshot.state = EditorPlaySessionState::CleanupFailed;
            Error error = std::move(*started.try_error());
            add_document_context(error, a_documentId);
            return Result<void>::failure(std::move(error));
        }
        if (candidate->state() != runtime::RuntimeApplicationSessionState::Stopped)
        {
            m_assertContext->fatal_handler().terminate(
                "Cue.EditorCore Play start failure did not reach a cleanup terminal state");
        }

        Result<std::optional<Error>> retained = candidate->take_failure();
        Error error = retained && retained.try_value()->has_value() ? std::move(retained.try_value()->value())
                                                                    : std::move(*started.try_error());
        m_snapshot.state = EditorPlaySessionState::Stopped;
        m_snapshot.hasFailure = true;
        add_document_context(error, a_documentId);
        return Result<void>::failure(std::move(error));
    }

    m_session = std::move(candidate);
    synchronize_snapshot();
    return Result<void>::success();
}

Result<bool> EditorPlaySessionController::push_input_event(InputEvent a_event) noexcept
{
    assert_owner_thread();
    if (m_session == nullptr || m_session->state() != runtime::RuntimeApplicationSessionState::Running)
    {
        return Result<bool>::failure(make_editor_core_error(*m_assertContext, EditorCoreError::InvalidPlayState,
                                                            "Editor Play input requires a running Runtime Session"));
    }

    return Result<bool>::success(m_session->input_events().push(a_event));
}

Result<void> EditorPlaySessionController::advance_frame(InputCapture a_capture) noexcept
{
    assert_owner_thread();
    if (m_session == nullptr || m_session->state() != runtime::RuntimeApplicationSessionState::Running)
    {
        return Result<void>::failure(make_editor_core_error(*m_assertContext, EditorCoreError::InvalidPlayState,
                                                            "Editor Play frame requires a running Runtime Session"));
    }

    Result<void> advanced = m_session->advance_frame(a_capture);
    synchronize_snapshot();
    if (!advanced)
    {
        Error error = std::move(*advanced.try_error());
        if (m_snapshot.documentId.has_value())
        {
            add_document_context(error, m_snapshot.documentId.value());
        }
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

Result<void> EditorPlaySessionController::request_stop() noexcept
{
    assert_owner_thread();
    if (m_session == nullptr)
    {
        return Result<void>::failure(
            make_editor_core_error(*m_assertContext, EditorCoreError::InvalidPlayState,
                                   "Editor Play stop request requires an active Runtime Session"));
    }

    Result<void> requested = m_session->request_stop(runtime::RuntimeApplicationStopReason::Requested);
    synchronize_snapshot();
    if (!requested)
    {
        Error error = std::move(*requested.try_error());
        if (m_snapshot.documentId.has_value())
        {
            add_document_context(error, m_snapshot.documentId.value());
        }
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

Result<void> EditorPlaySessionController::stop() noexcept
{
    assert_owner_thread();
    if (m_session == nullptr)
    {
        return Result<void>::success();
    }

    Result<void> stopped = m_session->stop();
    synchronize_snapshot();
    if (!stopped && m_session->state() != runtime::RuntimeApplicationSessionState::Stopped)
    {
        Error error = std::move(*stopped.try_error());
        if (m_snapshot.documentId.has_value())
        {
            add_document_context(error, m_snapshot.documentId.value());
        }
        return Result<void>::failure(std::move(error));
    }

    std::optional<Error> completedStopFailure;
    if (!stopped)
    {
        completedStopFailure.emplace(std::move(*stopped.try_error()));
    }
    Result<std::optional<Error>> retained = m_session->take_failure();
    m_snapshot.hasFailure = completedStopFailure.has_value() || (retained && retained.try_value()->has_value());
    m_snapshot.state = EditorPlaySessionState::Stopped;
    m_session.reset();

    if (!retained)
    {
        Error error = std::move(*retained.try_error());
        if (m_snapshot.documentId.has_value())
        {
            add_document_context(error, m_snapshot.documentId.value());
        }
        return Result<void>::failure(std::move(error));
    }
    if (retained.try_value()->has_value())
    {
        Error error = std::move(retained.try_value()->value());
        if (m_snapshot.documentId.has_value())
        {
            add_document_context(error, m_snapshot.documentId.value());
        }
        return Result<void>::failure(std::move(error));
    }
    if (completedStopFailure.has_value())
    {
        Error error = std::move(completedStopFailure.value());
        if (m_snapshot.documentId.has_value())
        {
            add_document_context(error, m_snapshot.documentId.value());
        }
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

EditorPlaySessionSnapshot EditorPlaySessionController::state_snapshot() const noexcept
{
    assert_owner_thread();
    return m_snapshot;
}

Result<std::uint64_t> EditorPlaySessionController::issue_generation() noexcept
{
    if (m_isGenerationExhausted)
    {
        return Result<std::uint64_t>::failure(
            make_editor_core_error(*m_assertContext, EditorCoreError::PlayGenerationExhausted,
                                   "Editor Play Session generation range is exhausted"));
    }

    std::uint64_t generation = m_nextGeneration;
    if (m_nextGeneration == std::numeric_limits<std::uint64_t>::max())
    {
        m_isGenerationExhausted = true;
    }
    else
    {
        ++m_nextGeneration;
    }
    return Result<std::uint64_t>::success(std::move(generation));
}

void EditorPlaySessionController::synchronize_snapshot() noexcept
{
    if (m_session == nullptr)
    {
        return;
    }

    m_snapshot.generation = m_session->generation();
    m_snapshot.worldId = m_session->world_id();
    m_snapshot.frameCount = m_session->next_frame_index();
    m_snapshot.hasFailure = m_session->try_failure() != nullptr;
    switch (m_session->state())
    {
    case runtime::RuntimeApplicationSessionState::Running:
        m_snapshot.state = EditorPlaySessionState::Running;
        return;
    case runtime::RuntimeApplicationSessionState::StopRequested:
        m_snapshot.state = EditorPlaySessionState::StopRequested;
        return;
    case runtime::RuntimeApplicationSessionState::CleanupFailed:
        m_snapshot.state = EditorPlaySessionState::CleanupFailed;
        return;
    case runtime::RuntimeApplicationSessionState::Stopped:
        m_snapshot.state = EditorPlaySessionState::Stopped;
        return;
    case runtime::RuntimeApplicationSessionState::Constructed:
    case runtime::RuntimeApplicationSessionState::Starting:
    case runtime::RuntimeApplicationSessionState::RollingBack:
    case runtime::RuntimeApplicationSessionState::Stopping:
        m_assertContext->fatal_handler().terminate(
            "Cue.EditorCore Play Session exposed a synchronous transient Runtime state");
    }

    std::terminate();
}

void EditorPlaySessionController::add_document_context(Error &a_error, EditorDocumentId a_documentId) const noexcept
{
    constexpr std::string_view prefix = "EditorPlayDocumentId=";
    std::array<char, prefix.size() + 20U> context{};
    std::copy(prefix.begin(), prefix.end(), context.begin());
    const auto converted =
        std::to_chars(context.data() + prefix.size(), context.data() + context.size(), a_documentId.value());
    if (converted.ec != std::errc{})
    {
        m_assertContext->fatal_handler().terminate("Cue.EditorCore Play document identity formatting failed");
    }
    a_error.add_context(m_assertContext->fatal_handler(),
                        std::string_view(context.data(), static_cast<std::size_t>(converted.ptr - context.data())));
}

void EditorPlaySessionController::assert_owner_thread() const noexcept
{
    const bool isOwner = std::this_thread::get_id() == m_ownerThread;
    CUE_ASSERT(*m_assertContext, isOwner, "Cue.EditorCore Play Session Controller API requires its owner thread");
    if (!isOwner)
    {
        m_assertContext->fatal_handler().terminate(
            "Cue.EditorCore Play Session Controller API requires its owner thread");
    }
}
} // namespace cue::editor_core
