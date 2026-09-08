#include <Cue/Platform/Process.h>

#include <utility>

namespace cue
{
ChildProcessResult ChildProcessResult::exited(std::uint32_t a_exitCode,
                                              std::vector<ChildProcessOutputChunk> a_output) noexcept
{
    return ChildProcessResult(ChildProcessOutcome::Exited, a_exitCode, std::move(a_output));
}

ChildProcessResult ChildProcessResult::cancelled(std::vector<ChildProcessOutputChunk> a_output) noexcept
{
    return ChildProcessResult(ChildProcessOutcome::Cancelled, std::nullopt, std::move(a_output));
}

ChildProcessResult ChildProcessResult::timed_out(std::vector<ChildProcessOutputChunk> a_output) noexcept
{
    return ChildProcessResult(ChildProcessOutcome::TimedOut, std::nullopt, std::move(a_output));
}

ChildProcessOutcome ChildProcessResult::outcome() const noexcept
{
    return m_outcome;
}

std::optional<std::uint32_t> ChildProcessResult::exit_code() const noexcept
{
    return m_exitCode;
}

const std::vector<ChildProcessOutputChunk> &ChildProcessResult::output() const noexcept
{
    return m_output;
}

ChildProcessResult::ChildProcessResult(ChildProcessOutcome a_outcome, std::optional<std::uint32_t> a_exitCode,
                                       std::vector<ChildProcessOutputChunk> a_output) noexcept
    : m_outcome(a_outcome), m_exitCode(a_exitCode), m_output(std::move(a_output))
{
}

ChildProcessRequest::ChildProcessRequest(std::string a_executable, std::vector<std::string> a_arguments,
                                         std::string a_workingDirectory,
                                         std::vector<ChildProcessEnvironmentEntry> a_environmentAllowlist,
                                         std::optional<std::chrono::milliseconds> a_timeout) noexcept
    : m_executable(std::move(a_executable)), m_arguments(std::move(a_arguments)),
      m_workingDirectory(std::move(a_workingDirectory)), m_environmentAllowlist(std::move(a_environmentAllowlist)),
      m_timeout(a_timeout)
{
}

std::string_view ChildProcessRequest::executable() const noexcept
{
    return m_executable;
}

const std::vector<std::string> &ChildProcessRequest::arguments() const noexcept
{
    return m_arguments;
}

std::string_view ChildProcessRequest::working_directory() const noexcept
{
    return m_workingDirectory;
}

const std::vector<ChildProcessEnvironmentEntry> &ChildProcessRequest::environment_allowlist() const noexcept
{
    return m_environmentAllowlist;
}

std::optional<std::chrono::milliseconds> ChildProcessRequest::timeout() const noexcept
{
    return m_timeout;
}

void ChildProcessCancellation::request_cancel() noexcept
{
    m_requested.store(true, std::memory_order_release);
}

bool ChildProcessCancellation::is_cancel_requested() const noexcept
{
    return m_requested.load(std::memory_order_acquire);
}
} // namespace cue
