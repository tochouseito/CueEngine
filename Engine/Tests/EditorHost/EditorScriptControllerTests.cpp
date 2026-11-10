#include "EditorScriptController.h"

#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <EditorHost/ScriptStatus.h>
#include <EditorIde/IdeService.h>
#include <Project/Project.h>
#include <ProjectBuild/GameBuildService.h>

#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Editor Script Controller failed at %d: %s\n", __LINE__, #__VA_ARGS__);               \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Fixture 内の定数だけから結合した Path の所有権を受け取る
cue::Path path(cue::Result<cue::Path> a_value)
{
    if (!a_value.has_value())
        throw std::runtime_error("fixture path");
    return a_value.take_value();
}
/// @brief 一意の Test Root にだけ小さい入力 File を作る
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief Owner の次の poll 前に状態を進め、外部 Process を使わず要求の順序を観測する
struct BuildFixture final : cue::IGameBuildService
{
    cue::GameBuildStatus current;
    unsigned starts = 0;
    unsigned cancels = 0;
    bool shouldFailStart = false;

    /// @brief 既存二引数 API は製品要求として扱う
    cue::Result<void> start(const cue::Project &a_project, const cue::GameBuildTarget &a_target) override
    {
        return start(a_project, a_target, cue::GameBuildPurpose::App);
    }
    /// @brief IDE / Script の選択と Native 構成を保持する
    cue::Result<void> start(const cue::Project &, const cue::GameBuildTarget &a_target,
                            cue::GameBuildPurpose a_purpose) override
    {
        if (shouldFailStart)
            return cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Fixture.start"});
        ++starts;
        current = {};
        current.target = a_target;
        current.purpose = a_purpose;
        current.state = cue::GameBuildState::Configuring;
        return cue::Result<void>::success();
    }
    /// @brief Build の poll は Host 所有のため、Controller から呼ばれても待機しない
    cue::Result<void> poll() override
    {
        return cue::Result<void>::success();
    }
    /// @brief Controller が所有する要求だけを取り消したことを数える
    cue::Result<void> cancel() override
    {
        ++cancels;
        current.state = cue::GameBuildState::Cancelled;
        return cue::Result<void>::success();
    }
    /// @brief Script Controller は製品を起動しない
    cue::Result<void> launch() override
    {
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Fixture.product_launch"});
    }
    /// @brief Test の所有期間を閉じる
    cue::Result<void> shutdown() override
    {
        return cue::Result<void>::success();
    }
    /// @brief Test が確定した値を借用させる
    const cue::GameBuildStatus &status() const noexcept override
    {
        return current;
    }
    /// @brief 外部 Build の完了を再現する
    void succeed(const cue::Path &a_workspace)
    {
        current.state = cue::GameBuildState::Succeeded;
        current.workspaceDirectory = a_workspace;
    }
};

/// @brief Bridge を起動せず、Source / Workspace の要求内容と完了待ちを確認する
struct IdeFixture final : cue::IEditorIdeService
{
    cue::IdeStatus current;
    unsigned opens = 0;
    unsigned sources = 0;
    unsigned attaches = 0;
    unsigned cancels = 0;

    /// @brief 生成完了後の Workspace の所有 Path を保持する
    cue::Result<void> open_folder(const cue::Path &a_workspace) override
    {
        ++opens;
        current = {};
        current.operation = cue::IdeOperation::OpenFolder;
        current.workspaceDirectory = a_workspace;
        current.state = cue::IdeState::Running;
        return cue::Result<void>::success();
    }
    /// @brief Source Map で解決した File と行を保持する
    cue::Result<void> open_source(const cue::Path &a_workspace, const cue::Path &a_source,
                                  std::uint32_t a_line) override
    {
        ++sources;
        current = {};
        current.operation = cue::IdeOperation::OpenSource;
        current.workspaceDirectory = a_workspace;
        current.source = a_source;
        current.sourceLine = a_line;
        current.state = cue::IdeState::Running;
        return cue::Result<void>::success();
    }
    /// @brief 実 PID を持たない Test では Attach へ進まないことを確認する
    cue::Result<void> attach_debugger(const cue::Path &, std::uint32_t, const cue::Path &) override
    {
        ++attaches;
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Fixture.attach_unexpected"});
    }
    /// @brief 非待機の結果取得だけを許可する
    cue::Result<void> poll() override
    {
        return cue::Result<void>::success();
    }
    /// @brief IDE 自体を終了せず、Bridge 相当の要求だけを取り消す
    cue::Result<void> cancel() override
    {
        ++cancels;
        if (current.state == cue::IdeState::Running)
            current.state = cue::IdeState::Cancelled;
        return cue::Result<void>::success();
    }
    /// @brief Test の所有期間を閉じる
    cue::Result<void> shutdown() override
    {
        return cue::Result<void>::success();
    }
    /// @brief 次の要求まで結果を借用させる
    const cue::IdeStatus &status() const noexcept override
    {
        return current;
    }
};
} // namespace

