#include <ProjectBuild/GameBuild.h>

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <Foundation/Windows/UtfConversion.h>
#include <Project/Project.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
constexpr std::string_view k_projectText =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\n"
    "name=Artifact Fixture\napplicationName=Example\nsourceRoot=Source\ncontentRoot=Content\n";
constexpr std::string_view k_manifestText =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\n"
    "gameTarget=ExampleGame\nconfiguration=Debug\nplatform=Windows\narchitecture=x64\nabiVersion=1\n"
    "module=Runtime/ExampleGame.dll\nstaticLibrary=Libraries/ExampleGameStatic.lib\n"
    "moduleSha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
    "staticLibrarySha256=2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\n";

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Game Build artifact test line %d: %s\n", __LINE__, #__VA_ARGS__);                    \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Fixture の成功済み Path だけを所有値で受け取る
cue::Path path(cue::Result<cue::Path> a_value)
{
    return a_value.take_value();
}

/// @brief Test Root にだけ検証用の所有データを書き込む
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief Manifest の一項目を変更して比較対象と無関係な File 内容を維持する
std::string manifest_with(std::string_view a_before, std::string_view a_after)
{
    std::string result(k_manifestText);
    result.replace(result.find(a_before), a_before.size(), a_after);
    return result;
}

/// @brief 成果物の種類・I/O 失敗を注入し、読込経路で書込みを行わないことを観測する
class ArtifactProbe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Fixture の FileSystem を Test の期間だけ借用する
    explicit ArtifactProbe(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief Build 内の Link や問い合わせ失敗を実際の権限変更なしで再現する
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
    {
        if (!failedStat.empty() && a_path.filename() == failedStat)
            return cue::Result<std::optional<cue::FileInfo>>::failure(
                {cue::ErrorCategory::PlatformFailure, "Probe.artifact_stat", 5});
        if (!linkedName.empty() && a_path.filename() == linkedName)
            return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Link});
        return FileSystemProbe::stat(a_path);
    }
    /// @brief 読込の副作用として Directory を作らないことを検証する
    cue::Result<void> create_directories(const cue::Path &a_path) override
    {
        ++writes;
        return FileSystemProbe::create_directories(a_path);
    }
    /// @brief 削除・移動による成果物の書換えを観測する
    cue::Result<bool> remove(const cue::Path &a_path) override
    {
        ++writes;
        return FileSystemProbe::remove(a_path);
    }
    /// @brief 読込が成果物を公開し直さないことを検証する
    cue::Result<void> rename(const cue::Path &a_from, const cue::Path &a_to) override
    {
        ++writes;
        return FileSystemProbe::rename(a_from, a_to);
    }
    /// @brief 読込が成果物をコピーしないことを検証する
    cue::Result<void> copy_file(const cue::Path &a_from, const cue::Path &a_to, bool a_overwrite) override
    {
        ++writes;
        return FileSystemProbe::copy_file(a_from, a_to, a_overwrite);
    }
    /// @brief File Open の書込み権限や作成モードを観測する
    cue::Result<std::unique_ptr<cue::IFile>> open(const cue::Path &a_path,
                                                  const cue::FileOpenDesc &a_desc = {}) override
    {
        if (a_desc.access != cue::FileAccess::Read || a_desc.creation != cue::FileCreation::OpenExisting)
            ++writes;
        return FileSystemProbe::open(a_path, a_desc);
    }
    std::string failedStat;
    std::string linkedName;
    unsigned writes = 0;
};

struct FixturePaths final
{
    cue::Path root;
    cue::Path projectFile;
    cue::Path manifest;
    cue::Path module;
    cue::Path library;
};

/// @brief 既知の SHA-256 ベクトルを持つ独立 Project の配置を準備する
bool prepare(cue::tests::TemporaryFiles &a_files, FixturePaths &a_paths)
{
    a_paths.root = path(a_files.root.join("日本語 Artifact Project"));
    a_paths.projectFile = path(a_paths.root.join("Game.cueproj"));
    const auto directory = path(a_paths.root.join("Build/Windows/x64/Debug"));
    a_paths.manifest = path(directory.join("GameModule.cuebuild"));
    a_paths.module = path(directory.join("Runtime/ExampleGame.dll"));
    a_paths.library = path(directory.join("Libraries/ExampleGameStatic.lib"));
    return a_files.files->create_directories(path(a_paths.root.join("Content"))).has_value() &&
           write(*a_files.files, path(a_paths.root.join("Source/CMakeLists.txt")), "# Fixture\n") &&
           write(*a_files.files, a_paths.projectFile, std::string(k_projectText) + "gameTarget=ExampleGame\n") &&
           write(*a_files.files, a_paths.manifest, k_manifestText) && write(*a_files.files, a_paths.module, "abc") &&
           write(*a_files.files, a_paths.library, "hello");
}

