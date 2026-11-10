#include <ProjectBuild/Windows/WindowsGameBuildService.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Foundation/Utf8.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Platform/FileSystem.h>
#include <Project/Project.h>

#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "GameBuildService failed at %d: %s\n", __LINE__, #__VA_ARGS__);                       \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief Fixture 内で検証済みの結合だけを簡潔に行う
cue::Path path(cue::Result<cue::Path> a_path)
{
    if (!a_path.has_value())
        throw std::runtime_error("fixture path");
    return a_path.take_value();
}
/// @brief Fixture の親を作り、完全転送で小さい設定を保存する
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}
/// @brief Job 孫 Process の出力を比較するため所有値で読む
std::string read(cue::IFileSystem &a_files, const cue::Path &a_file)
{
    auto data = a_files.read_all(a_file, 65536);
    if (!data.has_value())
        return {};
    const auto &bytes = *data.try_value();
    return {bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
/// @brief UI と同じ非待機 poll を有界期間だけ繰り返す。失敗状態は正常な検証対象とする
bool wait(cue::IGameBuildService &a_service, cue::GameBuildState a_state, unsigned a_timeout = 10000)
{
    const auto deadline = GetTickCount64() + a_timeout;
    while (GetTickCount64() < deadline)
    {
        auto observed = a_service.poll();
        (void)observed;
        if (a_service.status().state == a_state)
        {
            // 失敗回収の Job / 排他を後続 poll まで保持する場合も完了させる
            for (unsigned count = 0; count < 5; ++count)
            {
                auto cleanup = a_service.poll();
                (void)cleanup;
                Sleep(1);
            }
            return true;
        }
        Sleep(1);
    }
    std::fprintf(stderr, "Build output: %s\n", a_service.status().log.c_str());
    return false;
}

/// @brief 通常 CTest とは別の明示モードで、Editor と同じ専用 Tree の実 Build 接続を確認する
int engine_smoke(int a_count, wchar_t **a_arguments)
{
    CHECK(a_count == 8 || a_count == 9);
    std::vector<cue::Path> paths;
    for (int index = 2; index < 7; ++index)
    {
        auto text = cue::utf16_to_utf8(a_arguments[index]);
        CHECK(text.has_value());
        auto converted = cue::Path::create(*text.try_value());
        CHECK(converted.has_value() && converted.try_value()->is_absolute());
        paths.push_back(converted.take_value());
    }
    const std::wstring_view configuration(a_arguments[7]);
    CHECK(configuration == L"Debug" || configuration == L"Development" || configuration == L"Release");
    const auto selected = configuration == L"Debug"         ? cue::GameBuildConfiguration::Debug
                          : configuration == L"Development" ? cue::GameBuildConfiguration::Development
                                                            : cue::GameBuildConfiguration::Release;
    cue::GameBuildPurpose purpose = cue::GameBuildPurpose::App;
    if (a_count == 9)
    {
        const std::wstring_view value(a_arguments[8]);
        CHECK(value == L"app" || value == L"script" || value == L"ide");
        purpose = value == L"script" ? cue::GameBuildPurpose::ScriptModule
                  : value == L"ide"  ? cue::GameBuildPurpose::IdeProject
                                     : cue::GameBuildPurpose::App;
    }
    auto files = cue::create_windows_file_system();
    CHECK(files.has_value());
    auto project = cue::Project::load(**files.try_value(), paths[1]);
    CHECK(project.has_value());
    auto created = cue::create_windows_game_build_service({paths[0], paths[2], paths[3], paths[4]});
    CHECK(created.has_value());
    auto service = created.take_value();
    auto started = service->start(*project.try_value(), {"Windows", "x64", selected}, purpose);
    if (!started.has_value())
    {
        std::fprintf(stderr, "Engine build start failed: %s (%lld)\n", started.try_error()->operation.c_str(),
                     static_cast<long long>(started.try_error()->nativeCode));
        return 1;
    }
    const auto deadline = GetTickCount64() + 15 * 60 * 1000;
    bool isComplete = false;
    std::optional<cue::Error> pollingError;
    while (GetTickCount64() < deadline)
    {
        auto progressed = service->poll();
        if (!progressed.has_value())
            pollingError = *progressed.try_error();
        const auto state = service->status().state;
        if (state == cue::GameBuildState::Succeeded || state == cue::GameBuildState::Failed ||
            state == cue::GameBuildState::Cancelled || pollingError)
        {
            isComplete = true;
            break;
        }
        Sleep(10);
    }
    const bool succeeded = isComplete && service->status().state == cue::GameBuildState::Succeeded;
    // 外部出力は終端で一度だけ行い、Build 中は Service の保持値だけを更新する
    const auto &status = service->status();
    std::fwrite(status.log.data(), 1, status.log.size(), stdout);
    std::fflush(stdout);
    if (!isComplete)
        std::fputs("Engine build timed out after 15 minutes\n", stderr);
    const auto *error = status.error ? &*status.error : (pollingError ? &*pollingError : nullptr);
    if (error)
        std::fprintf(stderr, "Engine build failed: %s (%lld)\n", error->operation.c_str(),
                     static_cast<long long>(error->nativeCode));
    auto stopped = service->shutdown();
    if (!stopped.has_value())
        std::fprintf(stderr, "Engine build shutdown failed: %s\n", stopped.try_error()->operation.c_str());
    return succeeded && stopped.has_value() ? 0 : 1;
}
} // namespace

/// @brief 実 Process / Pipe / Job を使い、Build 中も Owner が進行できる契約を確認する
int wmain(int a_count, wchar_t **a_arguments)
{
    if (a_count > 1 && std::wstring_view(a_arguments[1]) == L"--engine-smoke")
        return engine_smoke(a_count, a_arguments);
    CHECK(a_count == 2);
    auto helperText = cue::utf16_to_utf8(a_arguments[1]);
    CHECK(helperText.has_value());
    auto helper = cue::Path::create(*helperText.try_value());
    CHECK(helper.has_value());
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    auto root = path(fixture.root.join("日本語 Project With Spaces"));
    auto engine = path(fixture.root.join("Engine Tools"));
    auto vcpkg = path(fixture.root.join("vcpkg"));
    auto toolchain = path(vcpkg.join("scripts/buildsystems/vcpkg.cmake"));
    CHECK(write(files, path(engine.join("CMakeLists.txt")), "# fixture\n"));
    CHECK(write(files, path(engine.join("CMakePresets.json")), "{}\n"));
    CHECK(write(files, path(engine.join("cmake/CueGameSdk.cmake")), "# fixture SDK\n"));
    CHECK(write(files, path(engine.join("Templates/GameWorkspace/CMakeLists.txt")), "# fixture workspace\n"));
    CHECK(write(files, path(engine.join("Templates/GameWorkspace/CMakePresets.json")), "{}\n"));
    CHECK(write(files, toolchain, "# fixture\n"));
    CHECK(write(files, path(root.join("Source/CMakeLists.txt")), "# fixture\n"));
    auto sceneFile = path(root.join("Content/Scenes/Start.cuescene"));
    CHECK(write(files, sceneFile, "formatVersion=1\nsceneId=8d94c2b1-9b6a-435c-ade7-c01f473c3951\nname=Fixture\n"));
    auto projectFile = path(root.join("Fixture.cueproj"));
    CHECK(write(
        files, projectFile,
        "formatVersion=1\nprojectId=8f533c49-d416-47ea-98c1-026822b1e6ed\nname=Fixture\napplicationName=FixtureApp\n"
        "sourceRoot=Source\ncontentRoot=Content\nstartupScene=Scenes/Start.cuescene\ngameTarget=FixtureGame\n"));
    auto project = cue::Project::load(files, projectFile);
    CHECK(project.has_value());
    cue::GameBuildTools tools{engine, *helper.try_value(), toolchain, vcpkg};
    auto created = cue::create_windows_game_build_service(tools);
    CHECK(created.has_value());
    auto service = created.take_value();
    const cue::GameBuildTarget target{"Windows", "x64", cue::GameBuildConfiguration::Debug};
    CHECK(service->reset_project().has_value());
    CHECK(!service->launch().has_value());
    CHECK(!service->start(*project.try_value(), {"Linux", "x64", cue::GameBuildConfiguration::Debug}).has_value());
    CHECK(service->status().state == cue::GameBuildState::Idle);
    bool rejectedThread = false;
    std::thread wrong(
        [&]()
        {
            auto reset = service->reset_project();
            rejectedThread = !service->poll().has_value() && !reset.has_value() &&
                             reset.try_error()->category == cue::ErrorCategory::WrongThread;
        });
    wrong.join();
    CHECK(rejectedThread);
    CHECK(service->start(*project.try_value(), target).has_value());
    CHECK(!service->reset_project().has_value());
    CHECK(!service->start(*project.try_value(), target).has_value());
    auto second = cue::create_windows_game_build_service(tools);
    CHECK(second.has_value());
    CHECK(!(*second.try_value())->start(*project.try_value(), target).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    CHECK(service->status().log.find("CONFIGURE FIXTURE") != std::string::npos);
    CHECK(service->status().log.find("BUILD FIXTURE") != std::string::npos);
    CHECK(service->status().exitCode == 0);
    CHECK(service->status().purpose == cue::GameBuildPurpose::App);
    const auto output = service->status().outputDirectory;
    CHECK(output.filename() == "App");
    CHECK(service->launch().has_value());
    CHECK(service->status().isApplicationRunning && !service->launch().has_value());
    CHECK(!service->reset_project().has_value());
    CHECK(service->status().isApplicationRunning && service->status().state == cue::GameBuildState::Succeeded);
    CHECK(!service->start(*project.try_value(), target).has_value());
    CHECK(!(*second.try_value())->start(*project.try_value(), target).has_value());
    // Service を閉じても製品は継続し、実行完了の File を自分で公開できる
    CHECK(service->shutdown().has_value());
    const auto deadline = GetTickCount64() + 5000;
    while (read(files, path(output.join("Finished.txt"))) != "finished" && GetTickCount64() < deadline)
        Sleep(10);
    CHECK(read(files, path(output.join("Finished.txt"))) == "finished");
    CHECK(service->shutdown().has_value() && !service->start(*project.try_value(), target).has_value());
    service = second.take_value();
    auto mode = path(root.join("Mode.txt"));
    CHECK(write(files, mode, "configure-fail"));
    CHECK(service->start(*project.try_value(), target).has_value());
    CHECK(wait(*service, cue::GameBuildState::Failed));
    CHECK(service->status().exitCode == 17 && service->status().error.has_value());
    CHECK(!service->launch().has_value());
    CHECK(write(files, mode, "build-fail"));
    CHECK(service->start(*project.try_value(), target).has_value());
    CHECK(wait(*service, cue::GameBuildState::Failed));
    CHECK(service->status().exitCode == 23);
    CHECK(write(files, mode, "hang"));
    CHECK(service->start(*project.try_value(), target).has_value());
    auto heartbeat = path(root.join("Heartbeat.txt"));
    const auto heartbeatDeadline = GetTickCount64() + 5000;
    while (read(files, heartbeat).empty() && GetTickCount64() < heartbeatDeadline)
    {
        CHECK(service->poll().has_value());
        Sleep(5);
    }
    CHECK(!read(files, heartbeat).empty());
    CHECK(service->cancel().has_value() && wait(*service, cue::GameBuildState::Cancelled));
    const auto stoppedHeartbeat = read(files, heartbeat);
    Sleep(100);
    CHECK(read(files, heartbeat) == stoppedHeartbeat);
    // 取消後も同じ Service / Tree を再利用できる
    CHECK(write(files, mode, "large-log"));
    CHECK(service->start(*project.try_value(), target).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded, 20000));
    CHECK(service->status().isLogTruncated && service->status().log.size() <= 1024 * 1024);
    CHECK(cue::is_valid_utf8_text(service->status().log));
    CHECK(service->status().log.find("LAST FIXTURE LINE") != std::string::npos);
    CHECK(service->status().log.find('\0') == std::string::npos);
    // 切替後は旧成功 Package を起動できず、ログや出力先も新 Project の表示へ流入しない
    CHECK(service->reset_project().has_value());
    CHECK(service->status().state == cue::GameBuildState::Idle && service->status().log.empty());
    CHECK(!service->status().error && !service->status().exitCode && !service->status().applicationExitCode);
    CHECK(service->status().outputDirectory.is_empty() && service->status().executable.is_empty());
    CHECK(service->status().workspaceDirectory.is_empty() && service->status().moduleFile.is_empty() &&
          service->status().symbolsFile.is_empty() && !service->status().isLogTruncated);
    CHECK(!service->launch().has_value());
    for (const auto *failure : {"wrong-config", "wrong-scene"})
    {
        CHECK(write(files, mode, failure));
        CHECK(service->start(*project.try_value(), target).has_value());
        CHECK(wait(*service, cue::GameBuildState::Failed));
        CHECK(!service->launch().has_value());
    }
    CHECK(write(files, mode, "success"));
    CHECK(service->start(*project.try_value(), target).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    // 完了後に失われた Marker を、前回の成功状態だけで起動しない
    CHECK(files.remove(path(service->status().outputDirectory.join("GamePackage.cuepackage"))).has_value());
    CHECK(!service->launch().has_value());
    CHECK(service->shutdown().has_value());
    // 排他 File に偽装した Hard Link でユーザー File を開かず、修復後は同じ Tree を再利用する
    const auto lockFile = path(root.join("Intermediate/.cue-editor-build.lock"));
    const auto userFile = path(root.join("UserOwned.txt"));
    CHECK(write(files, userFile, "User data must remain"));
    CHECK(files.remove(lockFile).has_value());
    auto nativeLock = cue::utf8_to_utf16(lockFile.utf8());
    auto nativeUser = cue::utf8_to_utf16(userFile.utf8());
    CHECK(nativeLock.has_value() && nativeUser.has_value());
    CHECK(CreateHardLinkW(nativeLock.try_value()->c_str(), nativeUser.try_value()->c_str(), nullptr));
    auto recovered = cue::create_windows_game_build_service(tools);
    CHECK(recovered.has_value());
    service = recovered.take_value();
    CHECK(!service->start(*project.try_value(), target).has_value());
    CHECK(read(files, userFile) == "User data must remain");
    CHECK(files.remove(lockFile).has_value());
    CHECK(files.create_directories(lockFile).has_value());
    CHECK(!service->start(*project.try_value(), target).has_value());
    CHECK(files.remove(lockFile).has_value());
    CHECK(service->start(*project.try_value(), target).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    CHECK(service->shutdown().has_value());
    // IDE はゲーム Root を返し、Solution の生成形式や残存 File を選択の根拠にしない
    auto toolsService = cue::create_windows_game_build_service(tools);
    CHECK(toolsService.has_value());
    service = toolsService.take_value();
    CHECK(!service->start(*project.try_value(), target, static_cast<cue::GameBuildPurpose>(99)).has_value());
    CHECK(write(files, mode, "success"));
    CHECK(service->start(*project.try_value(), target, cue::GameBuildPurpose::IdeProject).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    CHECK(service->status().purpose == cue::GameBuildPurpose::IdeProject);
    CHECK(service->status().workspaceDirectory.utf8() == root.utf8());
    CHECK(service->status().log.find("/Intermediate/IDE/WorkspaceV1/") != std::string::npos);
    CHECK(read(files, path(root.join("CMakeLists.txt"))) == "# fixture workspace\n");
    CHECK(read(files, path(root.join("CMakeUserPresets.json"))).find(engine.utf8()) != std::string::npos);
    CHECK(service->status().log.find("BUILD FIXTURE") == std::string::npos);
    CHECK(service->status().moduleFile.is_empty() && service->status().executable.is_empty());
    CHECK(!service->launch().has_value());
    const auto workspace = service->status().workspaceDirectory;
    CHECK(service->start(*project.try_value(), target, cue::GameBuildPurpose::ScriptModule).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    CHECK(service->status().purpose == cue::GameBuildPurpose::ScriptModule);
    CHECK(service->status().workspaceDirectory.utf8() == workspace.utf8());
    CHECK(service->status().moduleFile.filename() == "FixtureGame.dll");
    CHECK(service->status().symbolsFile.filename() == "FixtureGame.pdb");
    CHECK(!service->launch().has_value());
    for (const auto *failure : {"module-bad-hash", "module-wrong-config", "missing-symbols"})
    {
        CHECK(write(files, mode, failure));
        CHECK(service->start(*project.try_value(), target, cue::GameBuildPurpose::ScriptModule).has_value());
        CHECK(wait(*service, cue::GameBuildState::Failed));
        CHECK(!service->launch().has_value());
    }
    for (const auto *legacy : {"sln-only", "missing-solution", "ambiguous-solution"})
    {
        CHECK(write(files, mode, legacy));
        CHECK(service->start(*project.try_value(), target, cue::GameBuildPurpose::IdeProject).has_value());
        CHECK(wait(*service, cue::GameBuildState::Succeeded));
        CHECK(service->status().workspaceDirectory.utf8() == root.utf8());
    }
    // 既存のユーザー定義 CMake は保ち、所有していない Local Presets は上書きせず診断する
    auto localPresets = path(root.join("CMakeUserPresets.json"));
    const auto ownedPresets = read(files, localPresets);
    CHECK(write(files, path(root.join("CMakeLists.txt")), "# user workspace\n"));
    CHECK(write(files, localPresets, "{\"version\":9}\n"));
    CHECK(!service->start(*project.try_value(), target, cue::GameBuildPurpose::IdeProject).has_value());
    CHECK(read(files, localPresets) == "{\"version\":9}\n");
    CHECK(read(files, path(root.join("CMakeLists.txt"))) == "# user workspace\n");
    CHECK(write(files, localPresets, ownedPresets));
    CHECK(service->start(*project.try_value(), target, cue::GameBuildPurpose::IdeProject).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    CHECK(read(files, path(root.join("CMakeLists.txt"))) == "# user workspace\n");
    // 起動 Scene のない Project でも IDE / DLL は作れる。製品要求だけを拒否する
    auto descriptor = read(files, projectFile);
    descriptor.erase(descriptor.find("startupScene="), std::string("startupScene=Scenes/Start.cuescene\n").size());
    CHECK(write(files, projectFile, descriptor));
    auto noScene = cue::Project::load(files, projectFile);
    CHECK(noScene.has_value());
    CHECK(!service->start(*noScene.try_value(), target).has_value());
    CHECK(write(files, mode, "success"));
    CHECK(service->start(*noScene.try_value(), target, cue::GameBuildPurpose::ScriptModule).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    // 同じ Service を別 Project へ再利用し、旧 Root を含む IDE 結果を引き継がない
    CHECK(service->reset_project().has_value());
    const auto otherRoot = path(fixture.root.join("Other Project"));
    const auto otherFile = path(otherRoot.join("Other.cueproj"));
    auto otherDescriptor = descriptor;
    otherDescriptor.replace(otherDescriptor.find("8f533c49-d416-47ea-98c1-026822b1e6ed"), 36,
                            "3f533c49-d416-47ea-98c1-026822b1e6ed");
    otherDescriptor.replace(otherDescriptor.find("applicationName=FixtureApp"),
                            std::string("applicationName=FixtureApp").size(),
                            "applicationName=OtherFixtureApp");
    CHECK(write(files, path(otherRoot.join("Source/CMakeLists.txt")), "# fixture\n"));
    CHECK(files.create_directories(path(otherRoot.join("Content"))).has_value());
    CHECK(write(files, otherFile, otherDescriptor));
    auto otherProject = cue::Project::load(files, otherFile);
    CHECK(otherProject.has_value());
    CHECK(service->start(*otherProject.try_value(), target, cue::GameBuildPurpose::IdeProject).has_value());
    CHECK(wait(*service, cue::GameBuildState::Succeeded));
    CHECK(service->status().workspaceDirectory.utf8() == otherRoot.utf8());
    CHECK(service->status().moduleFile.is_empty() && !service->launch().has_value());
    CHECK(service->shutdown().has_value());
    CHECK(!service->reset_project().has_value());
    return 0;
}
