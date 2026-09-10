#include <Cue/EditorCore/EditorController.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>
#include <utility>

namespace cue::editor_core
{
/// @brief DocumentStateId を一つの Controller Session へ結び付ける内部 Origin
class DocumentStateOrigin final
{
};

/// @brief Duplicate Open Error へ競合 Document Identity を明示的な Context として付加する
void add_conflicting_document_context(Error &a_error, const AssertContext &a_assertContext,
                                      EditorDocumentId a_conflictingDocumentId) noexcept
{
    constexpr std::string_view prefix = "ConflictingEditorDocumentId=";
    std::array<char, prefix.size() + 20U> context{};
    std::copy(prefix.begin(), prefix.end(), context.begin());
    const auto converted =
        std::to_chars(context.data() + prefix.size(), context.data() + context.size(), a_conflictingDocumentId.value());
    if (converted.ec != std::errc{})
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore conflicting document identity formatting failed");
    }
    a_error.add_context(a_assertContext.fatal_handler(),
                        std::string_view(context.data(), static_cast<std::size_t>(converted.ptr - context.data())));
}

/// @brief Duplicate Scene Error へ要求 Scene と競合 Document の Identity を付加する
[[nodiscard]] Error make_duplicate_scene_error(const AssertContext &a_assertContext,
                                               const scene::SceneAssetId &a_requestedSceneId,
                                               EditorDocumentId a_conflictingDocumentId) noexcept
{
    Error error = make_editor_core_error(a_assertContext, EditorCoreError::DuplicateScene,
                                         "Scene is already open in this project workspace");
    add_conflicting_document_context(error, a_assertContext, a_conflictingDocumentId);

    constexpr std::string_view prefix = "RequestedSceneAssetId=";
    const scene::IdentityText sceneText = a_requestedSceneId.canonical_text();
    std::array<char, prefix.size() + std::tuple_size_v<scene::IdentityText>> context{};
    std::copy(prefix.begin(), prefix.end(), context.begin());
    std::copy(sceneText.begin(), sceneText.end(), context.begin() + static_cast<std::ptrdiff_t>(prefix.size()));
    error.add_context(a_assertContext.fatal_handler(), std::string_view(context.data(), context.size()));
    return error;
}

/// @brief Duplicate Locator Error へ要求 Locator と競合 Document の Identity を付加する
[[nodiscard]] Error make_duplicate_locator_error(const AssertContext &a_assertContext,
                                                 const RelativePath &a_requestedLocator,
                                                 EditorDocumentId a_conflictingDocumentId) noexcept
{
    Error error = make_editor_core_error(a_assertContext, EditorCoreError::DuplicateLocator,
                                         "Scene locator is already open in this project workspace");
    add_conflicting_document_context(error, a_assertContext, a_conflictingDocumentId);
    error.add_context(a_assertContext.fatal_handler(), "RequestedSceneLocator");
    error.add_context(a_assertContext.fatal_handler(), a_requestedLocator.text());
    return error;
}

ProjectWorkspaceSession::ProjectWorkspaceSession(ProjectDescriptor &&a_descriptor) noexcept
    : m_descriptor(std::move(a_descriptor))
{
}

const ProjectDescriptor &ProjectWorkspaceSession::project_descriptor() const noexcept
{
    return m_descriptor;
}

std::span<const EditorDocument> ProjectWorkspaceSession::documents() const noexcept
{
    return m_documents;
}

const EditorDocument *ProjectWorkspaceSession::find_document(EditorDocumentId a_id) const noexcept
{
    const auto found = std::find_if(m_documents.begin(), m_documents.end(),
                                    /// @brief Document Identity が検索対象と一致するか判定する
                                    [a_id](const EditorDocument &a_document) { return a_document.id() == a_id; });
    return found != m_documents.end() ? &*found : nullptr;
}

