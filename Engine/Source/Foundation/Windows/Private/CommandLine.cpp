#include <Foundation/Windows/CommandLine.h>

#include <cstddef>
#include <utility>

#include <Foundation/Windows/UtfConversion.h>

namespace cue
{
namespace
{
/// @brief 引用符の直前と末尾のバックスラッシュを増やし、CRT 側で元の引数を復元させる
void append_argument(std::wstring &a_line, std::wstring_view a_value)
{
    a_line.append(L" \"");
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

Result<std::wstring> make_windows_command_line(std::wstring_view a_executable,
                                               std::span<const std::string> a_arguments)
{
    using commandLineResult = Result<std::wstring>;
    if (a_executable.empty() || a_executable.find(L'\0') != std::wstring_view::npos ||
        a_executable.find(L'"') != std::wstring_view::npos)
    {
        return commandLineResult::failure({ErrorCategory::InvalidArgument, "WindowsCommandLine.executable"});
    }

    // argv[0] には通常の引数と異なる規則が適用される
    std::wstring line = L"\"";
    line.append(a_executable);
    line += L'"';
    for (const auto &argument : a_arguments)
    {
        if (argument.find('\0') != std::string::npos)
        {
            return commandLineResult::failure({ErrorCategory::InvalidArgument, "WindowsCommandLine.argument"});
        }
        auto converted = utf8_to_utf16(argument);
        if (const auto *error = converted.try_error())
        {
            return commandLineResult::failure(*error);
        }
        append_argument(line, *converted.try_value());
    }
    return commandLineResult::success(std::move(line));
}
} // namespace cue
