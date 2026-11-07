#include <EditorIde/Windows/WindowsIdeService.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <thread>
#include <vector>

#include <Foundation/Utf8.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Windows/WindowsFileSystem.h>

#include "IdeProcess.h"

namespace cue
{
namespace
{
constexpr std::size_t k_maxResponseBytes = 64 * 1024;
/// @brief Bridge 応答の Operation 名を公開要求と一致させる
const char *operation_name(IdeOperation a_operation)
{
    switch (a_operation)
    {
    case IdeOperation::OpenSolution:
        return "open-solution";
    case IdeOperation::OpenSource:
        return "open-source";
    case IdeOperation::AttachDebugger:
        return "attach-debugger";
    default:
        return "none";
    }
}
/// @brief 値を Locale や例外に依存せず読み、余剰文字を拒否する
template <class Number> bool parse_number(std::string_view a_text, Number &a_number)
{
    const auto result = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_number);
    return result.ec == std::errc{} && result.ptr == a_text.data() + a_text.size();
}
/// @brief 全親要素を確認し、File / Directory の Link 別名を受け付けない
Result<void> validate_file(IFileSystem &a_files, const Path &a_file)
{
    if (!a_file.is_absolute())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorIde.absolute_file_required"});
    auto current = a_file;
    auto type = FileType::Regular;
    for (;;)
    {
        auto info = a_files.stat(current);
        if (!info.has_value())
            return Result<void>::failure(*info.try_error());
        if (!*info.try_value() || (**info.try_value()).type != type)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorIde.file_kind:" + current.utf8()});
        auto parent = current.parent();
        if (parent.utf8() == current.utf8())
            break;
        current = std::move(parent);
        type = FileType::Directory;
    }
    return Result<void>::success();
}
/// @brief 入力された PID を生存中の Play Host と照合し、応答まで Handle を保持する
Result<ide_detail::IdeHandle> validate_target(std::uint32_t a_processId, const Path &a_executable)
{
    if (a_processId == 0 || a_processId == GetCurrentProcessId())
        return Result<ide_detail::IdeHandle>::failure({ErrorCategory::InvalidArgument, "EditorIde.target_pid"});
    auto expected = utf8_to_utf16(a_executable.utf8());
    if (!expected.has_value())
        return Result<ide_detail::IdeHandle>::failure(*expected.try_error());
    ide_detail::IdeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, a_processId));
    if (!process)
        return Result<ide_detail::IdeHandle>::failure(ide_detail::process_error("EditorIde.target_process"));
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
        return Result<ide_detail::IdeHandle>::failure(ide_detail::process_error("EditorIde.target_image"));
    auto text = utf16_to_utf8(std::wstring_view(image.data(), length));
    auto actual = text.has_value() ? Path::create(*text.try_value()) : Result<Path>::failure(*text.try_error());
    auto canonical = actual.has_value() ? utf8_to_utf16(actual.try_value()->utf8())
                                        : Result<std::wstring>::failure(*actual.try_error());
    if (!canonical.has_value() ||
        CompareStringOrdinal(canonical.try_value()->c_str(), -1, expected.try_value()->c_str(), -1, TRUE) !=
            CSTR_EQUAL ||
        WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        return Result<ide_detail::IdeHandle>::failure({ErrorCategory::InvalidState, "EditorIde.target_identity"});
    return Result<ide_detail::IdeHandle>::success(std::move(process));
}

