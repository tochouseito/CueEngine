#include <ScriptPlay/PlayClient.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <EditorIde/Windows/WindowsIdeService.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Scene/Scene.h>
#include <ScriptPlay/PlayServer.h>

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script Debug Play failed at %d: %s (native %lu)\n", __LINE__, #__VA_ARGS__,          \
                         GetLastError());                                                                              \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief この Test が起動した Play 子だけへ接続し、失敗でも自前の Debug Event を解放する
class DebugConnection final
{
  public:
    /// @brief Debugger としての接続前状態を作る
    DebugConnection() = default;
    /// @brief 保留 Event を続行し、接続を外してから PlayClient の Job 回収へ戻す
    ~DebugConnection()
    {
        release();
        if (m_processId)
            DebugActiveProcessStop(m_processId);
    }
    /// @brief Owner Thread が保持する子へだけ接続する
    bool attach(DWORD a_processId)
    {
        if (!a_processId || !DebugActiveProcess(a_processId))
            return false;
        m_processId = a_processId;
        return DebugSetProcessKillOnExit(FALSE) != FALSE;
    }
    /// @brief 非待機で既存 Event を処理し、最初の Breakpoint だけを保留できる
    bool pump(bool a_holdBreakpoint = false)
    {
        DEBUG_EVENT event{};
        while (WaitForDebugEvent(&event, 0))
        {
            if (event.dwProcessId != m_processId)
                return false;
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)
                CloseHandle(event.u.CreateProcessInfo.hFile);
            if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
                CloseHandle(event.u.LoadDll.hFile);
            if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
                event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && a_holdBreakpoint)
            {
                m_held = event;
                return true;
            }
            const auto status = event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
                                        event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT
                                    ? DBG_EXCEPTION_NOT_HANDLED
                                    : DBG_CONTINUE;
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status))
                return false;
            if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            {
                m_processId = 0;
                break;
            }
        }
        return true;
    }
    /// @brief 明示的に停止している Native Breakpoint があるか返す
    bool is_held() const noexcept
    {
        return m_held.has_value();
    }
    /// @brief 接続時の Breakpoint を続行し、子へ Script 開始を処理させる
    bool release() noexcept
    {
        if (!m_held)
            return true;
        const auto event = *m_held;
        m_held.reset();
        return ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE) != FALSE;
    }

  private:
    DWORD m_processId = 0;
    std::optional<DEBUG_EVENT> m_held;
};

/// @brief 不正な制御 Message のために Wire 整数を明示する。Game / World は生成しない
void append(std::vector<std::uint8_t> &a_bytes, std::uint64_t a_value, unsigned a_width)
{
    for (unsigned index = 0; index < a_width; ++index)
        a_bytes.push_back(static_cast<std::uint8_t>(a_value >> (index * 8)));
}

/// @brief Debugger 確認を迂回する生 Message と、別 Session の開始許可を Server 自身が拒否する
int test_untrusted_continue()
{
    for (const bool wrongSession : {false, true})
    {
        const auto text = "\\\\.\\pipe\\CueEngine.Play." + std::to_string(GetCurrentProcessId()) +
                          (wrongSession ? ".DebugWrongSession" : ".DebugNoAttachment");
        const std::wstring name(text.begin(), text.end());
        struct PipeOwner final
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            /// @brief この Test が生成した Server 端だけを必ず閉じる
            ~PipeOwner()
            {
                if (value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }
        } pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                65536, 65536, 0, nullptr)};
        CHECK(pipe.value != INVALID_HANDLE_VALUE);
        (void)ConnectNamedPipe(pipe.value, nullptr);
        auto created = cue::PlayServer::connect(text, 731, true);
        CHECK(created.has_value());
        auto server = created.take_value();
        CHECK(server->send_debug_waiting().has_value());
        std::vector<std::uint8_t> bytes;
        for (const unsigned kind : {1u, 4u})
        {
            append(bytes, 0x4355504c, 4);
            append(bytes, 2, 4);
            append(bytes, kind, 4);
            append(bytes, kind == 1 ? 4 : 0, 4);
            append(bytes, wrongSession && kind == 4 ? 732 : 731, 8);
            append(bytes, 0, 8);
            if (kind == 1)
                append(bytes, 0, 4);
        }
        DWORD written = 0;
        CHECK(WriteFile(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size());
        auto commands = server->poll();
        CHECK(!commands.has_value());
        CHECK(commands.try_error()->operation == (wrongSession ? "Play.header" : "Play.debugger_not_attached"));
    }
    return 0;
}

