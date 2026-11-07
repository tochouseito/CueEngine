#include "EditorPlayServices.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

#include <DX12/DX12Backend.h>
#include <DX12/DX12RenderDevice.h>
#include <EditorHost/EditorHost.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuild.h>

#include "../Support/FileSystemProbe.h"

namespace
{
#define CUE_PLAY_CONFIG_CHECK(...)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Play configuration test failed at line %d: %s\n", __LINE__, #__VA_ARGS__);           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Manifest の未 Build 状態と通常 File の検査を注入し、毎 Frame の再読込を観測する
class PlayFileProbe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Fixture の FileSystem を借り、PlayHost の配置先だけを明示する
    PlayFileProbe(cue::IFileSystem &a_files, cue::Path a_directory)
        : FileSystemProbe(a_files), directory(std::move(a_directory))
    {
    }
    /// @brief Test exe と PlayHost の配置差を明示的な注入で再現する
    cue::Result<cue::Path> executable_directory() override
    {
        return cue::Result<cue::Path>::success(directory);
    }
    /// @brief 未 Build の Manifest や Directory を指定した実行先を再現する
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_file) override
    {
        if (isManifestMissing && a_file.filename() == "GameModule.cuebuild")
            return cue::Result<std::optional<cue::FileInfo>>::success(std::nullopt);
        if (isExecutableDirectory && a_file.filename() == "CuePlayHost.exe")
            return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Directory});
        return FileSystemProbe::stat(a_file);
    }
    /// @brief Manifest の読込回数を保持し、存在確認を省略する経路でも未存在を再現する
    cue::Result<std::unique_ptr<cue::IFile>> open(const cue::Path &a_file,
                                                  const cue::FileOpenDesc &a_desc = {}) override
    {
        if (a_file.filename() == "GameModule.cuebuild")
        {
            ++manifestReads;
            if (isManifestMissing)
                return cue::Result<std::unique_ptr<cue::IFile>>::failure(
                    {cue::ErrorCategory::PlatformFailure, "Test.manifest_missing"});
        }
        return FileSystemProbe::open(a_file, a_desc);
    }
    cue::Path directory;
    bool isManifestMissing = false;
    bool isExecutableDirectory = false;
    unsigned manifestReads = 0;
};
} // namespace

/// @brief Project 成果物の汎用接続と明示設定の優先・未 Build 時の編集継続を検証する
int main(int a_count, char **a_arguments)
{
    CUE_PLAY_CONFIG_CHECK(a_count == 3);
    auto executable = cue::Path::create(a_arguments[1]);
    auto projectFile = cue::Path::create(a_arguments[2]);
    CUE_PLAY_CONFIG_CHECK(executable.has_value() && projectFile.has_value());
    auto files = cue::create_windows_file_system();
    CUE_PLAY_CONFIG_CHECK(files.has_value());
    auto project = cue::Project::load(**files.try_value(), *projectFile.try_value());
    CUE_PLAY_CONFIG_CHECK(project.has_value() && project.try_value()->descriptor().gameTarget);
    PlayFileProbe probe(**files.try_value(), executable.try_value()->parent());
    auto artifacts =
        cue::load_game_build_artifacts(probe, *project.try_value(), cue::current_game_build_configuration());
    CUE_PLAY_CONFIG_CHECK(artifacts.has_value());
    const auto module = artifacts.try_value()->moduleFile;

    auto resolved = cue::resolve_editor_play_config(probe, &*project.try_value(), {}, {});
    CUE_PLAY_CONFIG_CHECK(resolved.has_value());
    CUE_PLAY_CONFIG_CHECK(resolved.try_value()->module.utf8() == module.utf8());
    CUE_PLAY_CONFIG_CHECK(resolved.try_value()->executable.utf8() == executable.try_value()->utf8());
    probe.isManifestMissing = true;
    const auto readsBeforeExplicit = probe.manifestReads;
    auto half = cue::resolve_editor_play_config(probe, &*project.try_value(), *executable.try_value(), {});
    CUE_PLAY_CONFIG_CHECK(!half.has_value() && half.try_error()->operation == "EditorHost.play.partial_configuration");
    auto explicitPaths = cue::resolve_editor_play_config(probe, &*project.try_value(), *executable.try_value(), module);
    CUE_PLAY_CONFIG_CHECK(explicitPaths.has_value() && probe.manifestReads == readsBeforeExplicit);
    auto missing = cue::resolve_editor_play_config(probe, &*project.try_value(), {}, {});
    CUE_PLAY_CONFIG_CHECK(!missing.has_value());
    auto noProject = cue::resolve_editor_play_config(probe, nullptr, {}, {});
    CUE_PLAY_CONFIG_CHECK(!noProject.has_value());
    probe.isExecutableDirectory = true;
    auto invalidExe = cue::resolve_editor_play_config(probe, nullptr, *executable.try_value(), module);
    CUE_PLAY_CONFIG_CHECK(!invalidExe.has_value());
    probe.isExecutableDirectory = false;
    auto relative = cue::Path::create("CuePlayHost.exe");
    CUE_PLAY_CONFIG_CHECK(relative.has_value());
    auto relativeExe = cue::resolve_editor_play_config(probe, nullptr, *relative.try_value(), module);
    CUE_PLAY_CONFIG_CHECK(!relativeExe.has_value());

    // 未 Build の Game Target でも Editor の Window / Preview を開き、Frame 中に Manifest を再試行しない
    auto injected = std::make_unique<PlayFileProbe>(**files.try_value(), executable.try_value()->parent());
    injected->isManifestMissing = true;
    auto *observed = injected.get();
    cue::EditorHostConfig config;
    config.window.title = "CueEngine Project Play Configuration Test";
    config.project = project.take_value();
    config.fileSystem = std::move(injected);
    config.createBackend = [](cue::IFileSystem *a_files, cue::Clock &a_clock)
    {
        auto backend = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp, {}, a_files, &a_clock);
        if (!backend.has_value())
            return cue::Result<std::unique_ptr<cue::IBackend>>::failure(*backend.try_error());
        return cue::Result<std::unique_ptr<cue::IBackend>>::success(backend.take_value());
    };
    cue::EditorHost host(std::move(config));
    CUE_PLAY_CONFIG_CHECK(host.initialize().has_value());
    const auto startupReads = observed->manifestReads;
    for (int index = 0; index < 3; ++index)
        CUE_PLAY_CONFIG_CHECK(host.step().has_value());
    CUE_PLAY_CONFIG_CHECK(observed->manifestReads == startupReads);
    CUE_PLAY_CONFIG_CHECK(host.preview_world() && host.scene());
    CUE_PLAY_CONFIG_CHECK(host.shutdown().has_value());
    return 0;
}
