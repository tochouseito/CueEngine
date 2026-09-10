#include <Cue/Platform/Windows/WindowsProcess.h>

#include "WindowsUtilities.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maxCommandLineLength = 32767U;
constexpr std::size_t k_maxEnvironmentLength = 32767U;
constexpr DWORD k_pollMilliseconds = 10U;
constexpr DWORD k_terminationWaitMilliseconds = 5000U;
constexpr auto k_gracefulStopTimeout = std::chrono::seconds(5);
constexpr DWORD k_cancelExitCode = 0xC000013AU;
constexpr DWORD k_timeoutExitCode = 0xC0000102U;

/// @brief Windows HANDLEを一意所有して全経路で一度だけCloseする
class HandleOwner final
{
  public:
    HandleOwner() noexcept = default;
    explicit HandleOwner(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    HandleOwner(const HandleOwner &) = delete;
    HandleOwner &operator=(const HandleOwner &) = delete;
    HandleOwner(HandleOwner &&a_other) noexcept : m_handle(std::exchange(a_other.m_handle, nullptr))
    {
    }
    HandleOwner &operator=(HandleOwner &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, nullptr);
        }
        return *this;
    }
    ~HandleOwner() noexcept
    {
        reset();
    }

    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

    void reset() noexcept
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_handle);
        }
        m_handle = nullptr;
    }

  private:
    HANDLE m_handle = nullptr;
};

/// @brief Process Thread Attribute Listを一意所有する
class AttributeListOwner final
{
  public:
    AttributeListOwner() noexcept = default;
    AttributeListOwner(const AttributeListOwner &) = delete;
    AttributeListOwner &operator=(const AttributeListOwner &) = delete;
    ~AttributeListOwner() noexcept
    {
        if (m_list != nullptr)
        {
            DeleteProcThreadAttributeList(m_list);
        }
    }

    void assign(LPPROC_THREAD_ATTRIBUTE_LIST a_list) noexcept
    {
        m_list = a_list;
    }

  private:
    LPPROC_THREAD_ATTRIBUTE_LIST m_list = nullptr;
};

/// @brief Capture Thread間で全Streamの観測順とRead失敗を共有する
struct CaptureState final
{
    std::mutex mutex;
    std::uint64_t nextSequence = 0U;
    std::vector<cue::ChildProcessOutputChunk> chunks;
    std::optional<std::size_t> maximumBytes;
    std::size_t capturedBytes = 0U;
    DWORD outputError = ERROR_SUCCESS;
    DWORD errorError = ERROR_SUCCESS;
};

/// @brief AllocationやThread構築失敗を既存Fatal境界へ渡す
[[noreturn]] void terminate_process_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows child process failed unexpectedly");
    std::abort();
}

/// @brief Process境界の契約違反を安定Errorへ変換する
[[nodiscard]] cue::Error make_process_error(const cue::AssertContext &a_assertContext, cue::WindowsProcessError a_code,
                                            std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Platform.Windows.Process",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Win32失敗をNative Code付きProcess Errorへ変換する
[[nodiscard]] cue::Error make_native_process_error(const cue::AssertContext &a_assertContext,
                                                   cue::WindowsProcessError a_code, std::string_view a_summary,
                                                   DWORD a_nativeCode) noexcept
{
    return cue::windows_private::make_native_error(a_assertContext, "Cue.Platform.Windows.Process",
                                                   static_cast<std::int64_t>(a_code), a_summary, a_nativeCode);
}

/// @brief Strict UTF-8を埋め込みNULなしUTF-16へ変換する
[[nodiscard]] std::optional<std::wstring> to_utf16(std::string_view a_text, const cue::AssertContext &a_assertContext)
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_utf8_to_windows_utf16(a_text, converted, a_assertContext.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.find(L'\0') != std::wstring::npos)
    {
        return std::nullopt;
    }
    return converted;
}

/// @brief Windows C Runtime規則で一ArgumentをCommand LineへQuoteする
void append_argument(std::wstring &a_commandLine, std::wstring_view a_argument)
{
    if (!a_commandLine.empty())
    {
        a_commandLine.push_back(L' ');
    }
    a_commandLine.push_back(L'"');
    std::size_t slashCount = 0U;
    for (const wchar_t character : a_argument)
    {
        if (character == L'\\')
        {
            ++slashCount;
            continue;
        }
        if (character == L'"')
        {
            a_commandLine.append(slashCount * 2U + 1U, L'\\');
            a_commandLine.push_back(L'"');
            slashCount = 0U;
            continue;
        }
        a_commandLine.append(slashCount, L'\\');
        slashCount = 0U;
        a_commandLine.push_back(character);
    }
    a_commandLine.append(slashCount * 2U, L'\\');
    a_commandLine.push_back(L'"');
}

