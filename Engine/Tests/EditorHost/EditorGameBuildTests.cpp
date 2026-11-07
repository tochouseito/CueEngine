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
#include <Platform/Windows/WindowsFileSystem.h>
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
            std::fprintf(stderr, "Editor Game Build failed at %d: %s\n", __LINE__, #__VA_ARGS__);                      \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Fixture 内の検証済み Path を値で受け取る
cue::Path joined(const cue::Path &a_root, std::string_view a_relative)
{
    auto result = a_root.join(a_relative);
    return result.has_value() ? result.take_value() : cue::Path{};
}
/// @brief 起動 Scene の保存結果だけを読み、Build Service の動作と分離する
std::string read(cue::IFileSystem &a_files, const cue::Path &a_path)
{
    auto data = a_files.read_all(a_path, 65536);
    if (!data.has_value())
        return {};
    const auto &bytes = *data.try_value();
    return std::string(bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
/// @brief 一意の Fixture だけに Project と Scene の入力を保存する
bool write(cue::IFileSystem &a_files, const cue::Path &a_path, std::string_view a_text)
{
    return a_files.replace_file(a_path, std::as_bytes(std::span(a_text.data(), a_text.size()))).has_value();
}
/// @brief Host 破棄後も確認できる、外部 Process の要求回数
struct Observations final
{
    unsigned starts = 0;
    unsigned polls = 0;
    unsigned launches = 0;
    unsigned shutdowns = 0;
};
/// @brief Process を作らず、保存前後と Owner Thread の Host 契約を観測する
class BuildFixture final : public cue::IGameBuildService
{
  public:
    /// @brief Host より長く生存する観測先を借用する
    explicit BuildFixture(Observations &a_observed) : observed(a_observed)
    {
    }
    /// @brief 検証済み要求だけがここへ届くことを数える
    cue::Result<void> start(const cue::Project &, const cue::GameBuildTarget &a_target) override
    {
        ++observed.starts;
        current = {};
        current.state = cue::GameBuildState::Configuring;
        current.target = a_target;
        return cue::Result<void>::success();
    }
    /// @brief Host の通常 Frame が Build の待機へ入らないことを観測する
    cue::Result<void> poll() override
    {
        ++observed.polls;
        return cue::Result<void>::success();
    }
    /// @brief Cancel 後の再要求を許可する
    cue::Result<void> cancel() override
    {
        current.state = cue::GameBuildState::Cancelled;
        return cue::Result<void>::success();
    }
    /// @brief 成功後だけ Host が製品の実行を要求したことを確認する
    cue::Result<void> launch() override
    {
        ++observed.launches;
        current.isApplicationRunning = true;
        return cue::Result<void>::success();
    }
    /// @brief Host の明示停止が Service へ届く。複数回を許可する
    cue::Result<void> shutdown() override
    {
        ++observed.shutdowns;
        return cue::Result<void>::success();
    }
    /// @brief Test が状態を進め、同じ公開表示契約へ渡す
    const cue::GameBuildStatus &status() const noexcept override
    {
        return current;
    }
    Observations &observed;
    cue::GameBuildStatus current;
};
} // namespace

/// @brief 実 Editor の非同期接続と保存確認を、所有 Fixture だけを変更して検証する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    CHECK(files.create_directories(joined(fixture.root, "Source")).has_value());
    CHECK(files.create_directories(joined(fixture.root, "Content/Scenes")).has_value());
    CHECK(write(files, joined(fixture.root, "Source/CMakeLists.txt"), "# Build mock\n"));
    const auto sceneFile = joined(fixture.root, "Content/Scenes/Start.cuescene");
    CHECK(write(files, sceneFile, cue::tests::k_sceneText));
    const auto projectFile = joined(fixture.root, "Build.cueproj");
    CHECK(write(files, projectFile,
                "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\nname=Build Fixture\n"
                "applicationName=BuildFixture\nsourceRoot=Source\ncontentRoot=Content\n"
                "startupScene=Scenes/Start.cuescene\ngameTarget=BuildFixtureGame\n"));
    auto project = cue::Project::load(files, projectFile);
    CHECK(project.has_value());
    auto probe = std::make_unique<cue::tests::FileSystemProbe>(files);
    auto *observedFiles = probe.get();
    Observations observed;
    auto service = std::make_unique<BuildFixture>(observed);
    auto *builder = service.get();
    {
        cue::EditorHostConfig config;
        config.window = {"CueEngine Editor Build Integration", {320, 180}};
        config.project = project.take_value();
        // 初期 Scene を渡さず、新規で未保存の Document と既知の保存先を準備する
        config.fileSystem = std::move(probe);
        config.gameBuildService = std::move(service);
        config.imgui.settingsFile.clear();
        config.logging.isFileEnabled = false;
        config.logging.debugOutput = cue::DebugLogOutput::Disabled;
        config.storage.dataRootOverride = joined(fixture.root, "Data");
        config.storage.logsOverride = joined(fixture.root, "Logs");
        config.storage.shaderCacheOverride = joined(fixture.root, "ShaderCache");
        config.storage.psoCacheOverride = joined(fixture.root, "PsoCache");
        config.createBackend = [](cue::IFileSystem *a_files, cue::Clock &a_clock)
        {
            auto backend = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp, {}, a_files, &a_clock);
            return backend.has_value() ? cue::Result<std::unique_ptr<cue::IBackend>>::success(backend.take_value())
                                       : cue::Result<std::unique_ptr<cue::IBackend>>::failure(*backend.try_error());
        };
        cue::EditorHost host(std::move(config));
        const cue::GameBuildTarget target{"Windows", "x64", cue::GameBuildConfiguration::Debug};
        CHECK(!host.request_game_build(target).has_value());
        CHECK(!host.run_game().has_value());
        CHECK(host.initialize().has_value());
        auto scene = host.scene()->serialize();
        CHECK(scene.has_value());
        const auto originalScene = scene.take_value();
        const auto originalFile = read(files, sceneFile);

        // 生成 Thread 以外では Service を一度も呼ばない
        bool rejected = false;
        std::thread wrong([&]()
                          { rejected = !host.request_game_build(target).has_value() && !host.game_build_status(); });
        wrong.join();
        CHECK(rejected && observed.starts == 0);
        CHECK(!host.request_game_build({"Other", "x64", cue::GameBuildConfiguration::Debug}).has_value());
        CHECK(host.request_game_build(target).has_value() && host.has_pending_game_build_confirmation());
        CHECK(observed.starts == 0);
        CHECK(!host.request_game_build(target).has_value());
        CHECK(host.confirm_game_build(cue::GameBuildSaveChoice::Cancel).has_value());
        CHECK(!host.has_pending_game_build_confirmation() && observed.starts == 0);

        // 保存失敗では要求を消費するが、Build を後から再開しない
        CHECK(host.request_game_build(target).has_value());
        observedFiles->failSave = true;
        CHECK(!host.confirm_game_build(cue::GameBuildSaveChoice::Save).has_value());
        observedFiles->failSave = false;
        CHECK(!host.has_pending_game_build_confirmation() && observed.starts == 0);
        CHECK(read(files, sceneFile) == originalFile);
        CHECK(host.step().has_value() && observed.starts == 0);
        CHECK(!host.confirm_game_build(cue::GameBuildSaveChoice::SavedFiles).has_value());

        // 保存済み File の Build は Document とディスクの両方を変更しない
        CHECK(host.request_game_build(target).has_value());
        CHECK(host.confirm_game_build(cue::GameBuildSaveChoice::SavedFiles).has_value());
        CHECK(observed.starts == 1 && !host.has_pending_game_build_confirmation());
        scene = host.scene()->serialize();
        CHECK(scene.has_value() && *scene.try_value() == originalScene && read(files, sceneFile) == originalFile);
        CHECK(!host.request_game_build(target).has_value() && !host.run_game().has_value());
        const auto polls = observed.polls;
        CHECK(host.step().has_value() && observed.polls > polls);
        CHECK(host.cancel_game_build().has_value());

        // 保存成功の後でだけ開始し、完了後の起動を許可する
        CHECK(host.request_game_build(target).has_value() && host.has_pending_game_build_confirmation());
        CHECK(host.confirm_game_build(cue::GameBuildSaveChoice::Save).has_value());
        CHECK(observed.starts == 2 && read(files, sceneFile) != originalFile);
        builder->current.state = cue::GameBuildState::Succeeded;
        builder->current.exitCode = 0;
        CHECK(host.step().has_value() && host.run_game().has_value());
        CHECK(observed.launches == 1 && builder->current.isApplicationRunning);
        CHECK(!host.request_game_build(target).has_value());
        builder->current.isApplicationRunning = false;
        CHECK(host.request_game_build(target).has_value() && !host.has_pending_game_build_confirmation());
        CHECK(observed.starts == 3);
        CHECK(host.shutdown().has_value() && observed.shutdowns > 0);
        CHECK(!host.request_game_build(target).has_value() && !host.run_game().has_value());
    }
    CHECK(observed.shutdowns > 0);
    return 0;
}
