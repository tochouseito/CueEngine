#include "IdeProcess.h"

#include <array>
#include <memory>
#include <vector>

#include <Foundation/Windows/UtfConversion.h>

namespace cue::ide_detail
{
namespace
{
/// @brief CRT と CommandLineToArgvW が同じ一引数を復元できる形へ Quote する
void append_argument(std::wstring &a_line, std::wstring_view a_value)
{
    if (!a_line.empty())
        a_line += L' ';
    a_line += L'"';
    std::size_t slashes = 0;
    for (const auto character : a_value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        a_line.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        a_line += character;
        slashes = 0;
    }
    a_line.append(slashes * 2, L'\\');
    a_line += L'"';
}
} // namespace
Error process_error(const char *a_operation, DWORD a_code)
{
    return {ErrorCategory::PlatformFailure, a_operation, a_code};
}
Result<IdeProcess> start_bridge(const Path &a_executable, std::span<const std::string> a_arguments)
{
    auto executable = utf8_to_utf16(a_executable.utf8());
    auto directory = utf8_to_utf16(a_executable.parent().utf8());
    if (!executable.has_value() || !directory.has_value())
        return Result<IdeProcess>::failure({ErrorCategory::InvalidArgument, "EditorIde.bridge_path"});
    std::wstring line;
    append_argument(line, *executable.try_value());
    for (const auto &argument : a_arguments)
    {
        auto converted = utf8_to_utf16(argument);
        if (!converted.has_value())
            return Result<IdeProcess>::failure(*converted.try_error());
        append_argument(line, *converted.try_value());
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0))
        return Result<IdeProcess>::failure(process_error("EditorIde.pipe"));
    IdeHandle output(read), writer(write);
    if (!SetHandleInformation(output.get(), HANDLE_FLAG_INHERIT, 0))
        return Result<IdeProcess>::failure(process_error("EditorIde.pipe_inherit"));
    IdeHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    IdeHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // Bridge だけを Cancel し、そこから起動した Visual Studio を巻き込まない
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!input || !job ||
        !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Result<IdeProcess>::failure(process_error("EditorIde.job"));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        return Result<IdeProcess>::failure(process_error("EditorIde.attributes"));
    const auto destroy = [](LPPROC_THREAD_ATTRIBUTE_LIST a_value) { DeleteProcThreadAttributeList(a_value); };
    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(destroy)> owned(attributes, destroy);
    std::array<HANDLE, 2> inherited{writer.get(), input.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(inherited), nullptr, nullptr))
        return Result<IdeProcess>::failure(process_error("EditorIde.handle_list"));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.get();
    startup.StartupInfo.hStdInput = input.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.try_value()->c_str(), line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        directory.try_value()->c_str(), &startup.StartupInfo, &process))
        return Result<IdeProcess>::failure(process_error("EditorIde.create_bridge"));
    IdeHandle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.get(), child.get()))
    {
        const auto error = process_error("EditorIde.assign_job");
        TerminateProcess(child.get(), 1);
        return Result<IdeProcess>::failure(error);
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        return Result<IdeProcess>::failure(process_error("EditorIde.resume"));
    return Result<IdeProcess>::success({std::move(job), std::move(child), std::move(output)});
}
} // namespace cue::ide_detail
