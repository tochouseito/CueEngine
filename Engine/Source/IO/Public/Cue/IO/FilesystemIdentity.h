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

    /// @brief Platform AdapterだけがProvider、Volume Object、EntryのOpaque値から構築する
    FilesystemIdentity(std::uint64_t a_provider, std::uint64_t a_volumeHigh, std::uint64_t a_volumeLow,
                       std::uint64_t a_entryHigh, std::uint64_t a_entryLow) noexcept;

    std::uint64_t m_provider;
    std::uint64_t m_volumeHigh;
    std::uint64_t m_volumeLow;
    std::uint64_t m_entryHigh;
    std::uint64_t m_entryLow;
};
} // namespace cue