std::unique_ptr<EditorController> EditorController::create(ProjectDescriptor &&a_descriptor,
                                                           const AssertContext &a_assertContext) noexcept
{
    try
    {
        return std::make_unique<EditorController>(ConstructionKey{}, std::move(a_descriptor), a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore controller allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore controller construction failed");
    }

    std::terminate();
}

std::unique_ptr<EditorController> EditorController::create(ProjectDescriptor &&a_descriptor,
                                                           ScenePersistenceServices a_persistenceServices,
                                                           const AssertContext &a_assertContext) noexcept
{
    try
    {
        return std::make_unique<EditorController>(ConstructionKey{}, std::move(a_descriptor), a_persistenceServices,
                                                  a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore controller allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.EditorCore controller construction failed");
    }

    std::terminate();
}

EditorController::EditorController(ConstructionKey, ProjectDescriptor &&a_descriptor,
                                   const AssertContext &a_assertContext)
    : m_stateOrigin(std::make_shared<DocumentStateOrigin>()), m_session(std::move(a_descriptor)),
      m_assertContext(&a_assertContext), m_ownerThread(std::this_thread::get_id())
{
}

EditorController::EditorController(ConstructionKey, ProjectDescriptor &&a_descriptor,
                                   ScenePersistenceServices a_persistenceServices,
                                   const AssertContext &a_assertContext)
    : m_stateOrigin(std::make_shared<DocumentStateOrigin>()), m_session(std::move(a_descriptor)),
      m_assertContext(&a_assertContext), m_ownerThread(std::this_thread::get_id()),
      m_projectRoot(a_persistenceServices.m_projectRoot),
      m_sourceAssetsRoot(a_persistenceServices.m_sourceAssetsRoot), m_savedRoot(a_persistenceServices.m_savedRoot),
      m_schemaRegistry(a_persistenceServices.m_schemaRegistry),
      m_valueSchemaRegistry(a_persistenceServices.m_valueSchemaRegistry),
      m_sceneMigrations(a_persistenceServices.m_sceneMigrations),
      m_componentMigrations(a_persistenceServices.m_componentMigrations)
{
}

EditorController::~EditorController() noexcept
{
    assert_owner_thread();
}

const ProjectWorkspaceSession &EditorController::session() const noexcept
{
    assert_owner_thread();
    return m_session;
}

Result<EditorDocumentId> EditorController::open_document(scene::SceneDocument &&a_document, RelativePath &&a_locator,
                                                         bool a_hasSavedDestination) noexcept
{
    assert_owner_thread();

    auto validation = a_document.validate();
    if (!validation)
    {
        return Result<EditorDocumentId>::failure(std::move(*validation.try_error()));
    }

    const auto locatorKey = a_locator.comparison_key(*m_assertContext);
    for (const EditorDocument &document : m_session.m_documents)
    {
        if (document.close_state() == DocumentCloseState::Closed)
        {
            continue;
        }

        if (document.scene_document().scene_asset_id() == a_document.scene_asset_id())
        {
            return Result<EditorDocumentId>::failure(
                make_duplicate_scene_error(*m_assertContext, a_document.scene_asset_id(), document.id()));
        }

        if (document.scene_locator().comparison_key(*m_assertContext) == locatorKey)
        {
            return Result<EditorDocumentId>::failure(
                make_duplicate_locator_error(*m_assertContext, a_locator, document.id()));
        }
    }

    if (m_nextDocumentId == std::numeric_limits<std::uint64_t>::max())
    {
        return Result<EditorDocumentId>::failure(make_editor_core_error(
            *m_assertContext, EditorCoreError::DocumentIdExhausted, "Editor document identity space is exhausted"));
    }

    EditorDocumentId id(m_nextDocumentId);
    ++m_nextDocumentId;

    try
    {
        EditorDocument openedDocument(id, m_stateOrigin, std::move(a_document), std::move(a_locator),
                                      a_hasSavedDestination);
        m_session.m_documents.push_back(std::move(openedDocument));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }

    return Result<EditorDocumentId>::success(std::move(id));
}

Result<void> EditorController::set_selection(EditorDocumentId a_documentId,
                                             std::span<const scene::ObjectId> a_objectIds,
                                             const scene::ObjectId *a_primaryObjectId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::Open)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                                                "Selection can change only while the document is open",
                                                                a_documentId.value()));
    }

    std::vector<scene::ObjectId> nextSelection;
    std::optional<scene::ObjectId> nextPrimary;
    try
    {
        nextSelection.reserve((std::min)(a_objectIds.size(), document->m_document.object_count()));
        for (const scene::ObjectId &objectId : a_objectIds)
        {
            if (document->m_document.find_object(objectId) == nullptr ||
                std::find(nextSelection.begin(), nextSelection.end(), objectId) != nextSelection.end())
            {
                continue;
            }
            nextSelection.push_back(objectId);
        }

        if (a_primaryObjectId != nullptr &&
            std::find(nextSelection.begin(), nextSelection.end(), *a_primaryObjectId) != nextSelection.end())
        {
            nextPrimary = *a_primaryObjectId;
        }
        else if (!nextSelection.empty())
        {
            nextPrimary = nextSelection.front();
        }
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }

    document->m_selection = std::move(nextSelection);
    document->m_primarySelection = std::move(nextPrimary);
    return Result<void>::success();
}

