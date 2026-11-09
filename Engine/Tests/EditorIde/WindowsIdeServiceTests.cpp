#include <EditorIde/Windows/WindowsIdeService.h>

#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "IDE Service failed %d: %s\n", __LINE__, #__VA_ARGS__);                               \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)
/// @brief Fixture の Path だけを所有値にする
cue::Path path(cue::Result<cue::Path> a_value)
{
    if (!a_value.has_value())
        throw std::runtime_error("fixture path");
    return a_value.take_value();
}
/// @brief 手動 IDE を起動せず、実 Process / Pipe の終了を有限時間だけ進める
bool wait(cue::IEditorIdeService &a_service, cue::IdeState a_expected)
{
    const auto deadline = GetTickCount64() + 3000;
    while (a_service.status().state == cue::IdeState::Running && GetTickCount64() < deadline)
    {
        auto result = a_service.poll();
        (void)result;
        Sleep(1);
    }
    return a_service.status().state == a_expected;
}
} // namespace

/// @brief Bridge の非同期所有・要求照合・期限・取消を実 Process で確認する
int wmain(int a_count, wchar_t **a_arguments)
{
    CHECK(a_count == 2);
    auto helperText = cue::utf16_to_utf8(a_arguments[1]);
    CHECK(helperText.has_value());
    auto helper = cue::Path::create(*helperText.try_value());
    CHECK(helper.has_value());
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    auto &files = *fixture.files;
    const auto bridge = path(fixture.root.join("日本語 Tools/VisualStudioBridge.exe"));
    CHECK(files.create_directories(bridge.parent()).has_value());
    CHECK(files.copy_file(*helper.try_value(), bridge, false).has_value());
    auto created = cue::create_windows_ide_service({bridge, 1000});
    CHECK(created.has_value());
    auto service = created.take_value();
    auto workspace = [&](const char *a_name)
    {
        auto value = path(fixture.root.join(a_name));
        auto directory = files.create_directories(value);
        if (!directory.has_value())
            throw std::runtime_error("fixture workspace directory");
        auto cmake = path(value.join("CMakeLists.txt"));
        auto written = files.write_all(cmake, {}, true);
        if (!written.has_value())
            throw std::runtime_error("fixture workspace");
        return value;
    };
    const auto good = workspace("日本語 Good");
    const auto hang = workspace("Hang");
    const auto bad = workspace("BadIdentity");
    const auto large = workspace("Large");
    const auto failed = workspace("Failed");
    const auto source = path(fixture.root.join("日本語 Source.cpp"));
    CHECK(files.write_all(source, {}, true).has_value());
    const auto missing = path(fixture.root.join("Missing"));
    CHECK(!service->open_folder(missing).has_value());
    CHECK(!service->open_folder(source).has_value());
    CHECK(!service->open_folder(fixture.root).has_value());
    CHECK(service->open_folder(good).has_value());
    CHECK(!service->open_folder(good).has_value());
    CHECK(wait(*service, cue::IdeState::Succeeded));
    CHECK(service->status().ideProcessId == 1234 && service->status().requestId == 1);
    CHECK(service->status().workspaceDirectory.utf8() == good.utf8());
    CHECK(service->open_source(good, source, 12).has_value());
    CHECK(wait(*service, cue::IdeState::Succeeded));
    CHECK(service->status().sourceLine == 12);
    CHECK(!service->open_source(good, source, 0).has_value());
    bool rejected = false;
    std::thread wrong([&]() { rejected = !service->open_folder(good).has_value(); });
    wrong.join();
    CHECK(rejected);
    for (const auto *value : {&bad, &large, &failed})
    {
        CHECK(service->open_folder(*value).has_value());
        CHECK(wait(*service, cue::IdeState::Failed) && service->status().error);
    }
    CHECK(service->open_folder(hang).has_value());
    CHECK(wait(*service, cue::IdeState::Failed));
    CHECK(service->status().error->operation == "EditorIde.timeout");
    CHECK(service->open_folder(hang).has_value());
    CHECK(service->cancel().has_value());
    CHECK(wait(*service, cue::IdeState::Cancelled));
    CHECK(service->open_folder(good).has_value());
    CHECK(wait(*service, cue::IdeState::Succeeded));
    CHECK(!service->attach_debugger(good, GetCurrentProcessId(), bridge).has_value());
    CHECK(service->shutdown().has_value());
    CHECK(!service->open_folder(good).has_value());
    return 0;
}
