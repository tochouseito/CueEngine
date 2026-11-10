#include <EditorHost/EditorHost.h>

#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <DX12/DX12Backend.h>
#include <DX12/DX12RenderDevice.h>
#include <Project/Project.h>

#include "../Scene/SceneFixtures.h"
#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor Project Switch failed at %d: %s\n", __LINE__, #__VA_ARGS__);                  \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Test Root 内で固定した相対配置を組み立てる
cue::Path joined(const cue::Path &a_root, std::string_view a_relative)
{
    auto result = a_root.join(a_relative);
    return result.has_value() ? result.take_value() : cue::Path{};
}

/// @brief Test が所有する入力だけを原子的に書き込む
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.replace_file(a_file, std::as_bytes(std::span(a_text.data(), a_text.size()))).has_value();
}

/// @brief 保存確認の前後で Scene File が変更されたか比較する
std::string read(cue::IFileSystem &a_files, const cue::Path &a_file)
{
    auto bytes = a_files.read_all(a_file, 65536);
    if (!bytes.has_value())
        return {};
    const auto &data = *bytes.try_value();
    return std::string(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
}

/// @brief 通常の Scene Fixture に Cube と Camera を加え、切替前後も実描画 Snapshot を使う
std::string render_scene(std::string_view a_name)
{
    auto text = cue::tests::scene_replaced("name=RotateCube", "name=" + std::string(a_name));
    text.replace(0, std::string_view("formatVersion=1").size(), "formatVersion=2");
    text += "mesh.assetId=cue://engine/mesh/cube\n\n"
            "[object 1f533c49-d416-47ea-98c1-026822b1e6ed]\nname=Camera\nactive=true\n"
            "transform.position=0,0,12\ntransform.rotation=0,0,0,1\ntransform.scale=1,1,1\n"
            "camera.projection=perspective\ncamera.fovYRadians=1.04719758\ncamera.near=0.1\ncamera.far=1000\n";
    return text;
}

/// @brief Game Process を作らず、旧 Project の Build 結果の失効と Busy 判定を観測する
class BuildFixture final : public cue::IGameBuildService
{
  public:
    /// @brief この Fixture では外部 Build を要求しない
    cue::Result<void> start(const cue::Project &, const cue::GameBuildTarget &) override
    {
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.unexpected_build"});
    }
    /// @brief Host が状態を観測しても Test の注入値を進めない
    cue::Result<void> poll() override
    {
        return cue::Result<void>::success();
    }
    /// @brief 停止要求だけを記録し、新しい Project へ Build を持ち越さない
    cue::Result<void> cancel() override
    {
        current.state = cue::GameBuildState::Cancelled;
        return cue::Result<void>::success();
    }
    /// @brief 起動できない旧成果物を呼んだ場合に検証を失敗させる
    cue::Result<void> launch() override
    {
        ++launches;
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.unexpected_launch"});
    }
    /// @brief 静止中だけ表示結果を消し、失敗注入では旧状態を保持する
    cue::Result<void> reset_project() override
    {
        if (failReset || current.state == cue::GameBuildState::Configuring ||
            current.state == cue::GameBuildState::Building || current.isApplicationRunning)
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.reset_busy"});
        current = {};
        ++resets;
        return cue::Result<void>::success();
    }
    /// @brief この Fixture に回収すべき Process はない
    cue::Result<void> shutdown() override
    {
        return cue::Result<void>::success();
    }
    /// @brief Main Thread 上の Test が次の要求まで観測する
    const cue::GameBuildStatus &status() const noexcept override
    {
        return current;
    }
    cue::GameBuildStatus current;
    unsigned resets = 0;
    unsigned launches = 0;
    bool failReset = false;
};

/// @brief Project と Scene を独立 Directory へ作り、破損・版不一致も入力だけで再現する
bool make_project(cue::IFileSystem &a_files, const cue::Path &a_root, std::string_view a_name, std::string_view a_id,
                  std::string_view a_scene, bool a_isWrongVersion = false, bool a_hasGameTarget = false)
{
    if (!a_files.create_directories(joined(a_root, "Source")).has_value() ||
        !a_files.create_directories(joined(a_root, "Content/Scenes")).has_value())
        return false;
    const auto descriptor =
        std::string(a_isWrongVersion ? "formatVersion=2\nengineVersion=999.0.0\n" : "formatVersion=1\n") +
        "projectId=" + std::string(a_id) + "\nname=" + std::string(a_name) +
        "\napplicationName=SwitchFixture\nsourceRoot=Source\ncontentRoot=Content\n"
        "startupScene=Scenes/Start.cuescene\n" +
        std::string(a_hasGameTarget ? "gameTarget=SwitchFixtureGame\n" : "");
    return write(a_files, joined(a_root, "Source/CMakeLists.txt"), "# Project switch fixture\n") &&
           write(a_files, joined(a_root, "Content/Scenes/Start.cuescene"), a_scene) &&
           write(a_files, joined(a_root, "Project.cueproj"), descriptor);
}

/// @brief 外部入力・Logger 出力を分離し、実 WARP の Worker / Single 構成を同じ契約で作る
cue::EditorHostConfig host_config(cue::tests::TemporaryFiles &a_fixture, bool a_useWorkers,
                                  std::unique_ptr<cue::IFileSystem> a_files)
{
    cue::EditorHostConfig config;
    config.window = {"CueEngine Project Switch Test", {320, 180}};
    config.enableProjectOpening = true;
    config.frame.maxFps = 0;
    config.gpu.resourceFrameCount = a_useWorkers ? 2 : 1;
    config.gpu.maxFramesInFlight = config.gpu.resourceFrameCount;
    config.fileSystem = std::move(a_files);
    config.imgui.settingsFile.clear();
    config.logging.isFileEnabled = false;
    config.logging.debugOutput = cue::DebugLogOutput::Disabled;
    config.storage.dataRootOverride = joined(a_fixture.root, "Data");
    config.storage.logsOverride = joined(a_fixture.root, "Logs");
    config.storage.shaderCacheOverride = joined(a_fixture.root, "ShaderCache");
    config.storage.psoCacheOverride = joined(a_fixture.root, "PsoCache");
    config.createBackend = [](cue::IFileSystem *a_files, cue::Clock &a_clock)
    {
        auto backend = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp, {}, a_files, &a_clock);
        return backend.has_value() ? cue::Result<std::unique_ptr<cue::IBackend>>::success(backend.take_value())
                                   : cue::Result<std::unique_ptr<cue::IBackend>>::failure(*backend.try_error());
    };
    return config;
}

