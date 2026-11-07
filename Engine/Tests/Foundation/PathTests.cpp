#include <Foundation/Path.h>

#include <cstdio>
#include <string>

namespace
{
/// @brief Root と Unicode を含む字句操作、不正入力、結合時の Root 保全を確認する
int run_tests()
{
    using cue::Path;
    struct Case final
    {
        const char *input;
        const char *output;
        const char *parent;
    };
    for (const auto &entry :
         {Case{"C:\\資料\\.\\a\\..\\shader.hlsl", "C:/資料/shader.hlsl", "C:/資料"}, Case{"a//b/../c", "a/c", "a"},
          Case{"../a/../../b", "../../b", "../.."}, Case{"C:/", "C:/", "C:/"},
          Case{"//server/share/a/..", "//server/share/", "//server/share/"}, Case{"a/..", ".", "."},
          Case{"/a", "/a", "/"}})
    {
        auto path = Path::create(entry.input);
        if (!path.has_value() || path.try_value()->utf8() != entry.output ||
            path.try_value()->parent().utf8() != entry.parent)
        {
            return __LINE__;
        }
    }
    for (const auto &text : {std::string{}, std::string("C:foo"), std::string("C:/../escape"), std::string("//server"),
                             std::string("//server/share/../escape"), std::string("\\\\?\\C:\\test"),
                             std::string("a\0b", 3), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                             std::string("\xf4\x90\x80\x80"), std::string("\xe3\x81")})
    {
        if (Path::create(text).has_value())
        {
            return __LINE__;
        }
    }
    auto file = Path::create("C:/a/name.tar.gz");
    if (!file.has_value() || file.try_value()->filename() != "name.tar.gz" || file.try_value()->stem() != "name.tar" ||
        file.try_value()->extension() != ".gz")
    {
        return __LINE__;
    }
    auto hidden = Path::create(".gitignore");
    if (!hidden.has_value() || !hidden.try_value()->extension().empty())
    {
        return __LINE__;
    }
    auto joined = file.try_value()->parent().join("../資料.hlsl");
    auto absolute = file.try_value()->join("D:/other");
    if (!joined.has_value() || joined.try_value()->utf8() != "C:/資料.hlsl" || !absolute.has_value() ||
        absolute.try_value()->utf8() != "D:/other" || file.try_value()->join("").has_value() || !Path{}.is_empty())
    {
        return __LINE__;
    }
    return 0;
}
} // namespace
/// @brief Foundation 単独で Path の契約を検証する
int main()
{
    const auto result = run_tests();
    if (result)
    {
        std::fprintf(stderr, "PathTests line %d\n", result);
    }
    return result;
}
