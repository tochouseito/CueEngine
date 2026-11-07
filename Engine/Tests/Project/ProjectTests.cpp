#include <Project/Project.h>

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
constexpr std::string_view k_descriptor =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\n"
    "name=RotateCube\napplicationName=RotateCube\nsourceRoot=Source\ncontentRoot=Content\n";

/// @brief 固定の Fixture Path の成功結果を所有し、Result の参照修飾を保って値を取り出す
cue::Path test_path(cue::Result<cue::Path> a_result)
{
    return a_result.take_value();
}

/// @brief Test 用の設定値を既存 FileSystem 経由で保存する
bool write_text(cue::IFileSystem &a_files, const cue::Path &a_path, std::string_view a_text)
{
    return a_files.write_all(a_path, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief 既知の設定値を一箇所だけ置換し、異常系の入力を作る
std::string replace_value(std::string_view a_before, std::string_view a_after)
{
    std::string text(k_descriptor);
    text.replace(text.find(a_before), a_before.size(), a_after);
    return text;
}

/// @brief Unicode / 空白を含む配置、絶対化回数、移動後の ID と所有値の寿命を検証する
int test_loading(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto directory = test_path(a_fixture.root.join("日本語 Project"));
    const auto path = test_path(directory.join("回転 Cube.cueproj"));
    if (!files.create_directories(test_path(directory.join("Source"))).has_value() ||
        !files.create_directories(test_path(directory.join("Content"))).has_value() ||
        !write_text(files, path, k_descriptor))
    {
        return __LINE__;
    }
    cue::tests::FileSystemProbe probe(files);
    auto result = cue::Project::load(probe, path);
    if (!result.has_value() || probe.absoluteCount != 1 || result.try_value()->root().utf8() != directory.utf8() ||
        result.try_value()->source_directory().utf8() != directory.utf8() + "/Source" ||
        result.try_value()->content_directory().utf8() != directory.utf8() + "/Content" ||
        result.try_value()->descriptor().sourceRoot.utf8() != "Source" ||
        result.try_value()->descriptor().name != "RotateCube" || result.try_value()->descriptor().startupScene ||
        result.try_value()->startup_scene_file() != nullptr)
    {
        return __LINE__;
    }
    const auto project = result.take_value();
    // 元の File が変わっても、公開した Project の所有値は書き換わらない
    if (!write_text(files, path, replace_value("name=RotateCube", "name=回転 Cube")))
    {
        return __LINE__;
    }
    auto changed = cue::Project::load(files, path);
    if (!changed.has_value() || changed.try_value()->descriptor().name != "回転 Cube" ||
        project.descriptor().name != "RotateCube")
    {
        return __LINE__;
    }
    if (!write_text(files, path, replace_value("name=RotateCube", "name=A:B")) ||
        !cue::Project::load(files, path).has_value() ||
        !write_text(files, path, replace_value("name=RotateCube", "name=回転 Cube")))
    {
        return __LINE__;
    }
    const auto movedDirectory = test_path(a_fixture.root.join("Moved"));
    if (!files.rename(directory, movedDirectory).has_value())
    {
        return __LINE__;
    }
    auto moved = cue::Project::load(files, test_path(movedDirectory.join("回転 Cube.cueproj")));
    if (!moved.has_value() || moved.try_value()->descriptor().projectId != project.descriptor().projectId ||
        moved.try_value()->root().utf8() != movedDirectory.utf8())
    {
        return __LINE__;
    }
    return 0;
}

/// @brief 起動 CWD に対する相対指定を解決し、Project 読み込みが CWD を変更しないことを確認する
int test_relative_path(cue::tests::TemporaryFiles &a_fixture)
{
    std::wstring previous(32768, L'\0');
    const auto count = GetCurrentDirectoryW(static_cast<DWORD>(previous.size()), previous.data());
    if (!count || count >= previous.size())
    {
        return __LINE__;
    }
    previous.resize(count);
    // Fixture の破棄より先に CWD を復元し、早期 return でも一時 Directory を利用中にしない
    const auto restore = [](const wchar_t *a_directory) { SetCurrentDirectoryW(a_directory); };
    std::unique_ptr<const wchar_t, decltype(restore)> guard(previous.c_str(), restore);
    auto native = cue::utf8_to_utf16(a_fixture.root.utf8());
    if (!native.has_value() || !SetCurrentDirectoryW(native.try_value()->c_str()))
    {
        return __LINE__;
    }
    auto relative = test_path(cue::Path::create("Moved/回転 Cube.cueproj"));
    auto expected = a_fixture.files->absolute_path(relative);
    auto project = cue::Project::load(*a_fixture.files, relative);
    auto cwd = a_fixture.files->absolute_path(test_path(cue::Path::create(".")));
    if (!expected.has_value() || !project.has_value() || !cwd.has_value() ||
        cwd.try_value()->utf8() != a_fixture.root.utf8() ||
        project.try_value()->file().utf8() != expected.try_value()->utf8())
    {
        return __LINE__;
    }
    return 0;
}

/// @brief 形式異常・Version・重複・Root 外参照と I/O 失敗で部分 Project を返さないことを確認する
int test_failures(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto path = test_path(a_fixture.root.join("Moved/回転 Cube.cueproj"));
    const std::array badValues{std::string{},
                               std::string("invalid line"),
                               replace_value("formatVersion=1", "formatVersion=2"),
                               replace_value("projectId=7f533c49-d416-47ea-98c1-026822b1e6ed", "projectId=invalid"),
                               replace_value("projectId=7f533c49-d416-47ea-98c1-026822b1e6ed",
                                             "projectId=00000000-0000-0000-0000-000000000000"),
                               replace_value("name=RotateCube", "name="),
                               replace_value("name=RotateCube", "name=\xff"),
                               replace_value("applicationName=RotateCube", "applicationName=../Other"),
                               replace_value("sourceRoot=Source", "sourceRoot=../Other"),
                               replace_value("sourceRoot=Source", "sourceRoot=C:/Other"),
                               replace_value("sourceRoot=Source", "sourceRoot=Source/../../Other"),
                               replace_value("contentRoot=Content", "contentRoot=Missing"),
                               std::string(k_descriptor) + "name=Duplicate\n",
                               std::string(k_descriptor) + "unknown=value\n",
                               std::string(k_descriptor) + "#\xff\n",
                               std::string(k_descriptor) + std::string("#\0comment", 9),
                               std::string(64 * 1024 + 1, 'x')};
    for (std::size_t index = 0; index < badValues.size(); ++index)
    {
        if (!write_text(files, path, badValues[index]) || cue::Project::load(files, path).has_value())
        {
            std::fprintf(stderr, "Unexpected Project result for bad value %zu\n", index);
            return __LINE__;
        }
    }
    // 各必須項目の欠損と埋込み NUL を個別に拒否する
    const std::array<std::string_view, 6> keys{"formatVersion",   "projectId",  "name",
                                               "applicationName", "sourceRoot", "contentRoot"};
    for (const auto key : keys)
    {
        std::string text(k_descriptor);
        const auto begin = text.find(std::string(key) + '=');
        text.erase(begin, text.find('\n', begin) - begin + 1);
        if (!write_text(files, path, text) || cue::Project::load(files, path).has_value())
        {
            return __LINE__;
        }
    }
    auto nul = replace_value("name=RotateCube", std::string_view("name=Cube\0Extra", 15));
    if (!write_text(files, path, nul) || cue::Project::load(files, path).has_value() ||
        !write_text(files, path, std::string("\xef\xbb\xbf# comment\r\n\r\n") + std::string(k_descriptor)))
    {
        return __LINE__;
    }
    if (!cue::Project::load(files, path).has_value() ||
        cue::Project::load(files, test_path(a_fixture.root.join("missing.cueproj"))).has_value() ||
        cue::Project::load(files, test_path(a_fixture.root.join("invalid.txt"))).has_value())
    {
        return __LINE__;
    }
    // 上限ちょうどの File は読める。超過だけを形式の異常と混同せず I/O 上限で拒否する
    const auto atLimit = std::string(k_descriptor) + '#' + std::string(64 * 1024 - k_descriptor.size() - 1, 'x');
    if (!write_text(files, path, atLimit) || !cue::Project::load(files, path).has_value())
    {
        return __LINE__;
    }
    cue::tests::FileSystemProbe probe(files);
    probe.failRead = true;
    auto failedRead = cue::Project::load(probe, path);
    probe.failRead = false;
    probe.failDirectory = true;
    auto failedAbsolute = cue::Project::load(probe, path);
    if (failedRead.has_value() || failedRead.try_error()->operation != "Probe.open" || failedAbsolute.has_value() ||
        failedAbsolute.try_error()->operation != "Probe.absolute_path")
    {
        return __LINE__;
    }
    return 0;
}

/// @brief 任意の起動 Scene を Content 基準で解決し、従来形式と不正参照の拒否を確認する
int test_startup_scene(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto directory = test_path(a_fixture.root.join("Scene Project"));
    const auto projectFile = test_path(directory.join("Project.cueproj"));
    const auto sceneFile = test_path(directory.join("Content/Scenes/回転 Cube.cuescene"));
    if (!files.create_directories(test_path(directory.join("Source"))).has_value() ||
        !write_text(files, sceneFile, "Scene parser validates this separately\n"))
    {
        return __LINE__;
    }
    // 従来の六項目だけの設定も同じ Content 配置で引き続き受け付ける
    if (!write_text(files, projectFile, k_descriptor))
        return __LINE__;
    auto legacy = cue::Project::load(files, projectFile);
    if (!legacy.has_value() || legacy.try_value()->startup_scene_file() != nullptr ||
        legacy.try_value()->descriptor().startupScene)
        return __LINE__;
    const auto configured = std::string(k_descriptor) + "startupScene=Scenes/./回転 Cube.cuescene\n";
    if (!write_text(files, projectFile, configured))
        return __LINE__;
    auto loaded = cue::Project::load(files, projectFile);
    if (!loaded.has_value() || !loaded.try_value()->descriptor().startupScene ||
        loaded.try_value()->descriptor().startupScene->utf8() != "Scenes/回転 Cube.cuescene" ||
        !loaded.try_value()->startup_scene_file() ||
        loaded.try_value()->startup_scene_file()->utf8() != sceneFile.utf8())
        return __LINE__;
    // Project 自身が Path を所有し、設定 File の書換え後も既存値を保持する
    const auto snapshot = loaded.take_value();
    const std::array invalidReferences{"",
                                       "../Scene.cuescene",
                                       "Scenes/../../Outside.cuescene",
                                       "C:/Scene.cuescene",
                                       "/Scene.cuescene",
                                       "Scenes/Scene:stream.cuescene",
                                       "Scenes/Scene.txt",
                                       "Scenes/Scene.CUESCENE",
                                       "Scenes/Missing.cuescene"};
    for (const auto *reference : invalidReferences)
    {
        if (!write_text(files, projectFile, std::string(k_descriptor) + "startupScene=" + reference + '\n') ||
            cue::Project::load(files, projectFile).has_value())
            return __LINE__;
    }
    for (const auto &text : {configured + "startupScene=Scenes/回転 Cube.cuescene\n",
                             std::string(k_descriptor) + "startupScenes=Scenes/回転 Cube.cuescene\n"})
    {
        if (!write_text(files, projectFile, text) || cue::Project::load(files, projectFile).has_value())
            return __LINE__;
    }
    const auto directoryScene = test_path(directory.join("Content/Directory.cuescene"));
    if (!files.create_directories(directoryScene).has_value() ||
        !write_text(files, projectFile, std::string(k_descriptor) + "startupScene=Directory.cuescene\n") ||
        cue::Project::load(files, projectFile).has_value())
        return __LINE__;
    // Native Link 作成権限に依存せず、stat が Link を返す経路を検証する
    struct SceneLinkProbe final : cue::tests::FileSystemProbe
    {
        using FileSystemProbe::FileSystemProbe;
        /// @brief Scene File の最終要素だけを Link として観測させる
        cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
        {
            if (a_path.extension() == ".cuescene")
                return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Link});
            return FileSystemProbe::stat(a_path);
        }
    };
    if (!write_text(files, projectFile, configured))
        return __LINE__;
    SceneLinkProbe link(files);
    auto rejectedLink = cue::Project::load(link, projectFile);
    if (rejectedLink.has_value() || rejectedLink.try_error()->operation != "Project.startupScene.file" ||
        !snapshot.startup_scene_file() || snapshot.startup_scene_file()->utf8() != sceneFile.utf8())
        return __LINE__;
    return 0;
}
} // namespace

/// @brief 実 FileSystem 上の Project 読み込み・配置移動・失敗契約を確認する
int main()
{
    cue::tests::TemporaryFiles fixture;
    if (!fixture.isCreated)
    {
        return __LINE__;
    }
    for (const auto test : {test_loading, test_relative_path, test_failures, test_startup_scene})
    {
        const auto result = test(fixture);
        if (result != 0)
        {
            std::fprintf(stderr, "Project test failed at line %d\n", result);
            return result;
        }
    }
    return 0;
}