Result<void> EditorController::clear_selection(EditorDocumentId a_documentId) noexcept
{
    return set_selection(a_documentId, {});
}

Result<void> EditorController::reconcile_selection(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }

    return set_selection(a_documentId, document->m_selection, document->try_primary_selection());
}

Result<DocumentStateId> EditorController::record_persistent_change(EditorDocumentId a_documentId) noexcept
{
    return issue_persistent_state(a_documentId, true);
}

Result<DocumentStateId> EditorController::issue_persistent_state(EditorDocumentId a_documentId,
                                                                  bool a_invalidateHistory) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::Open)
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                       "Persistent changes require an open document", a_documentId.value()));
    }
    if (document->m_nextStateId == std::numeric_limits<std::uint64_t>::max())
    {
        return Result<DocumentStateId>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::RevisionExhausted,
                                       "Editor document state identity space is exhausted", a_documentId.value()));
    }

    document->m_currentStateId = DocumentStateId(m_stateOrigin, a_documentId, document->m_nextStateId);
    ++document->m_nextStateId;

    if (a_invalidateHistory)
    {
        document->m_history.clear();
        document->m_historyCursor = 0U;
        document->m_historyBytes = 0U;
    }

    auto reconciled = reconcile_selection(a_documentId);
    if (!reconciled)
    {
        return Result<DocumentStateId>::failure(std::move(*reconciled.try_error()));
    }
    DocumentStateId issuedState = document->m_currentStateId;
    return Result<DocumentStateId>::success(std::move(issuedState));
}

Result<void> EditorController::mark_saved(EditorDocumentId a_documentId, DocumentStateId a_savedStateId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState == DocumentCloseState::Closed || a_savedStateId.m_origin != m_stateOrigin ||
        a_savedStateId.document_id() != a_documentId || a_savedStateId.value() == 0U ||
        a_savedStateId.value() >= document->m_nextStateId)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::InvalidSavedState,
                                                                "Saved state identity was not issued by this document",
                                                                a_documentId.value()));
    }

    document->m_savedStateId = a_savedStateId;
    document->m_hasSavedDestination = true;
    if (document->m_closeState == DocumentCloseState::SaveRequested)
    {
        if (!document->is_dirty() && document->m_externalChangeState == ExternalChangeState::None &&
            document->m_persistenceState == DocumentPersistenceState::Idle)
        {
            document->m_closeState = DocumentCloseState::Closed;
            erase_closed_document(a_documentId);
        }
        else
        {
            document->m_closeState = DocumentCloseState::AwaitingDecision;
        }
    }
    return Result<void>::success();
}

Result<DocumentCloseState> EditorController::report_save_failure(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<DocumentCloseState>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::SaveRequested)
    {
        return Result<DocumentCloseState>::failure(make_editor_document_error(
            *m_assertContext, EditorCoreError::InvalidCloseTransition,
            "Save failure requires a document with a pending close save", a_documentId.value()));
    }

    document->m_closeState = DocumentCloseState::AwaitingDecision;
    return Result<DocumentCloseState>::success(DocumentCloseState::AwaitingDecision);
}

