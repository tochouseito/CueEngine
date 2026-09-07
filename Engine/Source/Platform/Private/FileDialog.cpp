#include <Cue/Platform/FileDialog.h>

#include <utility>

namespace cue
{
/// @brief Platform Adapterが検証済みの不透明Owner値、Generation、Owner Threadを保持する
FileDialogOwnerToken::FileDialogOwnerToken(std::uintptr_t a_nativeValue, std::uint64_t a_ownerGeneration,
                                           std::uint32_t a_ownerThreadId) noexcept
    : m_nativeValue(a_nativeValue), m_ownerGeneration(a_ownerGeneration), m_ownerThreadId(a_ownerThreadId)
{
}

/// @brief Owner Capabilityを移動し、移動元を無効化する
FileDialogOwnerToken::FileDialogOwnerToken(FileDialogOwnerToken &&a_other) noexcept
    : m_nativeValue(std::exchange(a_other.m_nativeValue, 0U)),
      m_ownerGeneration(std::exchange(a_other.m_ownerGeneration, 0U)),
      m_ownerThreadId(std::exchange(a_other.m_ownerThreadId, 0U))
{
}

/// @brief 現在のCapabilityを破棄してOwner Capabilityを移動する
FileDialogOwnerToken &FileDialogOwnerToken::operator=(FileDialogOwnerToken &&a_other) noexcept
{
    if (this != &a_other)
    {
        m_nativeValue = std::exchange(a_other.m_nativeValue, 0U);
        m_ownerGeneration = std::exchange(a_other.m_ownerGeneration, 0U);
        m_ownerThreadId = std::exchange(a_other.m_ownerThreadId, 0U);
    }

    return *this;
}

/// @brief Dialog種別、表示設定、短命Owner Capabilityを所有する要求を構築する
FileDialogRequest::FileDialogRequest(FileDialogKind a_kind, std::vector<FileDialogFilter> a_filters,
                                     std::string a_defaultExtension, std::string a_initialLocationHint,
                                     FileDialogOwnerToken a_owner) noexcept
    : m_kind(a_kind), m_filters(std::move(a_filters)), m_defaultExtension(std::move(a_defaultExtension)),
      m_initialLocationHint(std::move(a_initialLocationHint)), m_owner(std::move(a_owner))
{
}

/// @brief 要求したDialog種別を返す
FileDialogKind FileDialogRequest::kind() const noexcept
{
    return m_kind;
}

/// @brief 表示Filterを要求順で返す
const std::vector<FileDialogFilter> &FileDialogRequest::filters() const noexcept
{
    return m_filters;
}

/// @brief Save File用Default Extensionを返す
std::string_view FileDialogRequest::default_extension() const noexcept
{
    return m_defaultExtension;
}

/// @brief 初期Location Hintを返す
std::string_view FileDialogRequest::initial_location_hint() const noexcept
{
    return m_initialLocationHint;
}

/// @brief Dialog表示中だけ使用するOwner Capabilityを返す
const FileDialogOwnerToken &FileDialogRequest::owner() const noexcept
{
    return m_owner;
}

/// @brief 未検証UTF-8 Absolute Pathを持つSelected結果を構築する
FileDialogResult FileDialogResult::selected(std::string a_unverifiedPath) noexcept
{
    return FileDialogResult(FileDialogOutcome::Selected, std::move(a_unverifiedPath));
}

/// @brief Errorではない利用者Cancel結果を構築する
FileDialogResult FileDialogResult::cancelled() noexcept
{
    return FileDialogResult(FileDialogOutcome::Cancelled, std::nullopt);
}

/// @brief Dialogの非失敗Outcomeを返す
FileDialogOutcome FileDialogResult::outcome() const noexcept
{
    return m_outcome;
}

/// @brief Selected時だけ未検証UTF-8 Absolute Pathを返す
std::optional<std::string_view> FileDialogResult::selected_path() const noexcept
{
    if (!m_unverifiedPath.has_value())
    {
        return std::nullopt;
    }

    return std::string_view(*m_unverifiedPath);
}

/// @brief Outcomeと任意の未検証Pathを整合した状態で所有する
FileDialogResult::FileDialogResult(FileDialogOutcome a_outcome, std::optional<std::string> a_unverifiedPath) noexcept
    : m_outcome(a_outcome), m_unverifiedPath(std::move(a_unverifiedPath))
{
}
} // namespace cue
