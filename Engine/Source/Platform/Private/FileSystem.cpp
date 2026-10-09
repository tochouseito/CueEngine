#include <Platform/FileSystem.h>

#include <array>
#include <new>

namespace cue
{
Result<bool> IFileSystem::exists(const Path &a_path)
{
    auto info = stat(a_path);
    return info.has_value() ? Result<bool>::success(info.try_value()->has_value())
                            : Result<bool>::failure(*info.try_error());
}

Result<std::vector<std::byte>> IFileSystem::read_all(const Path &a_path, std::size_t a_maxBytes)
{
    using bytesResult = Result<std::vector<std::byte>>;
    try
    {
        auto opened = open(a_path);
        if (!opened.has_value())
        {
            return bytesResult::failure(*opened.try_error());
        }
        auto file = opened.take_value();
        auto size = file->size();
        if (!size.has_value())
        {
            return bytesResult::failure(*size.try_error());
        }
        std::vector<std::byte> bytes;
        if (*size.try_value() > a_maxBytes || *size.try_value() > bytes.max_size())
        {
            return bytesResult::failure({ErrorCategory::InvalidArgument, "FileSystem.read_all.limit"});
        }
        bytes.resize(static_cast<std::size_t>(*size.try_value()));
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            auto read = file->read(std::span(bytes).subspan(offset));
            if (!read.has_value())
            {
                return bytesResult::failure(*read.try_error());
            }
            if (*read.try_value() == 0 || *read.try_value() > bytes.size() - offset)
            {
                return bytesResult::failure({ErrorCategory::PlatformFailure, "FileSystem.read_all.changed"});
            }
            offset += *read.try_value();
        }
        std::array<std::byte, 1> extra;
        auto end = file->read(extra);
        auto finalSize = file->size();
        if (!end.has_value() || !finalSize.has_value())
        {
            return bytesResult::failure(!end.has_value() ? *end.try_error() : *finalSize.try_error());
        }
        if (*end.try_value() != 0 || *finalSize.try_value() != bytes.size())
        {
            return bytesResult::failure({ErrorCategory::PlatformFailure, "FileSystem.read_all.changed"});
        }
        auto closed = file->close();
        return closed.has_value() ? bytesResult::success(std::move(bytes)) : bytesResult::failure(*closed.try_error());
    }
    catch (const std::bad_alloc &)
    {
        return bytesResult::failure({ErrorCategory::PlatformFailure, "FileSystem.read_all.allocation"});
    }
}

Result<void> IFileSystem::write_all(const Path &a_path, std::span<const std::byte> a_data, bool a_createParents)
{
    if (a_createParents)
    {
        auto created = create_directories(a_path.parent());
        if (!created.has_value())
        {
            return created;
        }
    }
    auto opened = open(a_path, {FileAccess::Write, FileCreation::CreateAlways});
    if (!opened.has_value())
    {
        return Result<void>::failure(*opened.try_error());
    }
    auto file = opened.take_value();
    std::size_t offset = 0;
    while (offset < a_data.size())
    {
        auto written = file->write(a_data.subspan(offset));
        if (!written.has_value())
        {
            return Result<void>::failure(*written.try_error());
        }
        if (*written.try_value() == 0 || *written.try_value() > a_data.size() - offset)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "FileSystem.write_all.progress"});
        }
        offset += *written.try_value();
    }
    auto flushed = file->flush();
    auto closed = file->close();
    return !flushed.has_value() ? std::move(flushed) : std::move(closed);
}
} // namespace cue