/// @brief RequestのExecutableとArgument VectorからShell非依存Command Lineを構築する
[[nodiscard]] std::optional<std::wstring> build_command_line(const cue::ChildProcessRequest &a_request,
                                                             const cue::AssertContext &a_assertContext)
{
    const auto executable = to_utf16(a_request.executable(), a_assertContext);
    if (!executable || executable->empty())
    {
        return std::nullopt;
    }
    std::wstring commandLine;
    append_argument(commandLine, *executable);
    for (const std::string &argument : a_request.arguments())
    {
        const auto converted = to_utf16(argument, a_assertContext);
        if (!converted)
        {
            return std::nullopt;
        }
        append_argument(commandLine, *converted);
    }
    if (commandLine.size() + 1U > k_maxCommandLineLength)
    {
        return std::nullopt;
    }
    return commandLine;
}

/// @brief 明示AllowlistだけからCase-insensitive整列済みWindows Environment Blockを構築する
[[nodiscard]] std::optional<std::vector<wchar_t>> build_environment_block(const cue::ChildProcessRequest &a_request,
                                                                          const cue::AssertContext &a_assertContext)
{
    std::vector<std::wstring> entries;
    entries.reserve(a_request.environment_allowlist().size());
    for (const cue::ChildProcessEnvironmentEntry &entry : a_request.environment_allowlist())
    {
        const auto name = to_utf16(entry.name, a_assertContext);
        const auto value = to_utf16(entry.value, a_assertContext);
        if (!name || !value || name->empty() || name->find(L'=') != std::wstring::npos)
        {
            return std::nullopt;
        }
        for (const std::wstring &existing : entries)
        {
            const std::size_t separator = existing.find(L'=');
            const std::wstring_view existingName(existing.data(), separator);
            if (CompareStringOrdinal(existingName.data(), static_cast<int>(existingName.size()), name->data(),
                                     static_cast<int>(name->size()), TRUE) == CSTR_EQUAL)
            {
                return std::nullopt;
            }
        }
        entries.push_back(*name + L"=" + *value);
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring &a_left, const std::wstring &a_right) noexcept
              { return CompareStringOrdinal(a_left.c_str(), -1, a_right.c_str(), -1, TRUE) == CSTR_LESS_THAN; });

    std::vector<wchar_t> block;
    for (const std::wstring &entry : entries)
    {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    if (entries.empty())
    {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() > k_maxEnvironmentLength)
    {
        return std::nullopt;
    }
    return block;
}

/// @brief Pipeを読み切り、Chunk追加時点の全Stream共通Sequenceを付与する
void capture_pipe(HANDLE a_pipe, cue::ChildProcessStream a_stream, CaptureState &a_state,
                  const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::array<char, 4096U> buffer{};
        while (true)
        {
            DWORD read = 0U;
            if (ReadFile(a_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE)
            {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE)
                {
                    std::scoped_lock lock(a_state.mutex);
                    DWORD &destination =
                        a_stream == cue::ChildProcessStream::StandardOutput ? a_state.outputError : a_state.errorError;
                    destination = error;
                }
                return;
            }
            if (read == 0U)
            {
                return;
            }
            std::scoped_lock lock(a_state.mutex);
            const std::size_t available = a_state.maximumBytes ? (*a_state.maximumBytes > a_state.capturedBytes
                                                                      ? *a_state.maximumBytes - a_state.capturedBytes
                                                                      : 0U)
                                                               : static_cast<std::size_t>(read);
            const std::size_t captured = std::min<std::size_t>(read, available);
            if (captured > 0U)
            {
                a_state.chunks.push_back({a_state.nextSequence++, a_stream, std::string(buffer.data(), captured)});
                a_state.capturedBytes += captured;
            }
        }
    }
    catch (...)
    {
        terminate_process_exception(a_assertContext);
    }
}

