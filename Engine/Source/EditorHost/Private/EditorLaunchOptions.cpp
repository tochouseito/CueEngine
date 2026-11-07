#include <EditorHost/EditorLaunchOptions.h>

#include <new>

namespace cue
{
Result<EditorLaunchOptions> parse_editor_arguments(std::span<const std::string_view> a_arguments)
{
    try
    {
        // 引数なしは未選択の Editor を開始し、File Menu からの Project 指定を待つ
        if (a_arguments.empty())
            return Result<EditorLaunchOptions>::success({});
        if (a_arguments.size() != 2 || a_arguments[0] != "--project" || a_arguments[1].starts_with("--"))
        {
            return Result<EditorLaunchOptions>::failure(
                {ErrorCategory::InvalidArgument, "Editor.arguments: expected --project <file.cueproj>"});
        }
        auto path = Path::create(a_arguments[1]);
        if (!path.has_value() || path.try_value()->extension() != ".cueproj")
        {
            return Result<EditorLaunchOptions>::failure(
                {ErrorCategory::InvalidArgument, "Editor.arguments: invalid .cueproj path"});
        }
        return Result<EditorLaunchOptions>::success({path.take_value()});
    }
    catch (const std::bad_alloc &)
    {
        return Result<EditorLaunchOptions>::failure({ErrorCategory::PlatformFailure, "Editor.arguments.allocation"});
    }
}
} // namespace cue
