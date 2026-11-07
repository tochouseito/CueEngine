#include <EditorHost/EditorLaunchOptions.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

/// @brief 引数なしの未選択、Path の所有化と不正な明示指定の拒否を確認する
int main()
{
    std::string text = "Samples/回転 Cube/RotateCube.cueproj";
    const std::array<std::string_view, 2> arguments{"--project", text};
    auto result = cue::parse_editor_arguments(arguments);
    text = "changed";
    if (!result.has_value() || result.try_value()->projectFile.utf8() != "Samples/回転 Cube/RotateCube.cueproj")
    {
        return __LINE__;
    }
    const std::array<std::string_view, 2> absolute{"--project", "C:/Games/RotateCube/RotateCube.cueproj"};
    if (!cue::parse_editor_arguments(absolute).has_value())
    {
        return __LINE__;
    }
    auto unspecified = cue::parse_editor_arguments({});
    if (!unspecified.has_value() || !unspecified.try_value()->projectFile.is_empty())
        return __LINE__;
    const std::vector<std::vector<std::string_view>> invalid{{"--project"},
                                                             {"--project", ""},
                                                             {"--project", "bad.txt"},
                                                             {"--project", "--project"},
                                                             {"--unknown", "a.cueproj"},
                                                             {"a.cueproj"},
                                                             {"--project", "a.cueproj", "--project", "b.cueproj"},
                                                             {"--project", "a.cueproj", "extra"},
                                                             {"--project", std::string_view("a\0.cueproj", 10)}};
    for (std::size_t index = 0; index < invalid.size(); ++index)
    {
        if (cue::parse_editor_arguments(invalid[index]).has_value())
        {
            std::fprintf(stderr, "Unexpected arguments accepted: %zu\n", index);
            return __LINE__;
        }
    }
    return 0;
}
