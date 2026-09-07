#include <Cue/IO/Filesystem.h>

#include <Cue/IO/Error.h>

#include <utility>

namespace cue
{
FilesystemIdentity::FilesystemIdentity(std::uint64_t a_provider, std::uint64_t a_volumeHigh, std::uint64_t a_volumeLow,
                                       std::uint64_t a_entryHigh, std::uint64_t a_entryLow) noexcept
    : m_provider(a_provider), m_volumeHigh(a_volumeHigh), m_volumeLow(a_volumeLow), m_entryHigh(a_entryHigh),
      m_entryLow(a_entryLow)
{
}

FileWriteLease::FileWriteLease(std::unique_ptr<FileWriteLeaseState> a_state) noexcept : m_state(std::move(a_state))
{
}

FileWriteLease::FileWriteLease(FileWriteLease &&) noexcept = default;

FileWriteLease &FileWriteLease::operator=(FileWriteLease &&) noexcept = default;

FileWriteLease::~FileWriteLease() = default;

StagingArea::StagingArea(RelativePath &&a_path, std::uint64_t a_token) noexcept
    : m_path(std::move(a_path)), m_token(a_token)
{
}

StagingArea::StagingArea(StagingArea &&a_other) noexcept : m_path(std::move(a_other.m_path)), m_token(a_other.m_token)
{
    a_other.m_token = 0;
}

StagingArea &StagingArea::operator=(StagingArea &&a_other) noexcept
{
    if (this != &a_other)
    {
        std::swap(m_path, a_other.m_path);
        std::swap(m_token, a_other.m_token);
    }

    return *this;
}

const RelativePath &StagingArea::path() const noexcept
{
    return m_path;
}

StagingArea FilesystemRoot::make_staging_area(RelativePath &&a_path, std::uint64_t a_token) noexcept
{
    return StagingArea(std::move(a_path), a_token);
}

FilesystemIdentity FilesystemRoot::make_filesystem_identity(std::uint64_t a_providerScope,
                                                            std::uint64_t a_entry) noexcept
{
    return FilesystemIdentity(a_providerScope, 0U, 0U, 0U, a_entry);
}

FilesystemIdentity FilesystemRoot::make_filesystem_identity(std::uint64_t a_provider, std::uint64_t a_volumeHigh,
                                                            std::uint64_t a_volumeLow, std::uint64_t a_entryHigh,
                                                            std::uint64_t a_entryLow) noexcept
{
    return FilesystemIdentity(a_provider, a_volumeHigh, a_volumeLow, a_entryHigh, a_entryLow);
}

FileWriteLease FilesystemRoot::make_file_write_lease(std::unique_ptr<FileWriteLeaseState> a_state) noexcept
{
    return FileWriteLease(std::move(a_state));
}

FileWriteLeaseState *FilesystemRoot::file_write_lease_state(FileWriteLease &a_lease) noexcept
{
    return a_lease.m_state.get();
}

std::uint64_t FilesystemRoot::staging_token(const StagingArea &a_staging) noexcept
{
    return a_staging.m_token;
}

void FilesystemRoot::invalidate_staging(StagingArea &a_staging) noexcept
{
    a_staging.m_token = 0;
}

std::uint64_t file_content_digest(std::span<const std::byte> a_bytes) noexcept
{
    std::uint64_t digest = 14695981039346656037ULL;
    for (const std::byte value : a_bytes)
    {
        digest ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value));
        digest *= 1099511628211ULL;
    }
    return digest;
}

Result<FileFingerprint> fingerprint_file(FilesystemRoot &a_filesystem, const RelativePath &a_path,
                                         std::size_t a_maxBytes, const AssertContext &a_assertContext) noexcept
{
    auto entry = a_filesystem.query_entry(a_path);
    if (!entry)
    {
        return Result<FileFingerprint>::failure(std::move(*entry.try_error()));
    }
    if (*entry.try_value() == EntryType::Missing)
    {
        return Result<FileFingerprint>::success(FileFingerprint{});
    }
    if (*entry.try_value() != EntryType::RegularFile)
    {
        const IoError code =
            *entry.try_value() == EntryType::UnsupportedEntry ? IoError::UnsupportedEntry : IoError::TypeMismatch;
        return Result<FileFingerprint>::failure(
            make_io_error(a_assertContext, code, "Fingerprint target is not a regular file"));
    }
    auto bytes = a_filesystem.read_file(a_path, a_maxBytes);
    if (!bytes)
    {
        return Result<FileFingerprint>::failure(std::move(*bytes.try_error()));
    }
    const auto &storage = *bytes.try_value();
    return Result<FileFingerprint>::success(
        FileFingerprint{true, static_cast<std::uint64_t>(storage.size()), file_content_digest(storage)});
}
} // namespace cue
