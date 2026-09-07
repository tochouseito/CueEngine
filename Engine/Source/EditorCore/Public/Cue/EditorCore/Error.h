#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::editor_core
{
/// @brief Editor Core の回復可能な失敗を分類する Code
enum class EditorCoreError : std::int64_t
{
    DocumentNotFound = 1,
    DuplicateScene = 2,
    DuplicateLocator = 3,
    DocumentIdExhausted = 4,
    RevisionExhausted = 5,
    InvalidSavedState = 6,
    InvalidDocumentState = 7,
    InvalidCloseTransition = 8,
    SceneMismatch = 9,
    InvalidCommand = 10,
    InvalidTransaction = 11,
    UndoUnavailable = 12,
    RedoUnavailable = 13,
    PersistenceUnavailable = 14,
    ExternalConflict = 15,
    InvalidRecovery = 16,
    UnsupportedRecovery = 17,
    InvalidWorkspaceRequest = 18,
    WorkspaceEntryInUse = 19,
    WorkspaceUnavailable = 20,
    WorkspaceGenerationExhausted = 21
};

/// @brief Editor Core Error を診断 Summary と共に生成する
[[nodiscard]] Error make_editor_core_error(const AssertContext &a_assertContext, EditorCoreError a_code,
                                           std::string_view a_summary) noexcept;

/// @brief Document 単位の Editor Core Error へ対象 EditorDocumentId Context を付加する
[[nodiscard]] Error make_editor_document_error(const AssertContext &a_assertContext, EditorCoreError a_code,
                                               std::string_view a_summary, std::uint64_t a_documentId) noexcept;
} // namespace cue::editor_core