/// @brief 切替予約の公開と旧 Snapshot の消費を実 Frame で進める
bool step_frames(cue::EditorHost &a_host, unsigned a_count = 6)
{
    for (unsigned index = 0; index != a_count; ++index)
    {
        auto result = a_host.step();
        if (!result.has_value() || !*result.try_value())
            return false;
    }
    return true;
}

/// @brief 未選択から複数 Project を再読込し、失敗・Busy・旧 Build の失効を確認する
int test_switching(bool a_useWorkers)
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto firstRoot = joined(fixture.root, "A");
    const auto secondRoot = joined(fixture.root, "B");
    const auto brokenRoot = joined(fixture.root, "Broken");
    const auto wrongRoot = joined(fixture.root, "WrongVersion");
    CHECK(make_project(files, firstRoot, "Project A", "7f533c49-d416-47ea-98c1-026822b1e6ed",
                       render_scene("RotateCube"), false, true));
    const auto secondScene = render_scene("Project B Scene");
    CHECK(
        make_project(files, secondRoot, "Project B", "8f533c49-d416-47ea-98c1-026822b1e6ed", secondScene, false, true));
    CHECK(make_project(files, brokenRoot, "Broken", "9f533c49-d416-47ea-98c1-026822b1e6ed", "broken scene\n"));
    CHECK(
        make_project(files, wrongRoot, "Wrong", "af533c49-d416-47ea-98c1-026822b1e6ed", cue::tests::k_sceneText, true));
    const auto first = joined(firstRoot, "Project.cueproj");
    const auto second = joined(secondRoot, "Project.cueproj");
    auto probe = std::make_unique<cue::tests::FileSystemProbe>(files);
    auto *observedFiles = probe.get();
    auto builder = std::make_unique<BuildFixture>();
    auto *observedBuild = builder.get();
    auto config = host_config(fixture, a_useWorkers, std::move(probe));
    config.gameBuildService = std::move(builder);
    cue::EditorHost host(std::move(config));
    CHECK(!host.request_open_project(first).has_value());
    CHECK(host.initialize().has_value());
    CHECK(!host.project() && !host.scene() && host.can_open_project());
    CHECK(!host.preview_world() && !host.preview_instance());
    CHECK(!host.run_game().has_value());
    CHECK(step_frames(host));
    CHECK(!host.project() && !host.scene() && !host.preview_world() && !host.preview_instance());

    bool rejected = false;
    std::thread other(
        [&]()
        {
            rejected = !host.request_open_project(first).has_value() && !host.can_open_project() &&
                       !host.has_pending_project_confirmation();
        });
    other.join();
    CHECK(rejected && !host.project());
    CHECK(host.request_open_project(first).has_value());
    CHECK(!host.has_pending_project_confirmation());
    CHECK(step_frames(host));
    CHECK(host.project() && host.project()->file().utf8() == first.utf8() && host.scene());
    CHECK(host.scene()->descriptor().name == "RotateCube");
    const auto firstScene = host.scene()->serialize();
    CHECK(firstScene.has_value());
    // Game の構成は Scene / Project の正本と独立し、Project ごとのローカル設定として保持する
    const auto descriptorBefore = read(files, first);
    CHECK(host.game_configuration() == cue::GameBuildConfiguration::Development);
    CHECK(host.can_select_game_configuration());
    CHECK(host.request_game_configuration(cue::GameBuildConfiguration::Debug).has_value());
    CHECK(host.game_configuration() == cue::GameBuildConfiguration::Debug);
    CHECK(!host.request_game_configuration(static_cast<cue::GameBuildConfiguration>(99)).has_value());
    CHECK(host.game_configuration() == cue::GameBuildConfiguration::Debug);
    CHECK(read(files, first) == descriptorBefore);
    // Source の作成は文書や製品 Target を変更せず、同名作成と不正名を拒否する
    CHECK(write(files, joined(firstRoot, "Source/CMakeLists.txt"), "cue_add_game_scripts(SwitchFixtureGame)\n"));
    CHECK(write(files, joined(firstRoot, "Source/Existing.cpp"), "// Existing script\n"));
    CHECK(write(files, joined(firstRoot, "Source/ScriptSources.cueide"),
                "formatVersion=2\nscript.game.Existing=Existing.cpp\nregistration.game.Existing=Existing\n"));
    CHECK(host.request_script_creation("CreatedScript").has_value());
    CHECK(!host.request_script_creation("CreatedScript").has_value());
    CHECK(!host.request_script_creation("../Escape").has_value());
    CHECK(read(files, joined(firstRoot, "Source/CreatedScript.cpp")).find("CUE_DEFINE_SCRIPT(CreatedScript") !=
          std::string::npos);
    auto afterConfiguration = host.scene()->serialize();
    CHECK(afterConfiguration.has_value() && *afterConfiguration.try_value() == *firstScene.try_value());

    // 不正な候補の失敗は現在の Project、Document、Build 結果を変更しない
    CHECK(!host.request_open_project(joined(brokenRoot, "Project.cueproj")).has_value());
    CHECK(!host.request_open_project(joined(wrongRoot, "Project.cueproj")).has_value());
    CHECK(!host.request_open_project(joined(fixture.root, "Missing.cueproj")).has_value());
    observedFiles->failedFilename = "Start.cuescene";
    CHECK(!host.request_open_project(second).has_value());
    observedFiles->failedFilename.clear();
    CHECK(host.project()->file().utf8() == first.utf8());
    const auto afterFailures = host.scene()->serialize();
    CHECK(afterFailures.has_value() && *afterFailures.try_value() == *firstScene.try_value());
    CHECK(step_frames(host));

    for (const auto state : {cue::GameBuildState::Configuring, cue::GameBuildState::Building})
    {
        observedBuild->current.state = state;
        CHECK(!host.can_open_project() && !host.request_open_project(second).has_value());
        CHECK(!host.can_select_game_configuration() &&
              !host.request_game_configuration(cue::GameBuildConfiguration::Release).has_value());
        CHECK(host.project()->file().utf8() == first.utf8());
    }
    observedBuild->current = {};
    observedBuild->current.isApplicationRunning = true;
    CHECK(!host.can_open_project() && !host.request_open_project(second).has_value());
    observedBuild->current = {};
    observedBuild->current.state = cue::GameBuildState::Succeeded;
    observedBuild->current.log = "old Project log";
    observedBuild->current.executable = joined(firstRoot, "OldApp.exe");
    CHECK(host.can_open_project() && host.request_open_project(second).has_value());
    CHECK(step_frames(host));
    CHECK(host.project()->file().utf8() == second.utf8() && host.scene()->descriptor().name == "Project B Scene");
    CHECK(host.game_configuration() == cue::GameBuildConfiguration::Development);
    CHECK(host.request_game_configuration(cue::GameBuildConfiguration::Release).has_value());
    CHECK(observedBuild->resets >= 2 && observedBuild->current.state == cue::GameBuildState::Idle);
    CHECK(observedBuild->current.log.empty() && observedBuild->current.executable.utf8().empty());
    CHECK(!host.run_game().has_value() && observedBuild->launches == 0);

    CHECK(host.request_open_project(first).has_value() && step_frames(host));
    CHECK(host.project()->file().utf8() == first.utf8() && host.scene()->descriptor().name == "RotateCube");
    CHECK(host.game_configuration() == cue::GameBuildConfiguration::Debug);
    // 同じ Project の再選択も新しい入力を読み、以前の Scene 値を再利用しない
    CHECK(write(files, joined(firstRoot, "Content/Scenes/Start.cuescene"), render_scene("Reloaded A")));
    CHECK(host.request_open_project(first).has_value() && step_frames(host));
    CHECK(host.scene()->descriptor().name == "Reloaded A");
    CHECK(host.shutdown().has_value());
    CHECK(!host.can_open_project() && !host.request_open_project(second).has_value());
    auto reopenedConfig = host_config(fixture, a_useWorkers, nullptr);
    cue::EditorHost reopened(std::move(reopenedConfig));
    CHECK(reopened.initialize().has_value());
    CHECK(reopened.request_open_project(first).has_value() && step_frames(reopened));
    CHECK(reopened.game_configuration() == cue::GameBuildConfiguration::Debug);
    CHECK(reopened.shutdown().has_value());
    return 0;
}

