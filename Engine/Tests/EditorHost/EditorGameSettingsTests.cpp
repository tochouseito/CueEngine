#include "EditorGameSettings.h"

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <Project/Project.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
constexpr std::string_view k_projectId = "7f533c49-d416-47ea-98c1-026822b1e6ed";
constexpr std::string_view k_descriptor =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\n"
    "name=Settings Test\napplicationName=SettingsTest\nsourceRoot=Source\ncontentRoot=Content\n";

#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor Game settings failed at %d: %s\n", __LINE__, #__VA_ARGS__);                   \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Test の固定した相対 Path だけを所有 Root に結合する
cue::Path joined(const cue::Path &a_root, std::string_view a_relative)
{
    auto path = a_root.join(a_relative);
    return path.has_value() ? path.take_value() : cue::Path{};
}

/// @brief Test Root の設定 File へ悪い入力と正常入力を同じ実 File 操作で配置する
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief 有効な設定の内容を比較し、失敗後の置換・追記がないことを確認する
std::string read(cue::IFileSystem &a_files, const cue::Path &a_file)
{
    auto bytes = a_files.read_all(a_file, 4096);
    if (!bytes.has_value())
        return {};
    const auto &data = *bytes.try_value();
    return std::string(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
}

/// @brief 未存在の親や File にも Link を注入し、生成・Open の前に拒否することを確認する
class SettingsProbe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Fixture が所有する実 FileSystem を借用する
    explicit SettingsProbe(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief 指定した一箇所の種類だけを差し替える
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
    {
        if (a_path.utf8() == linkedPath)
            return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Link});
        return FileSystemProbe::stat(a_path);
    }
    std::string linkedPath;
};

/// @brief Project 未選択でも言語を保存でき、書き込み失敗時に前回の設定が残ることを確認する
int test_language(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto dataRoot = joined(a_fixture.root, "UserData");
    const auto languageFile = joined(dataRoot, "editor/language.txt");
    auto language = cue::load_editor_language(files, dataRoot);
    CHECK(language.has_value() && *language.try_value() == cue::EditorLanguage::English);
    CHECK(cue::save_editor_language(files, dataRoot, cue::EditorLanguage::Japanese).has_value());
    language = cue::load_editor_language(files, dataRoot);
    CHECK(language.has_value() && *language.try_value() == cue::EditorLanguage::Japanese);

    SettingsProbe probe(files);
    probe.failSave = true;
    CHECK(!cue::save_editor_language(probe, dataRoot, cue::EditorLanguage::English).has_value());
    CHECK(read(files, languageFile) == "ja\n");
    CHECK(cue::save_editor_language(files, dataRoot, cue::EditorLanguage::English).has_value());
    language = cue::load_editor_language(files, dataRoot);
    CHECK(language.has_value() && *language.try_value() == cue::EditorLanguage::English);
    CHECK(write(files, languageFile, "invalid\n"));
    CHECK(!cue::load_editor_language(files, dataRoot).has_value());
    return 0;
}

/// @brief Missing の既定値、全構成、厳格な入力、原子的保存と Link 拒否をまとめて検証する
int test_settings(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    CHECK(files.create_directories(joined(a_fixture.root, "Source")).has_value());
    CHECK(files.create_directories(joined(a_fixture.root, "Content")).has_value());
    CHECK(write(files, joined(a_fixture.root, "Project.cueproj"), k_descriptor));
    auto loaded = cue::Project::load(files, joined(a_fixture.root, "Project.cueproj"));
    CHECK(loaded.has_value());
    const auto project = loaded.take_value();
    const auto settingsFile = joined(project.root(), "Intermediate/Editor/Settings.cueuser");
    auto selected = cue::load_editor_game_configuration(files, project);
    CHECK(selected.has_value() && *selected.try_value() == cue::GameBuildConfiguration::Development);
    auto exists = files.exists(joined(project.root(), "Intermediate"));
    CHECK(exists.has_value() && !*exists.try_value());
    for (const auto configuration : {cue::GameBuildConfiguration::Debug, cue::GameBuildConfiguration::Development,
                                     cue::GameBuildConfiguration::Release})
    {
        CHECK(cue::save_editor_game_configuration(files, project, configuration).has_value());
        auto roundtrip = cue::load_editor_game_configuration(files, project);
        CHECK(roundtrip.has_value() && *roundtrip.try_value() == configuration);
    }

    const std::string prefix = "formatVersion=1\nprojectId=" + std::string(k_projectId) + '\n';
    const auto valid = prefix + "gameConfiguration=Debug\n";
    for (const auto &text :
         {std::string{}, prefix + "gameConfiguration=Fast\n", valid + "unexpected=value\n",
          valid + "gameConfiguration=Release\n",
          std::string("formatVersion=2\nprojectId=") + std::string(k_projectId) + "\ngameConfiguration=Debug\n",
          std::string("formatVersion=1\nprojectId=8f533c49-d416-47ea-98c1-026822b1e6ed\n") +
              "gameConfiguration=Debug\n",
          prefix, valid + "unclosed-line\n", std::string(1025, 'x')})
    {
        CHECK(write(files, settingsFile, text));
        CHECK(!cue::load_editor_game_configuration(files, project).has_value());
        CHECK(read(files, settingsFile) == text);
    }
    const std::string crlf =
        "formatVersion=1\r\nprojectId=" + std::string(k_projectId) + "\r\ngameConfiguration=Release\r\n";
    CHECK(write(files, settingsFile, crlf));
    auto windowsText = cue::load_editor_game_configuration(files, project);
    CHECK(windowsText.has_value() && *windowsText.try_value() == cue::GameBuildConfiguration::Release);
    CHECK(write(files, settingsFile, valid.substr(0, valid.size() - 1)));
    CHECK(cue::load_editor_game_configuration(files, project).has_value());

    SettingsProbe probe(files);
    const auto before = read(files, settingsFile);
    probe.failSave = true;
    CHECK(!cue::save_editor_game_configuration(probe, project, cue::GameBuildConfiguration::Release).has_value());
    CHECK(read(files, settingsFile) == before && probe.saveCount == 1);
    probe.failSave = false;
    probe.failCreate = true;
    CHECK(!cue::save_editor_game_configuration(probe, project, cue::GameBuildConfiguration::Release).has_value());
    CHECK(read(files, settingsFile) == before && probe.saveCount == 1);
    probe.failCreate = false;
    CHECK(
        !cue::save_editor_game_configuration(probe, project, static_cast<cue::GameBuildConfiguration>(99)).has_value());
    CHECK(read(files, settingsFile) == before && probe.saveCount == 1);
    for (const auto &path :
         {joined(project.root(), "Intermediate"), joined(project.root(), "Intermediate/Editor"), settingsFile})
    {
        probe.linkedPath = path.utf8();
        CHECK(!cue::load_editor_game_configuration(probe, project).has_value());
        CHECK(!cue::save_editor_game_configuration(probe, project, cue::GameBuildConfiguration::Release).has_value());
        CHECK(read(files, settingsFile) == before && probe.saveCount == 1);
    }
    CHECK(read(files, project.file()) == k_descriptor);
    return 0;
}
} // namespace

/// @brief 一意作成した Test Root 内でのみ設定の読み書きと失敗を再現する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    CHECK(test_language(fixture) == 0);
    return test_settings(fixture);
}