/// @brief 保存せずに同じ Game SDK の Script を割り当てた Scene を作る
cue::Result<cue::Scene> make_scene()
{
    cue::SceneDescriptor descriptor;
    descriptor.sceneId = "8d94c2b1-9b6a-435c-ade7-c01f473c3951";
    descriptor.name = "Debugger Gate";
    cue::SceneObjectDescriptor object;
    object.objectId = "9a00d731-7759-424c-9ade-fd086d624551";
    object.name = "Cube";
    object.transform = cue::math::floatTransform{};
    object.scripts = {{"6c78eb99-aee8-4602-9fba-66fa96080f99", "sample.RotateCube", 1, true, {}}};
    descriptor.objects.push_back(std::move(object));
    return cue::Scene::create(std::move(descriptor));
}

/// @brief OS が渡す Wide Path を UTF-8 の所有値へ変換する
cue::Result<cue::Path> argument_path(const wchar_t *a_value)
{
    auto text = cue::utf16_to_utf8(a_value);
    return text.has_value() ? cue::Path::create(*text.try_value()) : cue::Result<cue::Path>::failure(*text.try_error());
}

/// @brief 任意の実 VS 検証だけを実行する。通常の CTest ではこの Mode を選ばない
int attach_visual_studio(int a_count, wchar_t **a_arguments)
{
    CHECK(a_count == 7);
    std::vector<cue::Path> paths;
    for (int index = 2; index < a_count; ++index)
    {
        auto path = argument_path(a_arguments[index]);
        CHECK(path.has_value() && path.try_value()->is_absolute());
        paths.push_back(path.take_value());
    }
    auto files = cue::create_windows_file_system();
    CHECK(files.has_value());
    auto scene = cue::Scene::load(**files.try_value(), paths[2]);
    CHECK(scene.has_value());
    auto createdClient = cue::PlayClient::create({paths[0], paths[1], true, 3});
    CHECK(createdClient.has_value());
    auto client = createdClient.take_value();
    auto createdIde = cue::create_windows_ide_service({paths[3], 110000});
    CHECK(createdIde.has_value());
    auto ide = createdIde.take_value();
    CHECK(client->start_debug(*scene.try_value()).has_value());
    const auto deadline = GetTickCount64() + 120000;
    while (client->state() != cue::PlayState::WaitingForDebugger && GetTickCount64() < deadline)
    {
        CHECK(client->poll().has_value());
        if (client->state() == cue::PlayState::Failed)
        {
            std::fprintf(stderr, "Debug Play failed: %.*s\n", static_cast<int>(client->error().size()),
                         client->error().data());
            return 1;
        }
        Sleep(10);
    }
    CHECK(client->state() == cue::PlayState::WaitingForDebugger);
    std::printf("Waiting for Visual Studio: Play PID %u\n", client->process_id());
    std::fflush(stdout);
    CHECK(ide->attach_debugger(paths[4], client->process_id(), paths[0]).has_value());
    while (ide->status().state == cue::IdeState::Running && GetTickCount64() < deadline)
    {
        auto polled = ide->poll();
        if (!polled.has_value())
        {
            std::fprintf(stderr, "Visual Studio attach failed: %s (%lld)\n", polled.try_error()->operation.c_str(),
                         static_cast<long long>(polled.try_error()->nativeCode));
            return 1;
        }
        CHECK(client->poll().has_value());
        if (client->state() == cue::PlayState::Failed)
        {
            std::fprintf(stderr, "Debug Play failed during attach: %.*s\n", static_cast<int>(client->error().size()),
                         client->error().data());
            return 1;
        }
        Sleep(10);
    }
    if (ide->status().state != cue::IdeState::Succeeded)
    {
        const auto &status = ide->status();
        std::fprintf(stderr, "Visual Studio attach failed: %s\n",
                     status.error ? status.error->operation.c_str() : status.message.c_str());
        return 1;
    }
    CHECK(client->is_debugger_attached());
    std::printf("Visual Studio attached: IDE PID %u\n", ide->status().ideProcessId);
    std::fflush(stdout);
    CHECK(client->continue_debug().has_value());
    while (client->state() != cue::PlayState::Stopped && GetTickCount64() < deadline)
    {
        CHECK(client->poll().has_value());
        if (client->state() == cue::PlayState::Failed)
        {
            std::fprintf(stderr, "Debug Play failed after continue: %.*s\n", static_cast<int>(client->error().size()),
                         client->error().data());
            return 1;
        }
        Sleep(10);
    }
    CHECK(client->state() == cue::PlayState::Stopped);
    CHECK(ide->shutdown().has_value());
    std::puts("Debug Play completed normally");
    return 0;
}
} // namespace