/// @brief Build / Source Map / IDE / Debug の順序と失敗後の再試行を Controller 単位で検証する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto projectFile = path(fixture.root.join("Game.cueproj"));
    CHECK(write(files, path(fixture.root.join("Source/CMakeLists.txt")), "# fixture\n"));
    CHECK(write(files, path(fixture.root.join("Source/RotateCube.cpp")), "// script fixture\n"));
    CHECK(write(files, path(fixture.root.join("Source/ScriptSources.cueide")),
                "formatVersion=1\nscript.sample.RotateCube=RotateCube.cpp\n"));
    CHECK(files.create_directories(path(fixture.root.join("Content"))).has_value());
    CHECK(write(files, projectFile,
                "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\nname=Controller\n"
                "applicationName=Controller\nsourceRoot=Source\ncontentRoot=Content\ngameTarget=ControllerGame\n"));
    auto project = cue::Project::load(files, projectFile);
    CHECK(project.has_value());
    const auto workspace = fixture.root;
    CHECK(write(files, path(workspace.join("CMakeLists.txt")), "# fixture workspace\n"));
    BuildFixture build;
    IdeFixture ide;
    unsigned reloads = 0;
    unsigned startsDebug = 0;
    bool shouldFailReload = false;
    std::string debugSceneName;
    cue::EditorScriptCallbacks callbacks;
    auto selectedConfiguration = cue::GameBuildConfiguration::Development;
    callbacks.configuration = [&]() { return selectedConfiguration; };
    callbacks.play = []() -> cue::PlayClient * { return nullptr; };
    callbacks.reload = [&]()
    {
        ++reloads;
        return shouldFailReload ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Fixture.reload"})
                                : cue::Result<void>::success();
    };
    callbacks.startDebug = [&](const cue::Scene &a_scene)
    {
        ++startsDebug;
        debugSceneName = a_scene.descriptor().name;
        return cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Fixture.debug_start"});
    };
    cue::EditorScriptController controller(*project.try_value(), files, build, ide, std::move(callbacks));
    CHECK(controller.open_folder().has_value());
    CHECK(build.current.purpose == cue::GameBuildPurpose::IdeProject && build.starts == 1);
    CHECK(controller.status().state == cue::EditorScriptState::PreparingWorkspace);
    CHECK(!controller.open_folder().has_value() && !controller.build_scripts().has_value());
    CHECK(controller.poll().has_value() && ide.opens == 0);
    build.succeed(workspace);
    CHECK(controller.poll().has_value() && ide.opens == 1);
    CHECK(ide.current.workspaceDirectory.utf8() == workspace.utf8());
    CHECK(controller.status().state == cue::EditorScriptState::OpeningIde);
    ide.current.state = cue::IdeState::Succeeded;
    CHECK(controller.poll().has_value() && !controller.status().isBusy);
    CHECK(controller.status().state == cue::EditorScriptState::Succeeded);

    // 同じ Workspace を再 Configure せず、明示 Source Map の解決値を渡す
    CHECK(!controller.open_script("unknown.Type").has_value());
    CHECK(controller.open_script("sample.RotateCube").has_value());
    CHECK(build.starts == 1 && ide.sources == 1 && ide.current.sourceLine == 1);
    CHECK(ide.current.source.utf8() == path(fixture.root.join("Source/RotateCube.cpp")).utf8());
    ide.current.state = cue::IdeState::Succeeded;
    CHECK(controller.poll().has_value());
    CHECK(reloads == 0 && startsDebug == 0 && ide.attaches == 0);

    // DLL の失敗では古い型を再読込せず、次の要求を受け付ける
    CHECK(controller.build_scripts().has_value());
    CHECK(build.current.purpose == cue::GameBuildPurpose::ScriptModule);
    build.current.state = cue::GameBuildState::Failed;
    build.current.error = cue::Error{cue::ErrorCategory::PlatformFailure, "Fixture.compile"};
    CHECK(!controller.poll().has_value());
    CHECK(controller.status().state == cue::EditorScriptState::Failed && reloads == 0);
    CHECK(controller.build_scripts().has_value());
    build.succeed(workspace);
    CHECK(controller.poll().has_value() && reloads == 1);
    CHECK(controller.status().state == cue::EditorScriptState::ReloadingScripts);
    // Play 接続の poll は Host の責務。ここでは Callback の直後で取消する
    CHECK(controller.cancel().has_value());
    CHECK(controller.status().state == cue::EditorScriptState::Cancelled);
    shouldFailReload = true;
    CHECK(controller.build_scripts().has_value());
    build.succeed(workspace);
    CHECK(!controller.poll().has_value() && reloads == 2);
    CHECK(controller.status().state == cue::EditorScriptState::Failed);
    shouldFailReload = false;

    auto scene = cue::Scene::parse("formatVersion=1\nsceneId=8d94c2b1-9b6a-435c-ade7-c01f473c3951\nname=Captured\n");
    CHECK(scene.has_value());
    CHECK(controller.debug_play(*scene.try_value()).has_value());
    build.current.state = cue::GameBuildState::Failed;
    CHECK(!controller.poll().has_value() && startsDebug == 0);
    CHECK(controller.debug_play(*scene.try_value()).has_value());
    scene = cue::Scene::parse("formatVersion=1\nsceneId=8d94c2b1-9b6a-435c-ade7-c01f473c3951\nname=Changed\n");
    CHECK(scene.has_value());
    build.succeed(workspace);
    CHECK(!controller.poll().has_value() && startsDebug == 1 && debugSceneName == "Captured");
    CHECK(controller.status().state == cue::EditorScriptState::Failed);

    // 開始前の入力失敗も Busy を残さず、未完の所有要求だけを Cancel する
    build.shouldFailStart = true;
    CHECK(!controller.build_scripts().has_value() && !controller.status().isBusy);
    build.shouldFailStart = false;
    CHECK(controller.open_folder().has_value());
    const auto cancels = build.cancels;
    CHECK(controller.cancel().has_value() && build.cancels == cancels + 1);
    CHECK(controller.open_folder().has_value());
    build.succeed(workspace);
    CHECK(controller.poll().has_value());
    ide.current.state = cue::IdeState::Failed;
    ide.current.error = cue::Error{cue::ErrorCategory::PlatformFailure, "Fixture.ide"};
    CHECK(!controller.poll().has_value());
    CHECK(controller.status().state == cue::EditorScriptState::Failed);
    const auto unownedCancels = build.cancels;
    build.current.state = cue::GameBuildState::Building;
    CHECK(!controller.build_scripts().has_value());
    CHECK(controller.cancel().has_value() && build.cancels == unownedCancels);
    CHECK(startsDebug == 1 && ide.attaches == 0);
    build.current.state = cue::GameBuildState::Idle;

    // Owner 違反では共有 Service の要求・取消数を変更しない
    const auto starts = build.starts;
    const auto cancellations = ide.cancels;
    bool rejectedThread = false;
    std::thread wrong(
        [&]()
        {
            rejectedThread =
                !controller.open_folder().has_value() && !controller.open_script("sample.RotateCube").has_value() &&
                !controller.build_scripts().has_value() && !controller.debug_play(*scene.try_value()).has_value() &&
                !controller.poll().has_value() && !controller.cancel().has_value();
        });
    wrong.join();
    CHECK(rejectedThread && build.starts == starts && ide.cancels == cancellations);
    // Editor の構成と独立して選択し、操作開始後は変更前の対象構成を保持する
    for (const auto configuration : {cue::GameBuildConfiguration::Debug, cue::GameBuildConfiguration::Development,
                                     cue::GameBuildConfiguration::Release})
    {
        selectedConfiguration = configuration;
        CHECK(controller.build_scripts().has_value());
        CHECK(build.current.target.configuration == configuration);
        selectedConfiguration = cue::GameBuildConfiguration::Development;
        CHECK(controller.poll().has_value() && build.current.target.configuration == configuration);
        CHECK(controller.cancel().has_value());
        selectedConfiguration = configuration;
        CHECK(controller.debug_play(*scene.try_value()).has_value());
        CHECK(build.current.target.configuration == configuration);
        CHECK(controller.cancel().has_value());
    }
    selectedConfiguration = static_cast<cue::GameBuildConfiguration>(99);
    const auto beforeInvalid = build.starts;
    CHECK(!controller.build_scripts().has_value() && build.starts == beforeInvalid && !controller.status().isBusy);
    return 0;
}