/// @brief C# Bridge を一要求ずつ所有し、呼出 Frame に COM の待機を持ち込まない
class WindowsIdeService final : public IEditorIdeService
{
  public:
    /// @brief Factory が検証した配置だけを所有する
    WindowsIdeService(WindowsIdeConfig a_config, std::unique_ptr<IFileSystem> a_files)
        : m_config(std::move(a_config)), m_files(std::move(a_files)), m_owner(std::this_thread::get_id())
    {
    }
    /// @brief 未終了 Bridge を Job 単位で回収する。IDE は所有しない
    ~WindowsIdeService() override
    {
        m_process = {};
    }
    /// @brief 指定 Solution の VS を再利用または起動する
    Result<void> open_solution(const Path &a_solution) override
    {
        return start(IdeOperation::OpenSolution, a_solution, {}, 1, 0, {});
    }
    /// @brief Source 表示要求を独立した所有値にする
    Result<void> open_source(const Path &a_solution, const Path &a_source, std::uint32_t a_line) override
    {
        return start(IdeOperation::OpenSource, a_solution, a_source, a_line, 0, {});
    }
    /// @brief 自分で起動した Play Host にだけ接続する
    Result<void> attach_debugger(const Path &a_solution, std::uint32_t a_processId, const Path &a_expected) override
    {
        return start(IdeOperation::AttachDebugger, a_solution, {}, 1, a_processId, a_expected);
    }
    /// @brief 一回の出力予算と終了状態を待機せず読む
    Result<void> poll() override;
    /// @brief IDE を残して所有 Bridge だけを取り消す
    Result<void> cancel() override;
    /// @brief 所有 Process の回収を要求し、この Service への再入を拒否する
    Result<void> shutdown() override;
    /// @brief 次の操作まで現在値を借用する
    const IdeStatus &status() const noexcept override
    {
        return m_status;
    }

  private:
    /// @brief Owner / 停止 / 二重要求の前提を検証して Bridge を開始する
    Result<void> start(IdeOperation a_operation, const Path &a_solution, const Path &a_source, std::uint32_t a_line,
                       std::uint32_t a_pid, const Path &a_expected);
    /// @brief 要求 ID と Operation を照合し、成功値を部分公開しない
    Result<void> parse_response(DWORD a_exitCode);
    /// @brief 所有 Job を閉じ、最終診断を保持する
    Result<void> fail(Error a_error);
    /// @brief 公開操作の呼出元を検証する
    Result<void> check_access() const;
    WindowsIdeConfig m_config;
    std::unique_ptr<IFileSystem> m_files;
    std::thread::id m_owner;
    IdeStatus m_status;
    std::uint64_t m_nextRequest = 0;
    std::uint64_t m_deadline = 0;
    Path m_pinnedSolution;
    std::uint32_t m_pinnedIde = 0;
    ide_detail::IdeHandle m_target;
    ide_detail::IdeProcess m_process;
    std::string m_response;
    bool m_isCancelled = false;
    bool m_isClosed = false;
};