/// @brief 実 Native Debugger で通常起動期限を超えて停止し、その後の Script 開始と正常 Stop を確認する
int wmain(int a_count, wchar_t **a_arguments)
{
    if (a_count > 1 && std::wstring_view(a_arguments[1]) == L"--vs-attach")
        return attach_visual_studio(a_count, a_arguments);
    CHECK(a_count == 3);
    CHECK(test_untrusted_continue() == 0);
    auto executable = argument_path(a_arguments[1]);
    auto module = argument_path(a_arguments[2]);
    CHECK(executable.has_value() && module.has_value());
    auto created = cue::PlayClient::create({executable.take_value(), module.take_value(), true, 0, true});
    CHECK(created.has_value());
    auto client = created.take_value();
    auto scene = make_scene();
    CHECK(scene.has_value() && client->start_debug(*scene.try_value()).has_value());
    auto deadline = GetTickCount64() + 8000;
    while (client->state() != cue::PlayState::WaitingForDebugger && GetTickCount64() < deadline)
    {
        CHECK(client->poll().has_value() && client->state() != cue::PlayState::Failed);
        Sleep(1);
    }
    CHECK(client->state() == cue::PlayState::WaitingForDebugger && client->types().empty());
    DebugConnection debugger;
    CHECK(debugger.attach(client->process_id()));
    deadline = GetTickCount64() + 8000;
    while (!debugger.is_held() && GetTickCount64() < deadline)
    {
        CHECK(debugger.pump(true));
        Sleep(1);
    }
    CHECK(debugger.is_held() && client->is_debugger_attached());
    CHECK(client->continue_debug().has_value());
    // 旧 Starting 期限 15 秒を超えても、実際に Debugger 停止中の子を失敗にしない
    deadline = GetTickCount64() + 15500;
    while (GetTickCount64() < deadline)
    {
        CHECK(client->poll().has_value() && client->state() == cue::PlayState::Starting && client->types().empty());
        Sleep(10);
    }
    CHECK(debugger.release());
    deadline = GetTickCount64() + 8000;
    while (client->state() != cue::PlayState::Running && GetTickCount64() < deadline)
    {
        CHECK(debugger.pump() && client->poll().has_value() && client->state() != cue::PlayState::Failed);
        Sleep(1);
    }
    CHECK(client->state() == cue::PlayState::Running && client->types().size() == 1);
    deadline = GetTickCount64() + 8000;
    while (!client->view_snapshot() && GetTickCount64() < deadline)
    {
        CHECK(debugger.pump() && client->poll().has_value() && client->state() == cue::PlayState::Running);
        Sleep(1);
    }
    CHECK(client->view_snapshot());
    CHECK(!client->continue_debug().has_value());
    CHECK(client->stop().has_value());
    deadline = GetTickCount64() + 8000;
    while (client->state() != cue::PlayState::Stopped && GetTickCount64() < deadline)
    {
        CHECK(debugger.pump() && client->poll().has_value() && client->state() != cue::PlayState::Failed);
        Sleep(1);
    }
    CHECK(client->state() == cue::PlayState::Stopped && !client->process_id());
    return 0;
}