Result<void> EditorController::set_external_change_state(EditorDocumentId a_documentId,
                                                         ExternalChangeState a_state) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState == DocumentCloseState::Closed)
    {
        return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::InvalidDocumentState,
                                                                "Closed document cannot receive external change state",
                                                                a_documentId.value()));
    }

    document->m_externalChangeState = a_state;
    return Result<void>::success();
}

Result<DocumentCloseState> EditorController::request_close(EditorDocumentId a_documentId) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<DocumentCloseState>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::Open)
    {
        return Result<DocumentCloseState>::success(DocumentCloseState(document->m_closeState));
    }

    if (document->requires_close_decision())
    {
        document->m_closeState = DocumentCloseState::AwaitingDecision;
        return Result<DocumentCloseState>::success(DocumentCloseState::AwaitingDecision);
    }

    document->m_closeState = DocumentCloseState::Closed;
    erase_closed_document(a_documentId);
    return Result<DocumentCloseState>::success(DocumentCloseState::Closed);
}

Result<DocumentCloseState> EditorController::respond_to_close(EditorDocumentId a_documentId,
                                                              CloseDecision a_decision) noexcept
{
    assert_owner_thread();
    EditorDocument *document = find_document(a_documentId);
    if (document == nullptr)
    {
        return Result<DocumentCloseState>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                       "Editor document was not found", a_documentId.value()));
    }
    if (document->m_closeState != DocumentCloseState::AwaitingDecision)
    {
        return Result<DocumentCloseState>::failure(
            make_editor_document_error(*m_assertContext, EditorCoreError::InvalidCloseTransition,
                                       "Close decision requires an awaiting document", a_documentId.value()));
    }

    switch (a_decision)
    {
    case CloseDecision::Save:
        if (document->m_externalChangeState != ExternalChangeState::None)
        {
            return Result<DocumentCloseState>::failure(make_editor_document_error(
                *m_assertContext, EditorCoreError::InvalidCloseTransition,
                "External scene changes require reload, save as, or cancel before close", a_documentId.value()));
        }
        document->m_closeState = DocumentCloseState::SaveRequested;
        break;
    case CloseDecision::Discard:
        document->m_closeState = DocumentCloseState::Closed;
        erase_closed_document(a_documentId);
        return Result<DocumentCloseState>::success(DocumentCloseState::Closed);
    case CloseDecision::Cancel:
        document->m_closeState = DocumentCloseState::Open;
        break;
    }

    return Result<DocumentCloseState>::success(DocumentCloseState(document->m_closeState));
}

EditorDocument *EditorController::find_document(EditorDocumentId a_id) noexcept
{
    const auto found = std::find_if(m_session.m_documents.begin(), m_session.m_documents.end(),
                                    /// @brief Document Identity が検索対象と一致するか判定する
                                    [a_id](const EditorDocument &a_document) { return a_document.id() == a_id; });
    return found != m_session.m_documents.end() ? &*found : nullptr;
}

void EditorController::erase_closed_document(EditorDocumentId a_id) noexcept
{
    const auto found = std::find_if(m_session.m_documents.begin(), m_session.m_documents.end(),
                                    /// @brief Document Identity が削除対象と一致するか判定する
                                    [a_id](const EditorDocument &a_document) { return a_document.id() == a_id; });
    if (found != m_session.m_documents.end())
    {
        m_session.m_documents.erase(found);
    }
}

void EditorController::assert_owner_thread() const noexcept
{
    const bool isOwner = std::this_thread::get_id() == m_ownerThread;
    CUE_ASSERT(*m_assertContext, isOwner, "Cue.EditorCore controller API requires its owner thread");
    if (!isOwner)
    {
        m_assertContext->fatal_handler().terminate("Cue.EditorCore controller API requires its owner thread");
    }
}

[[noreturn]] void EditorController::terminate_allocation() const noexcept
{
    m_assertContext->fatal_handler().terminate("Cue.EditorCore allocation failed");
}

[[noreturn]] void EditorController::terminate_exception() const noexcept
{
    m_assertContext->fatal_handler().terminate("Cue.EditorCore unexpected exception");
}
} // namespace cue::editor_core