Result<void> WindowsIdeService::check_access() const
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorIde.owner_thread"});
    if (m_isClosed)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorIde.closed"});
    return Result<void>::success();
}
Result<void> WindowsIdeService::start(IdeOperation a_operation, const Path &a_solution, const Path &a_source,
                                      std::uint32_t a_line, std::uint32_t a_pid, const Path &a_expected)
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (m_process.process)
        return Result<void>::failure({ErrorCategory::InvalidState, "EditorIde.busy"});
    try
    {
        auto checked = validate_file(*m_files, a_solution);
        if (!checked.has_value())
            return checked;
        if (a_solution.extension() != ".sln" && a_solution.extension() != ".slnx")
            return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorIde.solution_extension"});
        if (a_line == 0 || a_line > 10000000)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorIde.source_line"});
        if (a_operation == IdeOperation::OpenSource)
        {
            checked = validate_file(*m_files, a_source);
            if (!checked.has_value())
                return checked;
        }
        ide_detail::IdeHandle target;
        if (a_operation == IdeOperation::AttachDebugger)
        {
            checked = validate_file(*m_files, a_expected);
            if (!checked.has_value())
                return checked;
            auto name = utf8_to_utf16(a_expected.filename());
            if (!name.has_value() ||
                CompareStringOrdinal(name.try_value()->c_str(), -1, L"CuePlayHost.exe", -1, TRUE) != CSTR_EQUAL)
                return Result<void>::failure({ErrorCategory::InvalidArgument, "EditorIde.only_play_host"});
            auto validated = validate_target(a_pid, a_expected);
            if (!validated.has_value())
                return Result<void>::failure(*validated.try_error());
            target = validated.take_value();
        }
        checked = validate_file(*m_files, m_config.bridgeExecutable);
        if (!checked.has_value())
            return checked;
        const auto request = ++m_nextRequest;
        std::vector<std::string> arguments{"--command",    operation_name(a_operation), "--solution", a_solution.utf8(),
                                           "--request-id", std::to_string(request)};
        if (m_pinnedIde && m_pinnedSolution.utf8() == a_solution.utf8())
        {
            arguments.insert(arguments.end(), {"--ide-process-id", std::to_string(m_pinnedIde)});
        }
        if (a_operation == IdeOperation::OpenSource)
        {
            arguments.insert(arguments.end(), {"--source", a_source.utf8(), "--line", std::to_string(a_line)});
        }
        if (a_operation == IdeOperation::AttachDebugger)
        {
            arguments.insert(arguments.end(),
                             {"--process-id", std::to_string(a_pid), "--expected-executable", a_expected.utf8()});
        }
        auto process = ide_detail::start_bridge(m_config.bridgeExecutable, arguments);
        if (!process.has_value())
            return Result<void>::failure(*process.try_error());
        m_status = {a_operation, IdeState::Running, request, a_solution, a_source, a_line, a_pid};
        m_process = process.take_value();
        m_target = std::move(target);
        m_response.clear();
        m_isCancelled = false;
        m_deadline = GetTickCount64() + m_config.timeoutMilliseconds;
        return Result<void>::success();
    }
    catch (...)
    {
        return fail({ErrorCategory::PlatformFailure, "EditorIde.start_exception"});
    }
}
Result<void> WindowsIdeService::fail(Error a_error)
{
    m_process = {};
    m_target.reset();
    m_status.state = IdeState::Failed;
    m_status.error = a_error;
    m_status.message = a_error.operation;
    return Result<void>::failure(std::move(a_error));
}
Result<void> WindowsIdeService::parse_response(DWORD a_exitCode)
{
    constexpr std::array<std::string_view, 7> names{"formatVersion", "requestId",       "operation", "success",
                                                    "ideProcessId",  "targetProcessId", "message"};
    std::array<std::string_view, 7> values{};
    std::array<bool, 7> seen{};
    if (m_response.empty() || !is_valid_utf8_text(m_response))
        return fail({ErrorCategory::PlatformFailure, "EditorIde.invalid_response", a_exitCode});
    const std::string_view text(m_response);
    for (std::size_t offset = 0; offset < text.size();)
    {
        const auto end = text.find('\n', offset);
        auto line = text.substr(offset, end == std::string_view::npos ? end : end - offset);
        offset = end == std::string_view::npos ? text.size() : end + 1;
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        const auto separator = line.find('=');
        const auto key = line.substr(0, separator);
        const auto position = std::find(names.begin(), names.end(), key);
        if (separator == std::string_view::npos || position == names.end())
            return fail({ErrorCategory::PlatformFailure, "EditorIde.response_schema", a_exitCode});
        const auto index = static_cast<std::size_t>(position - names.begin());
        if (seen[index])
            return fail({ErrorCategory::PlatformFailure, "EditorIde.response_duplicate", a_exitCode});
        seen[index] = true;
        values[index] = line.substr(separator + 1);
    }
    std::uint64_t request = 0;
    std::uint32_t ide = 0, target = 0;
    if (std::find(seen.begin(), seen.end(), false) != seen.end() || values[0] != "1" ||
        !parse_number(values[1], request) || request != m_status.requestId ||
        values[2] != operation_name(m_status.operation) || !parse_number(values[4], ide) ||
        !parse_number(values[5], target) || target != m_status.targetProcessId ||
        (values[3] != "true" && values[3] != "false") || values[6].size() > 8192)
        return fail({ErrorCategory::PlatformFailure, "EditorIde.response_identity", a_exitCode});
    if (a_exitCode != 0 || values[3] != "true" || ide == 0)
        return fail({ErrorCategory::PlatformFailure, "EditorIde.bridge_failed:" + std::string(values[6]), a_exitCode});
    m_status.ideProcessId = ide;
    m_status.message = values[6];
    m_pinnedSolution = m_status.solution;
    m_pinnedIde = ide;
    m_status.state = IdeState::Succeeded;
    return Result<void>::success();
}
Result<void> WindowsIdeService::poll()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_process.process)
        return Result<void>::success();
    try
    {
        std::array<char, 8192> bytes{};
        std::size_t budget = k_maxResponseBytes;
        while (m_process.output && budget)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(m_process.output.get(), nullptr, 0, nullptr, &available, nullptr))
            {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                {
                    m_process.output.reset();
                    break;
                }
                return fail(ide_detail::process_error("EditorIde.peek"));
            }
            if (!available)
                break;
            DWORD read = 0;
            const auto count =
                static_cast<DWORD>((std::min)({budget, bytes.size(), static_cast<std::size_t>(available)}));
            if (!ReadFile(m_process.output.get(), bytes.data(), count, &read, nullptr))
                return fail(ide_detail::process_error("EditorIde.read"));
            if (read == 0)
                break;
            if (m_response.size() + read > k_maxResponseBytes)
                return fail({ErrorCategory::PlatformFailure, "EditorIde.response_limit"});
            m_response.append(bytes.data(), read);
            budget -= read;
        }
        if (m_target && WaitForSingleObject(m_target.get(), 0) != WAIT_TIMEOUT)
            return fail({ErrorCategory::PlatformFailure, "EditorIde.target_exited"});
        const auto waited = WaitForSingleObject(m_process.process.get(), 0);
        if (waited == WAIT_TIMEOUT)
        {
            if (GetTickCount64() >= m_deadline)
                return fail({ErrorCategory::PlatformFailure, "EditorIde.timeout"});
            return Result<void>::success();
        }
        if (waited != WAIT_OBJECT_0)
            return fail(ide_detail::process_error("EditorIde.wait"));
        // 終了時も残り出力があれば次の予算へ回し、末尾を取りこぼさない
        DWORD available = 0;
        if (m_process.output && PeekNamedPipe(m_process.output.get(), nullptr, 0, nullptr, &available, nullptr) &&
            available)
            return Result<void>::success();
        DWORD code = 1;
        if (!GetExitCodeProcess(m_process.process.get(), &code))
            return fail(ide_detail::process_error("EditorIde.exit_code"));
        m_process = {};
        m_target.reset();
        if (m_isCancelled)
        {
            m_status.state = IdeState::Cancelled;
            m_status.message = "IDE request cancelled";
            return Result<void>::success();
        }
        return parse_response(code);
    }
    catch (...)
    {
        return fail({ErrorCategory::PlatformFailure, "EditorIde.poll_exception"});
    }
}
Result<void> WindowsIdeService::cancel()
{
    auto access = check_access();
    if (!access.has_value())
        return access;
    if (!m_process.process || m_isCancelled)
        return Result<void>::success();
    if (!TerminateJobObject(m_process.job.get(), ERROR_CANCELLED))
        return Result<void>::failure(ide_detail::process_error("EditorIde.cancel"));
    m_isCancelled = true;
    return Result<void>::success();
}
Result<void> WindowsIdeService::shutdown()
{
    if (std::this_thread::get_id() != m_owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "EditorIde.owner_thread"});
    m_process = {};
    m_target.reset();
    if (m_status.state == IdeState::Running)
        m_status.state = IdeState::Cancelled;
    m_isClosed = true;
    return Result<void>::success();
}
} // namespace

Result<std::unique_ptr<IEditorIdeService>> create_windows_ide_service(WindowsIdeConfig a_config)
{
    try
    {
        if (a_config.timeoutMilliseconds < 10 || a_config.timeoutMilliseconds > 300000)
            return Result<std::unique_ptr<IEditorIdeService>>::failure(
                {ErrorCategory::InvalidArgument, "EditorIde.timeout_range"});
        auto files = create_windows_file_system();
        if (!files.has_value())
            return Result<std::unique_ptr<IEditorIdeService>>::failure(*files.try_error());
        auto valid = validate_file(**files.try_value(), a_config.bridgeExecutable);
        if (!valid.has_value())
            return Result<std::unique_ptr<IEditorIdeService>>::failure(*valid.try_error());
        return Result<std::unique_ptr<IEditorIdeService>>::success(
            std::make_unique<WindowsIdeService>(std::move(a_config), files.take_value()));
    }
    catch (...)
    {
        return Result<std::unique_ptr<IEditorIdeService>>::failure(
            {ErrorCategory::PlatformFailure, "EditorIde.create_exception"});
    }
}
} // namespace cue
