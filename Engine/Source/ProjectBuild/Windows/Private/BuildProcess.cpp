#include "BuildProcess.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cwchar>
#include <memory>
#include <string_view>

#include <Foundation/Windows/UtfConversion.h>

namespace cue::detail
{
namespace
{
/// @brief CommandLineToArgvW / CRT の引用規則で一つの引数を連結する
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

/// @brief 起動 Shell に VCPKG_ROOT がなくても Preset の展開先を固定する
Result<std::vector<wchar_t>> environment(const Path &a_vcpkgRoot)
{
    auto root = utf8_to_utf16(a_vcpkgRoot.utf8());
    if (!root.has_value())
        return Result<std::vector<wchar_t>>::failure(*root.try_error());
    auto *native = GetEnvironmentStringsW();
    if (!native)
        return Result<std::vector<wchar_t>>::failure(build_process_error("GameBuild.environment"));
    const auto free = [](wchar_t *a_value) { FreeEnvironmentStringsW(a_value); };
    std::unique_ptr<wchar_t, decltype(free)> owned(native, free);
    std::vector<std::wstring> entries;
    for (const auto *cursor = native; *cursor; cursor += std::wcslen(cursor) + 1)
    {
        std::wstring_view entry(cursor);
        const auto end = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
        const auto key = entry.substr(0, end);
        if (CompareStringOrdinal(key.data(), static_cast<int>(key.size()), L"VCPKG_ROOT", 10, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(key.data(), static_cast<int>(key.size()), L"VSLANG", 6, TRUE) == CSTR_EQUAL)
            continue;
        entries.emplace_back(entry);
    }
    entries.push_back(L"VCPKG_ROOT=" + *root.try_value());
    // MSBuild の診断本文を共通の英語にし、Path の UTF-8 / ACP は表示時に正規化する
    entries.emplace_back(L"VSLANG=1033");
    std::sort(entries.begin(), entries.end(),
              [](const auto &a_left, const auto &a_right)
              {
                  return CompareStringOrdinal(a_left.data(), static_cast<int>(a_left.size()), a_right.data(),
                                              static_cast<int>(a_right.size()), TRUE) == CSTR_LESS_THAN;
              });
    std::vector<wchar_t> result;
    for (const auto &entry : entries)
    {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(0);
    }
    result.push_back(0);
    return Result<std::vector<wchar_t>>::success(std::move(result));
}
} // namespace

Error build_process_error(const char *a_operation, DWORD a_code)
{
    return {ErrorCategory::PlatformFailure, a_operation, a_code};
}

std::string build_path_identity(const Path &a_path)
{
    std::uint64_t value = 14695981039346656037ull;
    auto native = utf8_to_utf16(a_path.utf8());
    if (!native.has_value())
        return {};
    // Windows の大小文字を統一し、別表記で同じ Package の Lock を迂回させない
    auto text = native.take_value();
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    for (const auto character : text)
    {
        value ^= static_cast<std::uint16_t>(character);
        value *= 1099511628211ull;
    }
    std::array<char, 17> textValue{};
    const auto result = std::to_chars(textValue.data(), textValue.data() + 16, value, 16);
    return std::string(textValue.data(), result.ptr);
}

Result<BuildHandle> acquire_build_lock(const Path &a_projectRoot)
{
    auto file = a_projectRoot.join("Intermediate/.cue-editor-build.lock");
    if (!file.has_value())
        return Result<BuildHandle>::failure(*file.try_error());
    auto native = utf8_to_utf16(file.try_value()->utf8());
    if (!native.has_value())
        return Result<BuildHandle>::failure(*native.try_error());
    // Mutex の再帰獲得では同じ Main Thread の別 Service を排除できないため、共有なし File を使う
    BuildHandle lock(CreateFileW(native.try_value()->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock)
        return Result<BuildHandle>::failure(build_process_error("GameBuild.project_busy"));
    // 排他 File そのものも Link として辿らず、Directory / Hard Link による別用途の Handle を拒否する
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(lock.get(), &information))
        return Result<BuildHandle>::failure(build_process_error("GameBuild.lock_information"));
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        GetFileType(lock.get()) != FILE_TYPE_DISK || information.nNumberOfLinks != 1)
        return Result<BuildHandle>::failure({ErrorCategory::InvalidArgument, "GameBuild.lock_file_kind"});
    return Result<BuildHandle>::success(std::move(lock));
}

void release_build_lock(BuildHandle &a_lock) noexcept
{
    a_lock.reset();
}

Result<BuildProcess> start_build_process(const Path &a_executable, std::span<const std::string> a_arguments,
                                         const Path &a_directory, const Path &a_vcpkgRoot)
{
    auto executable = utf8_to_utf16(a_executable.utf8());
    auto directory = utf8_to_utf16(a_directory.utf8());
    auto variables = environment(a_vcpkgRoot);
    if (!executable.has_value() || !directory.has_value() || !variables.has_value())
        return Result<BuildProcess>::failure({ErrorCategory::InvalidArgument, "GameBuild.process_paths"});
    std::wstring line;
    append_argument(line, *executable.try_value());
    for (const auto &argument : a_arguments)
    {
        auto converted = utf8_to_utf16(argument);
        if (!converted.has_value())
            return Result<BuildProcess>::failure(*converted.try_error());
        append_argument(line, *converted.try_value());
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.pipe"));
    BuildHandle output(read), writer(write);
    if (!SetHandleInformation(output.get(), HANDLE_FLAG_INHERIT, 0))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.pipe_inherit"));
    BuildHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input)
        return Result<BuildProcess>::failure(build_process_error("GameBuild.input"));
    BuildHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.job"));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.attributes"));
    const auto destroy = [](LPPROC_THREAD_ATTRIBUTE_LIST a_value) { DeleteProcThreadAttributeList(a_value); };
    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(destroy)> owned(attributes, destroy);
    const std::array<HANDLE, 2> inherited{writer.get(), input.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   const_cast<HANDLE *>(inherited.data()), sizeof(inherited), nullptr, nullptr))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.handle_list"));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.get();
    startup.StartupInfo.hStdInput = input.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.try_value()->c_str(), line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        variables.try_value()->data(), directory.try_value()->c_str(), &startup.StartupInfo, &process))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.create_process"));
    BuildHandle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.get(), child.get()))
    {
        const auto error = build_process_error("GameBuild.assign_job");
        TerminateProcess(child.get(), 1);
        return Result<BuildProcess>::failure(error);
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        return Result<BuildProcess>::failure(build_process_error("GameBuild.resume"));
    return Result<BuildProcess>::success({std::move(job), std::move(child), std::move(output)});
}

Result<BuildHandle> start_game_process(const Path &a_executable, const Path &a_directory)
{
    auto executable = utf8_to_utf16(a_executable.utf8());
    auto directory = utf8_to_utf16(a_directory.utf8());
    if (!executable.has_value() || !directory.has_value())
        return Result<BuildHandle>::failure({ErrorCategory::InvalidArgument, "GameBuild.launch_path"});
    std::wstring line;
    append_argument(line, *executable.try_value());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.try_value()->c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.try_value()->c_str(), &startup, &process))
        return Result<BuildHandle>::failure(build_process_error("GameBuild.launch"));
    BuildHandle thread(process.hThread);
    return Result<BuildHandle>::success(BuildHandle(process.hProcess));
}
} // namespace cue::detail