/// @brief 所有 Path・構成別配置・既知 Hash を確認し、読込で CWD や File を変更しないことを検証する
int test_success(cue::tests::TemporaryFiles &a_files, const FixturePaths &a_paths, const cue::Project &a_project)
{
    auto &files = *a_files.files;
    const auto cwd = files.absolute_path(path(cue::Path::create(".")));
    CHECK(cwd.has_value());
    ArtifactProbe probe(files);
    auto artifacts = cue::load_game_build_artifacts(probe, a_project, cue::GameBuildConfiguration::Debug);
    CHECK(artifacts.has_value());
    CHECK(artifacts.try_value()->moduleFile.utf8() == a_paths.module.utf8());
    CHECK(artifacts.try_value()->staticLibraryFile.utf8() == a_paths.library.utf8());
    CHECK(artifacts.try_value()->manifestFile.utf8() == a_paths.manifest.utf8());
    CHECK(artifacts.try_value()->gameTarget == "ExampleGame");
    CHECK(artifacts.try_value()->configuration == cue::GameBuildConfiguration::Debug);
    auto selected = cue::create_game_build_target("Windows", "x64", cue::GameBuildConfiguration::Debug);
    CHECK(selected.has_value());
    auto explicitArtifacts = cue::load_game_build_artifacts(probe, a_project, *selected.try_value());
    CHECK(explicitArtifacts.has_value());
    CHECK(explicitArtifacts.try_value()->moduleFile.utf8() == artifacts.try_value()->moduleFile.utf8());
    CHECK(explicitArtifacts.try_value()->buildTarget == *selected.try_value());
    const cue::GameBuildTarget unsupported{"Linux", "x64", cue::GameBuildConfiguration::Debug};
    CHECK(!cue::game_build_directory(a_project, unsupported).has_value());
    CHECK(!cue::load_game_build_artifacts(probe, a_project, unsupported).has_value());
    CHECK(probe.writes == 0 && probe.saveCount == 0 && probe.absoluteCount == 0);
    auto after = files.absolute_path(path(cue::Path::create(".")));
    CHECK(after.has_value() && after.try_value()->utf8() == cwd.try_value()->utf8());
    for (const auto configuration : {cue::GameBuildConfiguration::Debug, cue::GameBuildConfiguration::Development,
                                     cue::GameBuildConfiguration::Release})
    {
        const auto name = cue::game_build_configuration_name(configuration);
        auto directory = cue::game_build_directory(a_project, configuration);
        CHECK(directory.has_value());
        CHECK(directory.try_value()->utf8() == a_paths.root.utf8() + "/Build/Windows/x64/" + std::string(name));
    }
    const auto unknown = static_cast<cue::GameBuildConfiguration>(999);
    CHECK(cue::game_build_configuration_name(unknown).empty());
    CHECK(!cue::game_build_directory(a_project, unknown).has_value());
    CHECK(!cue::load_game_build_artifacts(files, a_project, cue::GameBuildConfiguration::Development).has_value());
    return 0;
}

