#include <EditorHost/ImGuiManager.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <imgui.h>

#include <Platform/Windows/WindowsPlatform.h>

namespace
{
/// @brief この Test が新規作成した Directory だけを最後に回収する
class TestSettings final
{
public:
    /// @brief 並行 Test と利用者の Layout を分離した保存先を作る
    TestSettings()
    {
        path = std::filesystem::temp_directory_path() /
               ("cue-imgui-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        isCreated = std::filesystem::create_directory(path);
    }

    /// @brief Manager の停止後に Test 自身の保存物を回収する
    ~TestSettings()
    {
        if (isCreated)
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }

    std::filesystem::path path;
    bool isCreated = false;
};

/// @brief Win32 入力、Frame 状態、設定の再読込と生成失敗の Rollback を実 Window で確認する
int test_manager()
{
    TestSettings temporary;
    if (!temporary.isCreated)
    {
        return 1;
    }
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> external(ImGui::CreateContext(),
                                                                          &ImGui::DestroyContext);
    auto systemResult = cue::create_windows_window_system();
    if (!systemResult.has_value())
    {
        return 2;
    }
    auto system = systemResult.take_value();
    auto windowResult = system->create_window({"ImGui Manager Test", {320, 240}});
    if (!windowResult.has_value())
    {
        return 3;
    }
    auto window = windowResult.take_value();
    auto handleResult = cue::borrow_windows_window_handle(*window);
    if (!handleResult.has_value())
    {
        return 4;
    }
    const auto handle = static_cast<HWND>(handleResult.take_value());
    cue::ImGuiManagerConfig config;
    const auto settingsPath = temporary.path / L"設定" / "layout.ini";
    const auto utf8Path = settingsPath.u8string();
    config.settingsFile.assign(utf8Path.begin(), utf8Path.end());
    auto managerResult = cue::ImGuiManager::create(*window, config);
    if (!managerResult.has_value() || ImGui::GetCurrentContext() != external.get())
    {
        return 5;
    }
    auto manager = managerResult.take_value();
    if (manager->end_frame().has_value() || !manager->begin_frame().has_value() ||
        manager->begin_frame().has_value() || !manager->end_frame().has_value() ||
        ImGui::GetCurrentContext() != external.get())
    {
        return 6;
    }
    // 二重生成の失敗が、既存 Manager の Context と Win32 Backend を変えないことを確認する
    auto duplicate = cue::ImGuiManager::create(*window, config);
    if (duplicate.has_value() || duplicate.try_error()->category != cue::ErrorCategory::InvalidState ||
        ImGui::GetCurrentContext() != external.get())
    {
        return 7;
    }
    bool validStyle = false;
    for (int frame = 0; frame < 2; ++frame)
    {
        auto result = manager->build_frame([&]()
        {
            validStyle = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0 &&
                         (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0 &&
                         ImGui::GetIO().Fonts->Fonts.Size == 1 && ImGui::GetStyle().FrameRounding == 4.0f;
            ImGui::SetNextWindowSize({200.0f, 120.0f});
            ImGui::Begin("ManagerTest");
            ImGui::TextUnformatted("CPU UI Frame");
            ImGui::End();
            ImGui::SetNextFrameWantCaptureKeyboard(true);
            ImGui::SetNextFrameWantCaptureMouse(true);
            return cue::Result<void>::success();
        });
        if (!result.has_value())
        {
            return 8;
        }
    }
    auto info = manager->frame_info();
    if (!validStyle || !info.has_value() || info.try_value()->frames != 3 || info.try_value()->vertexCount == 0 ||
        !info.try_value()->wantsKeyboard || !info.try_value()->wantsMouse ||
        ImGui::GetCurrentContext() != external.get())
    {
        return 9;
    }

    // 実 WndProc から Unicode 文字と Keyboard / Mouse を送り、対象 Context 内で取り込みを確認する
    SendMessageW(handle, WM_SETFOCUS, 0, 0);
    SendMessageW(handle, WM_KEYDOWN, 'A', 0x001e0001);
    SendMessageW(handle, WM_CHAR, L'あ', 0);
    SendMessageW(handle, WM_LBUTTONDOWN, 0, MAKELPARAM(10, 10));
    SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    bool receivedKey = false;
    bool receivedMouse = false;
    bool receivedWheel = false;
    bool receivedCharacter = false;
    // 公式 Backend の Trickle Queue は入力順序を保ちながら複数 Frame に分配する
    // 同一 Frame への集約を要求せず、各入力が失われずに届いたことを確認する
    for (int frame = 0; frame < 4; ++frame)
    {
        auto input = manager->build_frame([&]()
        {
            const auto& io = ImGui::GetIO();
            receivedKey |= ImGui::IsKeyDown(ImGuiKey_A);
            receivedMouse |= io.MouseDown[0];
            receivedWheel |= io.MouseWheel == 1.0f;
            for (int index = 0; index < io.InputQueueCharacters.Size; ++index)
            {
                receivedCharacter |= io.InputQueueCharacters[index] == L'あ';
            }
            return cue::Result<void>::success();
        });
        if (!input.has_value())
        {
            return 10;
        }
    }
    SendMessageW(handle, WM_KEYUP, 'A', 0xc01e0001);
    SendMessageW(handle, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    SendMessageW(handle, WM_KILLFOCUS, 0, 0);
    bool receivedFocus = false;
    for (int frame = 0; frame < 4 && !receivedFocus; ++frame)
    {
        auto focus = manager->build_frame([&]()
        {
            receivedFocus = ImGui::GetIO().AppFocusLost && !ImGui::IsKeyDown(ImGuiKey_A) && !ImGui::GetIO().MouseDown[0];
            return cue::Result<void>::success();
        });
        if (!focus.has_value())
        {
            return 10;
        }
    }
    if (!receivedKey || !receivedMouse || !receivedWheel || !receivedCharacter || !receivedFocus)
    {
        std::fprintf(stderr, "key=%d mouse=%d wheel=%d character=%d focus=%d\n", receivedKey, receivedMouse,
                     receivedWheel, receivedCharacter, receivedFocus);
        return 10;
    }

    bool rejectedThread = false;
    std::thread worker([&]()
    {
        auto begin = manager->begin_frame();
        auto end = manager->end_frame();
        auto snapshot = manager->frame_info();
        auto stop = manager->shutdown();
        auto save = manager->save_settings();
        rejectedThread = !begin.has_value() && !end.has_value() && !snapshot.has_value() && !stop.has_value() &&
                         !save.has_value() && begin.try_error()->category == cue::ErrorCategory::WrongThread &&
                         end.try_error()->category == cue::ErrorCategory::WrongThread &&
                         snapshot.try_error()->category == cue::ErrorCategory::WrongThread &&
                         stop.try_error()->category == cue::ErrorCategory::WrongThread &&
                         save.try_error()->category == cue::ErrorCategory::WrongThread;
    });
    worker.join();
    if (!rejectedThread)
    {
        return 11;
    }
    bool rejectedReentry = false;
    auto failure = manager->build_frame([&]()
    {
        rejectedReentry = !manager->shutdown().has_value() && !manager->end_frame().has_value() &&
                          !manager->begin_frame().has_value();
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.ui"});
    });
    auto exception = manager->build_frame([]() -> cue::Result<void>
    {
        throw std::runtime_error("UI failure");
    });
    if (!rejectedReentry || failure.has_value() || failure.try_error()->operation != "Test.ui" ||
        exception.has_value() || ImGui::GetCurrentContext() != external.get() ||
        !manager->build_frame().has_value())
    {
        return 12;
    }
    // 不完全な Begin / Style Stack からも失敗を返し、次の Frame の構築を妨げない
    for (int kind = 0; kind < 3; ++kind)
    {
        auto broken = manager->build_frame([kind]() -> cue::Result<void>
        {
            if (kind == 2)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            }
            else
            {
                ImGui::Begin("InterruptedUi");
            }
            if (kind == 0)
            {
                return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.interrupted_ui"});
            }
            throw std::runtime_error("interrupted UI");
        });
        if (broken.has_value() || ImGui::GetCurrentContext() != external.get() ||
            !manager->build_frame().has_value())
        {
            return 18;
        }
    }
    if (!manager->save_settings().has_value() || !manager->save_settings().has_value() ||
        !std::filesystem::exists(settingsPath) || !manager->shutdown().has_value() ||
        !manager->shutdown().has_value() || manager->begin_frame().has_value() || manager->frame_info().has_value() ||
        ImGui::GetCurrentContext() != external.get())
    {
        return 13;
    }
    manager.reset();

    // 一度保存した Layout を新しい Context に読み込み、同じ Window へ再接続する
    auto reloaded = cue::ImGuiManager::create(*window, config);
    if (!reloaded.has_value())
    {
        return 14;
    }
    auto loaded = reloaded.take_value();
    bool hasLayout = false;
    auto layout = loaded->build_frame([&]()
    {
        hasLayout = std::string(ImGui::SaveIniSettingsToMemory()).find("ManagerTest") != std::string::npos;
        return cue::Result<void>::success();
    });
    if (!layout.has_value() || !hasLayout || !loaded->shutdown().has_value())
    {
        return 15;
    }
    loaded.reset();

    // 保存先の Directory が外部で File に変わった場合も、停止は借用と Context を回収する
    const auto blockedDirectory = temporary.path / "blocked";
    std::filesystem::create_directory(blockedDirectory);
    cue::ImGuiManagerConfig blockedConfig;
    const auto blockedPath = (blockedDirectory / "layout.ini").u8string();
    blockedConfig.settingsFile.assign(blockedPath.begin(), blockedPath.end());
    auto blockedResult = cue::ImGuiManager::create(*window, blockedConfig);
    if (!blockedResult.has_value())
    {
        return 19;
    }
    auto blocked = blockedResult.take_value();
    std::filesystem::remove(blockedDirectory);
    {
        std::ofstream obstruction(blockedDirectory);
        obstruction << "not a directory";
        if (!obstruction.good())
        {
            return 19;
        }
    }
    auto saveFailure = blocked->shutdown();
    if (saveFailure.has_value() || blocked->frame_info().has_value() ||
        ImGui::GetCurrentContext() != external.get() || !blocked->shutdown().has_value())
    {
        return 20;
    }
    blocked.reset();

    // 読めない保存先での途中失敗は Context を破棄し、Handler を残さない
    cue::ImGuiManagerConfig invalid;
    const auto directory = temporary.path.u8string();
    invalid.settingsFile.assign(directory.begin(), directory.end());
    auto failed = cue::ImGuiManager::create(*window, invalid);
    auto available = cue::register_windows_message_handler(*window, [](const auto&) { return cue::WindowsMessageResult{}; });
    if (failed.has_value() || !available.has_value() || ImGui::GetCurrentContext() != external.get() ||
        !cue::unregister_windows_message_handler(*window, available.take_value()).has_value())
    {
        return 16;
    }
    if (!window->destroy().has_value() || !system->pump_events().has_value())
    {
        return 17;
    }
    return 0;
}
} // namespace

/// @brief Window を借用する UI 基盤の正常系と回復可能な失敗を確認する
int main()
{
    return test_manager();
}