/// @brief 未保存の旧 Document を保存・破棄・取消した場合だけ切替が進むことを確認する
int test_dirty_document(bool a_useWorkers, bool a_shouldSave)
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto firstRoot = joined(fixture.root, "A");
    const auto secondRoot = joined(fixture.root, "B");
    CHECK(make_project(files, firstRoot, "Dirty A", "7f533c49-d416-47ea-98c1-026822b1e6ed", cue::tests::k_sceneText));
    CHECK(
        make_project(files, secondRoot, "Project B", "8f533c49-d416-47ea-98c1-026822b1e6ed", cue::tests::k_sceneText));
    const auto first = joined(firstRoot, "Project.cueproj");
    const auto second = joined(secondRoot, "Project.cueproj");
    const auto sceneFile = joined(firstRoot, "Content/Scenes/Start.cuescene");
    auto project = cue::Project::load(files, first);
    CHECK(project.has_value());
    auto probe = std::make_unique<cue::tests::FileSystemProbe>(files);
    auto *observedFiles = probe.get();
    auto config = host_config(fixture, a_useWorkers, std::move(probe));
    config.project = project.take_value();
    // Scene を明示しない Library 起動は、新規で未保存だが保存先が既知の Document を用意する
    cue::EditorHost host(std::move(config));
    CHECK(host.initialize().has_value());
    CHECK(host.project() && host.scene());
    const auto before = host.scene()->serialize();
    CHECK(before.has_value());
    const auto savedBefore = read(files, sceneFile);
    CHECK(!savedBefore.empty() && savedBefore != *before.try_value());

    CHECK(host.request_open_project(second).has_value() && host.has_pending_project_confirmation());
    CHECK(!host.can_open_project() && !host.request_open_project(first).has_value());
    CHECK(host.confirm_open_project(cue::ProjectOpenSaveChoice::Cancel).has_value());
    CHECK(!host.has_pending_project_confirmation() && host.project()->file().utf8() == first.utf8());
    CHECK(step_frames(host));
    CHECK(read(files, sceneFile) == savedBefore && host.project()->file().utf8() == first.utf8());

    if (a_shouldSave)
    {
        CHECK(host.request_open_project(second).has_value() && host.has_pending_project_confirmation());
        observedFiles->failSave = true;
        CHECK(!host.confirm_open_project(cue::ProjectOpenSaveChoice::Save).has_value());
        observedFiles->failSave = false;
        CHECK(!host.has_pending_project_confirmation());
        CHECK(host.project()->file().utf8() == first.utf8() && read(files, sceneFile) == savedBefore);
        CHECK(step_frames(host) && host.project()->file().utf8() == first.utf8());
    }
    // 同じ Project の保存付き再選択は、保存前の候補ではなく保存後の Scene を読み直す
    const auto &destination = a_shouldSave ? first : second;
    CHECK(host.request_open_project(destination).has_value() && host.has_pending_project_confirmation());
    CHECK(
        host.confirm_open_project(a_shouldSave ? cue::ProjectOpenSaveChoice::Save : cue::ProjectOpenSaveChoice::Discard)
            .has_value());
    CHECK(!host.has_pending_project_confirmation());
    CHECK(step_frames(host) && host.project()->file().utf8() == destination.utf8());
    CHECK(read(files, sceneFile) == (a_shouldSave ? *before.try_value() : savedBefore));
    if (a_shouldSave)
    {
        auto after = host.scene()->serialize();
        CHECK(after.has_value() && *after.try_value() == *before.try_value());
    }
    CHECK(host.shutdown().has_value());
    return 0;
}
} // namespace

/// @brief Single / Worker の両方で、同一 Editor Process の Project 再読込と保存境界を検証する
int main()
{
    for (const bool useWorkers : {false, true})
    {
        CHECK(test_switching(useWorkers) == 0);
        CHECK(test_dirty_document(useWorkers, true) == 0);
        CHECK(test_dirty_document(useWorkers, false) == 0);
    }
    return 0;
}