/// @brief 別 Project・構成・ABI と、Manifest の不正 Path / Hash / Key を拒否する
int test_manifest(cue::IFileSystem &a_files, const FixturePaths &a_paths, const cue::Project &a_project)
{
    const std::array bad{
        std::string{},
        manifest_with("formatVersion=1", "formatVersion=2"),
        manifest_with("projectId=7f533c49-d416-47ea-98c1-026822b1e6ed",
                      "projectId=5f533c49-d416-47ea-98c1-026822b1e6ed"),
        manifest_with("gameTarget=ExampleGame", "gameTarget=DifferentGame"),
        manifest_with("configuration=Debug", "configuration=Release"),
        manifest_with("platform=Windows", "platform=Linux"),
        manifest_with("architecture=x64", "architecture=arm64"),
        manifest_with("abiVersion=1", "abiVersion=2"),
        manifest_with("module=Runtime/ExampleGame.dll", "module=C:/Different.dll"),
        manifest_with("module=Runtime/ExampleGame.dll", "module=../Different.dll"),
        manifest_with("module=Runtime/ExampleGame.dll", "module=Runtime/../Runtime/ExampleGame.dll"),
        manifest_with("staticLibrary=Libraries/ExampleGameStatic.lib", "staticLibrary=Libraries/Other.lib"),
        manifest_with("moduleSha256=ba7816bf", "moduleSha256=Ba7816bf"),
        manifest_with("moduleSha256=ba7816bf", "moduleSha256=za7816bf"),
        manifest_with("staticLibrarySha256=2cf24dba", "staticLibrarySha256=cf24dba"),
        manifest_with("formatVersion=1\n", ""),
        std::string(k_manifestText) + "gameTarget=ExampleGame\n",
        std::string(k_manifestText) + "unknown=1\n",
        std::string(k_manifestText) + std::string("bad=\0x", 7),
        std::string(4097, 'x')};
    for (std::size_t index = 0; index < bad.size(); ++index)
    {
        CHECK(write(a_files, a_paths.manifest, bad[index]));
        auto result = cue::load_game_build_artifacts(a_files, a_project, cue::GameBuildConfiguration::Debug);
        if (result.has_value())
        {
            std::fprintf(stderr, "Accepted invalid Game Manifest %zu\n", index);
            return __LINE__;
        }
    }
    // 上限ちょうどは余分な空行を許可する。File Size 超過とは区別する
    auto atLimit = std::string(k_manifestText) + std::string(4096 - k_manifestText.size(), '\n');
    CHECK(write(a_files, a_paths.manifest, atLimit));
    CHECK(cue::load_game_build_artifacts(a_files, a_project, cue::GameBuildConfiguration::Debug).has_value());
    CHECK(write(a_files, a_paths.manifest, k_manifestText));
    return 0;
}

/// @brief 同じ Size の変更・空 File・Library の変更も Hash で検出する
int test_changed_artifacts(cue::IFileSystem &a_files, const FixturePaths &a_paths, const cue::Project &a_project)
{
    for (const auto *content : {"abd", "", "changed module"})
    {
        CHECK(write(a_files, a_paths.module, content));
        auto result = cue::load_game_build_artifacts(a_files, a_project, cue::GameBuildConfiguration::Debug);
        CHECK(!result.has_value());
    }
    CHECK(write(a_files, a_paths.module, "abc"));
    CHECK(write(a_files, a_paths.library, "jello"));
    auto libraryChanged = cue::load_game_build_artifacts(a_files, a_project, cue::GameBuildConfiguration::Debug);
    CHECK(!libraryChanged.has_value() && libraryChanged.try_error()->operation == "GameBuild.artifact_hash_mismatch");
    CHECK(write(a_files, a_paths.library, "hello"));
    CHECK(a_files.remove(a_paths.module).has_value());
    auto missing = cue::load_game_build_artifacts(a_files, a_project, cue::GameBuildConfiguration::Debug);
    CHECK(!missing.has_value() && missing.try_error()->category == cue::ErrorCategory::InvalidState);
    CHECK(write(a_files, a_paths.module, "abc"));
    return 0;
}

/// @brief Link・I/O 失敗と、Hash 照合中に Manifest が再公開された競合を検出する
int test_io(cue::IFileSystem &a_files, const FixturePaths &a_paths, const cue::Project &a_project)
{
    ArtifactProbe probe(a_files);
    for (const auto *name : {"GameModule.cuebuild", "ExampleGame.dll", "ExampleGameStatic.lib", "Runtime", "Build"})
    {
        probe.linkedName = name;
        CHECK(!cue::load_game_build_artifacts(probe, a_project, cue::GameBuildConfiguration::Debug).has_value());
    }
    probe.linkedName.clear();
    probe.failedStat = "GameModule.cuebuild";
    auto failedStat = cue::load_game_build_artifacts(probe, a_project, cue::GameBuildConfiguration::Debug);
    CHECK(!failedStat.has_value() && failedStat.try_error()->operation == "Probe.artifact_stat");
    probe.failedStat.clear();
    for (const auto *name : {"GameModule.cuebuild", "ExampleGame.dll", "ExampleGameStatic.lib"})
    {
        probe.failedFilename = name;
        auto failedOpen = cue::load_game_build_artifacts(probe, a_project, cue::GameBuildConfiguration::Debug);
        CHECK(!failedOpen.has_value() && failedOpen.try_error()->operation == "Probe.open");
    }
    probe.failedFilename.clear();
    unsigned manifestReads = 0;
    bool hasChanged = false;
    probe.onOpen = [&](const cue::Path &a_path)
    {
        if (a_path.filename() == "GameModule.cuebuild" && ++manifestReads == 2)
            hasChanged = write(a_files, a_paths.manifest, std::string(k_manifestText) + '\n');
    };
    auto raced = cue::load_game_build_artifacts(probe, a_project, cue::GameBuildConfiguration::Debug);
    CHECK(hasChanged && !raced.has_value() && raced.try_error()->operation == "GameBuild.manifest_changed");
    CHECK(probe.writes == 0 && probe.saveCount == 0);
    CHECK(write(a_files, a_paths.manifest, k_manifestText));
    return 0;
}

