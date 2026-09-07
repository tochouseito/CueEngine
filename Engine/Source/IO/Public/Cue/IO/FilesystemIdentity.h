#pragma once

#include <cstdint>

namespace cue
{
class FilesystemRoot;
class WorkspaceFilesystem;

/// @brief Native PathやHandleを公開せず同じFilesystem Entry Bindingか比較するOpaque値
class FilesystemIdentity final
{
  public:
    /// @brief Provider ScopeとEntry Identityの全要素を比較する
    [[nodiscard]] bool operator==(const FilesystemIdentity &) const noexcept = default;

  private:
    friend class FilesystemRoot;
    friend class WorkspaceFilesystem;

    /// @brief Platform AdapterだけがProvider固有の二つのOpaque値から構築する
    FilesystemIdentity(std::uint64_t a_providerScope, std::uint64_t a_entry) noexcept;

    std::uint64_t m_providerScope;
    std::uint64_t m_entry;
};
} // namespace cue
