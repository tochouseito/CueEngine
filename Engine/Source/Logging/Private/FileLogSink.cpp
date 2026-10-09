#include <Logging/FileLogSink.h>

#include <optional>
#include <span>
#include <utility>

namespace cue
{
FileLogSink::FileLogSink(CreateToken, std::unique_ptr<IFile> a_file) : m_file(std::move(a_file))
{
}

FileLogSink::~FileLogSink()
{
    try
    {
        static_cast<void>(close());
    }
    catch (...)
    { /* 明示 close の結果確認を公開契約とする */
    }
}

Result<std::unique_ptr<FileLogSink>> FileLogSink::create(IFileSystem &a_files, const Path &a_path, bool a_append)
{
    using sinkResult = Result<std::unique_ptr<FileLogSink>>;
    if (a_path.is_empty())
    {
        return sinkResult::failure({ErrorCategory::InvalidArgument, "FileLogSink.path"});
    }
    if (!a_path.parent().is_empty())
    {
        auto parents = a_files.create_directories(a_path.parent());
        if (!parents.has_value())
        {
            return sinkResult::failure(*parents.try_error());
        }
    }
    // Handle を得た後の Allocation 失敗で公開前の File を残さないよう Owner を先に確保する
    auto sink = std::make_unique<FileLogSink>(CreateToken{}, nullptr);
    auto opened =
        a_files.open(a_path, {FileAccess::Write, a_append ? FileCreation::OpenAlways : FileCreation::CreateNew});
    if (!opened.has_value())
    {
        return sinkResult::failure(*opened.try_error());
    }
    sink->m_file = opened.take_value();
    if (a_append)
    {
        auto moved = sink->m_file->seek(0, SeekOrigin::End);
        if (!moved.has_value())
        {
            return sinkResult::failure(*moved.try_error());
        }
    }
    return sinkResult::success(std::move(sink));
}

Result<void> FileLogSink::write(std::string_view a_line)
{
    if (!m_file || m_hasWriteFailure)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "FileLogSink.write.unavailable"});
    }
    try
    {
        auto bytes = std::as_bytes(std::span(a_line.data(), a_line.size()));
        while (!bytes.empty())
        {
            auto written = m_file->write(bytes);
            if (!written.has_value())
            {
                m_hasWriteFailure = true;
                return Result<void>::failure(*written.try_error());
            }
            const auto count = *written.try_value();
            if (count == 0 || count > bytes.size())
            {
                m_hasWriteFailure = true;
                return Result<void>::failure({ErrorCategory::PlatformFailure, "FileLogSink.write.progress"});
            }
            bytes = bytes.subspan(count);
        }
    }
    catch (...)
    {
        // 例外前に部分転送した可能性があるため、次の Record も同じ File へ追記しない
        m_hasWriteFailure = true;
        throw;
    }
    return Result<void>::success();
}

Result<void> FileLogSink::flush()
{
    return m_file ? m_file->flush() : Result<void>::failure({ErrorCategory::InvalidState, "FileLogSink.flush.closed"});
}

Result<void> FileLogSink::close()
{
    if (!m_file)
    {
        return Result<void>::success();
    }
    std::optional<Error> failure;
    try
    {
        auto flushed = m_file->flush();
        if (!flushed.has_value())
        {
            failure = *flushed.try_error();
        }
    }
    catch (...)
    {
        failure = Error{ErrorCategory::PlatformFailure, "FileLogSink.flush.exception"};
    }
    try
    {
        auto closed = m_file->close();
        if (closed.has_value())
        {
            m_file.reset();
        }
        else if (!failure)
        {
            failure = *closed.try_error();
        }
    }
    catch (...)
    {
        if (!failure)
        {
            failure = Error{ErrorCategory::PlatformFailure, "FileLogSink.close.exception"};
        }
    }
    return failure ? Result<void>::failure(std::move(*failure)) : Result<void>::success();
}
} // namespace cue