/// @brief Project の移動後も保存値だけから再解決し、旧形式には暗黙の DLL を適用しない
int test_move_and_legacy(cue::tests::TemporaryFiles &a_files, const FixturePaths &a_paths)
{
    auto &files = *a_files.files;
    const auto destination = path(a_files.root.join("移動後 Project"));
    CHECK(files.rename(a_paths.root, destination).has_value());
    auto moved = cue::Project::load(files, path(destination.join("Game.cueproj")));
    CHECK(moved.has_value());
    auto resolved = cue::load_game_build_artifacts(files, *moved.try_value(), cue::GameBuildConfiguration::Debug);
    CHECK(resolved.has_value() && resolved.try_value()->moduleFile.utf8().starts_with(destination.utf8() + '/'));
    const auto legacyFile = path(destination.join("Legacy.cueproj"));
    CHECK(write(files, legacyFile, k_projectText));
    auto legacy = cue::Project::load(files, legacyFile);
    CHECK(legacy.has_value());
    CHECK(!cue::game_build_directory(*legacy.try_value(), cue::GameBuildConfiguration::Debug).has_value());
    CHECK(!cue::load_game_build_artifacts(files, *legacy.try_value(), cue::GameBuildConfiguration::Debug).has_value());
    return 0;
}

/// @brief CMake が公開した実 Manifest と実 DLL / Library の Hash を同じ読込 API で検証する
int test_generated(const wchar_t *a_projectFile, const wchar_t *a_moduleFile)
{
    auto projectText = cue::utf16_to_utf8(a_projectFile);
    auto moduleText = cue::utf16_to_utf8(a_moduleFile);
    CHECK(projectText.has_value() && moduleText.has_value());
    auto projectFile = cue::Path::create(*projectText.try_value());
    auto moduleFile = cue::Path::create(*moduleText.try_value());
    CHECK(projectFile.has_value() && moduleFile.has_value());
    auto files = cue::create_windows_file_system();
    CHECK(files.has_value());
    auto project = cue::Project::load(**files.try_value(), *projectFile.try_value());
    CHECK(project.has_value());
    auto artifacts = cue::load_game_build_artifacts(**files.try_value(), *project.try_value(),
                                                    cue::current_game_build_configuration());
    if (!artifacts.has_value())
        std::fprintf(stderr, "%s\n", artifacts.try_error()->operation.c_str());
    CHECK(artifacts.has_value());
    CHECK(artifacts.try_value()->moduleFile.utf8() == moduleFile.try_value()->utf8());
    return 0;
}
} // namespace

/// @brief Manifest と実成果物の一致・失敗境界を、Game DLL を Load せず検証する
int wmain(int a_count, wchar_t **a_arguments)
{
    if (a_count == 3)
        return test_generated(a_arguments[1], a_arguments[2]);
    CHECK(a_count == 1);
    cue::tests::TemporaryFiles files;
    CHECK(files.isCreated && files.files);
    FixturePaths paths;
    CHECK(prepare(files, paths));
    auto project = cue::Project::load(*files.files, paths.projectFile);
    CHECK(project.has_value());
    int result = test_success(files, paths, *project.try_value());
    if (!result)
        result = test_manifest(*files.files, paths, *project.try_value());
    if (!result)
        result = test_changed_artifacts(*files.files, paths, *project.try_value());
    if (!result)
        result = test_io(*files.files, paths, *project.try_value());
    if (!result)
        result = test_move_and_legacy(files, paths);
    return result;
}