/// @brief ProcessがSignal済みでなければJob Treeを終了し、Primary Process終了を待つ
[[nodiscard]] DWORD terminate_process_tree(HANDLE a_job, HANDLE a_process, DWORD a_exitCode) noexcept
{
    if (WaitForSingleObject(a_process, 0U) == WAIT_OBJECT_0)
    {
        return ERROR_SUCCESS;
    }
    if (TerminateJobObject(a_job, a_exitCode) == FALSE)
    {
        const DWORD error = GetLastError();
        if (WaitForSingleObject(a_process, 0U) != WAIT_OBJECT_0)
        {
            return error;
        }
    }
    const DWORD wait = WaitForSingleObject(a_process, k_terminationWaitMilliseconds);
    return wait == WAIT_OBJECT_0 ? ERROR_SUCCESS : (wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
}

/// @brief 一つのProcessが所有するTop-level WindowへWM_CLOSEを通知する列挙Context
struct GracefulStopContext final
{
    DWORD processId = 0U;
    bool hasPosted = false;
};

/// @brief 対象Processが所有するTop-level WindowだけへWM_CLOSEを通知する
[[nodiscard]] BOOL CALLBACK post_close_to_process_window(HWND a_window, LPARAM a_parameter) noexcept
{
    auto &context = *reinterpret_cast<GracefulStopContext *>(a_parameter);
    DWORD ownerProcessId = 0U;
    static_cast<void>(GetWindowThreadProcessId(a_window, &ownerProcessId));
    if (ownerProcessId == context.processId && PostMessageW(a_window, WM_CLOSE, 0U, 0U) != FALSE)
    {
        context.hasPosted = true;
    }
    return TRUE;
}

/// @brief 対象Processの作成済みWindowへ正常終了要求を送り、未作成時は次回Pollへ委ねる
[[nodiscard]] bool request_graceful_process_stop(DWORD a_processId) noexcept
{
    GracefulStopContext context{a_processId, false};
    static_cast<void>(EnumWindows(post_close_to_process_window, reinterpret_cast<LPARAM>(&context)));
    return context.hasPosted;
}

/// @brief Windows Job ObjectとPipe Captureで一回ずつ同期実行するRunner
class WindowsChildProcessRunner final : public cue::ChildProcessRunner
{
  public:
    explicit WindowsChildProcessRunner(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &a_request,
        const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        try
        {
            return run_impl(a_request, a_cancellation);
        }
        catch (...)
        {
            terminate_process_exception(*m_assertContext);
        }
    }

  private:
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run_impl(const cue::ChildProcessRequest &a_request,
                                                                const cue::ChildProcessCancellation &a_cancellation)
    {
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        if ((a_request.timeout() && a_request.timeout()->count() <= 0) || a_request.working_directory().empty())
        {
            return cue::Result<cue::ChildProcessResult>::failure(make_process_error(
                *m_assertContext, cue::WindowsProcessError::InvalidRequest, "Child process request is invalid"));
        }

        const auto executable = to_utf16(a_request.executable(), *m_assertContext);
        const auto workingDirectory = to_utf16(a_request.working_directory(), *m_assertContext);
        const auto commandLine = build_command_line(a_request, *m_assertContext);
        auto environment = build_environment_block(a_request, *m_assertContext);
        if (!executable || executable->empty() || !workingDirectory || workingDirectory->empty() || !commandLine ||
            !environment || !std::filesystem::path(*executable).is_absolute() ||
            !std::filesystem::path(*workingDirectory).is_absolute())
        {
            return cue::Result<cue::ChildProcessResult>::failure(make_process_error(
                *m_assertContext, cue::WindowsProcessError::InvalidRequest, "Child process request is invalid"));
        }
        const DWORD executableAttributes = GetFileAttributesW(executable->c_str());
        const DWORD workingAttributes = GetFileAttributesW(workingDirectory->c_str());
        if (executableAttributes == INVALID_FILE_ATTRIBUTES ||
            (executableAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U || workingAttributes == INVALID_FILE_ATTRIBUTES ||
            (workingAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_process_error(*m_assertContext, cue::WindowsProcessError::InvalidRequest,
                                   "Child process executable or working directory is unavailable"));
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE outputReadRaw = nullptr;
        HANDLE outputWriteRaw = nullptr;
        if (CreatePipe(&outputReadRaw, &outputWriteRaw, &security, 0U) == FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::PipeCreationFailed,
                                          "stdout pipe creation failed", GetLastError()));
        }
        HandleOwner outputRead(outputReadRaw);
        HandleOwner outputWrite(outputWriteRaw);
        HANDLE errorReadRaw = nullptr;
        HANDLE errorWriteRaw = nullptr;
        if (CreatePipe(&errorReadRaw, &errorWriteRaw, &security, 0U) == FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::PipeCreationFailed,
                                          "stderr pipe creation failed", GetLastError()));
        }
        HandleOwner errorRead(errorReadRaw);
        HandleOwner errorWrite(errorWriteRaw);
        if (SetHandleInformation(outputRead.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE ||
            SetHandleInformation(errorRead.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::InheritanceConfigurationFailed,
                                          "Process pipe inheritance configuration failed", GetLastError()));
        }
        HandleOwner nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (nullInput.get() == INVALID_HANDLE_VALUE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::InheritanceConfigurationFailed,
                                          "Process stdin configuration failed", GetLastError()));
        }

        HandleOwner job(CreateJobObjectW(nullptr, nullptr));
        if (job.get() == nullptr)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::JobCreationFailed,
                                          "Process Job Object creation failed", GetLastError()));
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)) ==
            FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::JobCreationFailed,
                                          "Process Job Object configuration failed", GetLastError()));
        }

        SIZE_T attributeBytes = 0U;
        static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1U, 0U, &attributeBytes));
        if (attributeBytes == 0U)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::InheritanceConfigurationFailed,
                                          "Process handle allowlist sizing failed", GetLastError()));
        }
        std::vector<std::byte> attributeStorage(attributeBytes);
        auto *attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (InitializeProcThreadAttributeList(attributeList, 1U, 0U, &attributeBytes) == FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::InheritanceConfigurationFailed,
                                          "Process handle allowlist creation failed", GetLastError()));
        }
        AttributeListOwner attributeOwner;
        attributeOwner.assign(attributeList);
        std::array<HANDLE, 3U> inheritedHandles = {nullInput.get(), outputWrite.get(), errorWrite.get()};
        if (UpdateProcThreadAttribute(attributeList, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles.data(),
                                      sizeof(inheritedHandles), nullptr, nullptr) == FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::InheritanceConfigurationFailed,
                                          "Process handle allowlist update failed", GetLastError()));
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nullInput.get();
        startup.StartupInfo.hStdOutput = outputWrite.get();
        startup.StartupInfo.hStdError = errorWrite.get();
        startup.lpAttributeList = attributeList;
        PROCESS_INFORMATION processInfo{};
        std::wstring mutableCommandLine = *commandLine;
        const DWORD flags =
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
        if (CreateProcessW(executable->c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE, flags,
                           environment->data(), workingDirectory->c_str(), &startup.StartupInfo, &processInfo) == FALSE)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::ProcessCreationFailed,
                                          "Child process creation failed", GetLastError()));
        }
        HandleOwner process(processInfo.hProcess);
        HandleOwner primaryThread(processInfo.hThread);
        if (AssignProcessToJobObject(job.get(), process.get()) == FALSE)
        {
            const DWORD error = GetLastError();
            static_cast<void>(TerminateProcess(process.get(), k_cancelExitCode));
            static_cast<void>(WaitForSingleObject(process.get(), k_terminationWaitMilliseconds));
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::ProcessAssignmentFailed,
                                          "Child process Job Object assignment failed", error));
        }
        const auto started = std::chrono::steady_clock::now();
        if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1))
        {
            const DWORD error = GetLastError();
            static_cast<void>(terminate_process_tree(job.get(), process.get(), k_cancelExitCode));
            return cue::Result<cue::ChildProcessResult>::failure(make_native_process_error(
                *m_assertContext, cue::WindowsProcessError::ProcessResumeFailed, "Child process resume failed", error));
        }
        primaryThread.reset();
        nullInput.reset();
        outputWrite.reset();
        errorWrite.reset();

        CaptureState capture;
        capture.maximumBytes = a_request.maximum_captured_output_bytes();
        capture.chunks.reserve(32U);
        std::thread outputThread;
        std::thread errorThread;
        try
        {
            outputThread = std::thread(capture_pipe, outputRead.get(), cue::ChildProcessStream::StandardOutput,
                                       std::ref(capture), std::cref(*m_assertContext));
            errorThread = std::thread(capture_pipe, errorRead.get(), cue::ChildProcessStream::StandardError,
                                      std::ref(capture), std::cref(*m_assertContext));
        }
        catch (...)
        {
            static_cast<void>(terminate_process_tree(job.get(), process.get(), k_cancelExitCode));
            job.reset();
            if (outputThread.joinable())
            {
                outputThread.join();
            }
            if (errorThread.joinable())
            {
                errorThread.join();
            }
            terminate_process_exception(*m_assertContext);
        }

        cue::ChildProcessOutcome outcome = cue::ChildProcessOutcome::Exited;
        DWORD waitError = ERROR_SUCCESS;
        std::optional<std::chrono::steady_clock::time_point> gracefulStopDeadline;
        bool didCompleteGracefulStop = false;
        while (true)
        {
            const DWORD wait = WaitForSingleObject(process.get(), k_pollMilliseconds);
            if (wait == WAIT_OBJECT_0)
            {
                if (a_cancellation.cancellation_mode() == cue::ChildProcessCancellationMode::Graceful &&
                    gracefulStopDeadline.has_value())
                {
                    didCompleteGracefulStop = true;
                }
                else if (a_cancellation.is_cancel_requested())
                {
                    outcome = cue::ChildProcessOutcome::Cancelled;
                }
                break;
            }
            if (wait == WAIT_FAILED)
            {
                waitError = GetLastError();
                static_cast<void>(terminate_process_tree(job.get(), process.get(), k_cancelExitCode));
                break;
            }
            const cue::ChildProcessCancellationMode cancellationMode = a_cancellation.cancellation_mode();
            if (cancellationMode == cue::ChildProcessCancellationMode::Immediate)
            {
                outcome = cue::ChildProcessOutcome::Cancelled;
                waitError = terminate_process_tree(job.get(), process.get(), k_cancelExitCode);
                break;
            }
            if (cancellationMode == cue::ChildProcessCancellationMode::Graceful)
            {
                outcome = cue::ChildProcessOutcome::Cancelled;
                const auto now = std::chrono::steady_clock::now();
                if (!gracefulStopDeadline)
                {
                    gracefulStopDeadline = now + k_gracefulStopTimeout;
                }
                static_cast<void>(request_graceful_process_stop(processInfo.dwProcessId));
                if (now >= *gracefulStopDeadline)
                {
                    waitError = terminate_process_tree(job.get(), process.get(), k_cancelExitCode);
                    break;
                }
                continue;
            }
            if (a_request.timeout() && std::chrono::steady_clock::now() - started >= *a_request.timeout())
            {
                outcome = cue::ChildProcessOutcome::TimedOut;
                waitError = terminate_process_tree(job.get(), process.get(), k_timeoutExitCode);
                break;
            }
        }

        std::optional<std::uint32_t> exitCode;
        DWORD exitCodeError = ERROR_SUCCESS;
        if ((outcome == cue::ChildProcessOutcome::Exited || didCompleteGracefulStop) && waitError == ERROR_SUCCESS)
        {
            DWORD nativeExitCode = 0U;
            if (GetExitCodeProcess(process.get(), &nativeExitCode) == FALSE)
            {
                exitCodeError = GetLastError();
            }
            else
            {
                exitCode = nativeExitCode;
            }
        }
        job.reset();
        outputThread.join();
        errorThread.join();
        outputRead.reset();
        errorRead.reset();

        if (waitError != ERROR_SUCCESS)
        {
            const cue::WindowsProcessError code = outcome == cue::ChildProcessOutcome::Exited
                                                      ? cue::WindowsProcessError::ProcessWaitFailed
                                                      : cue::WindowsProcessError::ProcessTerminationFailed;
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, code, "Child process completion failed", waitError));
        }
        if (capture.outputError != ERROR_SUCCESS || capture.errorError != ERROR_SUCCESS)
        {
            const DWORD error = capture.outputError != ERROR_SUCCESS ? capture.outputError : capture.errorError;
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::OutputCaptureFailed,
                                          "Child process output capture failed", error));
        }
        if (exitCodeError != ERROR_SUCCESS)
        {
            return cue::Result<cue::ChildProcessResult>::failure(
                make_native_process_error(*m_assertContext, cue::WindowsProcessError::ExitCodeQueryFailed,
                                          "Child process exit code query failed", exitCodeError));
        }
        if (outcome == cue::ChildProcessOutcome::Cancelled)
        {
            if (didCompleteGracefulStop && *exitCode != 0U)
            {
                return cue::Result<cue::ChildProcessResult>::success(
                    cue::ChildProcessResult::exited(*exitCode, std::move(capture.chunks)));
            }
            return cue::Result<cue::ChildProcessResult>::success(
                cue::ChildProcessResult::cancelled(std::move(capture.chunks)));
        }
        if (outcome == cue::ChildProcessOutcome::TimedOut)
        {
            return cue::Result<cue::ChildProcessResult>::success(
                cue::ChildProcessResult::timed_out(std::move(capture.chunks)));
        }
        return cue::Result<cue::ChildProcessResult>::success(
            cue::ChildProcessResult::exited(*exitCode, std::move(capture.chunks)));
    }

    const cue::AssertContext *m_assertContext;
};
} // namespace

namespace cue
{
Result<std::unique_ptr<ChildProcessRunner>> create_windows_child_process_runner(
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        return Result<std::unique_ptr<ChildProcessRunner>>::success(
            std::make_unique<WindowsChildProcessRunner>(a_assertContext));
    }
    catch (...)
    {
        terminate_process_exception(a_assertContext);
    }
}
} // namespace cue
