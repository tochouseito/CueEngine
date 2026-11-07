#include <ScriptPlay/PlayClient.h>
#include <ScriptPlay/PlayServer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Foundation/Uuid.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Scene/Scene.h>

namespace cue
{
namespace
{
constexpr std::uint32_t k_magic = 0x4355504c;
constexpr std::uint32_t k_version = 1;
constexpr std::size_t k_headerBytes = 32;
constexpr std::size_t k_maxMessageBytes = 5 * 1024 * 1024;
constexpr std::size_t k_maxQueueBytes = 8 * 1024 * 1024;
constexpr std::uint64_t k_startTimeoutMs = 15000;
constexpr std::uint64_t k_stopTimeoutMs = 2000;
constexpr std::uint64_t k_debugWaitTimeoutMs = 120000;
std::atomic<std::uint64_t> g_nextSession = 1;

enum class Message : std::uint32_t
{
    Start = 1,
    Stop = 2,
    SetProperty = 3,
    ContinueDebug = 4,
    Types = 101,
    Ready = 102,
    Ack = 103,
    Error = 104,
    DebugWaiting = 105
};

/// @brief Win32 Handle の一意所有。所有 Job の Close は子 Process の回収にもなる
struct Handle final
{
    HANDLE value = nullptr;
    /// @brief 空の Handle を作る
    Handle() = default;
    /// @brief OS の生成成功値を受け取る
    explicit Handle(HANDLE a_value) : value(a_value)
    {
    }
    /// @brief OS の参照を閉じる
    ~Handle()
    {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    /// @brief 所有権を移す
    Handle(Handle &&a_other) noexcept : value(std::exchange(a_other.value, nullptr))
    {
    }
    /// @brief 旧 Handle を閉じて所有権を移す
    Handle &operator=(Handle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            value = std::exchange(a_other.value, nullptr);
        }
        return *this;
    }
    /// @brief 有効な参照だけを閉じる
    void reset() noexcept
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = nullptr;
    }
};

/// @brief 通信失敗に共通の操作名と Native Code を付ける
Error transport_error(const char *a_operation)
{
    return {ErrorCategory::PlatformFailure, a_operation, GetLastError()};
}

/// @brief Protocol の整数を Little Endian で所有 Buffer へ書く
struct Writer final
{
    std::vector<std::uint8_t> bytes;
    /// @brief 32 bit 値を書き込む
    void u32(std::uint32_t a_value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(a_value >> shift));
    }
    /// @brief 64 bit 値を書き込む
    void u64(std::uint64_t a_value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(a_value >> shift));
    }
    /// @brief 長さ付き UTF-8 文字列を書き込む
    void text(std::string_view a_value)
    {
        u32(static_cast<std::uint32_t>(a_value.size()));
        bytes.insert(bytes.end(), a_value.begin(), a_value.end());
    }
    /// @brief Boolean と Number の型・値を明示して書く
    void property(const ScriptPropertyValue &a_value)
    {
        text(a_value.propertyId);
        u32(static_cast<std::uint32_t>(a_value.kind));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &a_value.number, sizeof(bits));
        u64(bits);
        u32(a_value.boolean ? 1 : 0);
    }
};

/// @brief 境界外 Read をせず Protocol を解釈する。失敗後の値は公開しない
struct Reader final
{
    const std::uint8_t *data;
    std::size_t size;
    std::size_t offset = 0;
    bool valid = true;
    /// @brief 32 bit 値を読む
    std::uint32_t u32()
    {
        if (!valid || size - offset < 4)
        {
            valid = false;
            return 0;
        }
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(data[offset++]) << shift;
        return value;
    }
    /// @brief 64 bit 値を読む
    std::uint64_t u64()
    {
        const auto low = u32();
        const auto high = u32();
        return low | (static_cast<std::uint64_t>(high) << 32);
    }
    /// @brief 上限付き文字列を複写する。空文字列の許可は利用側で判断する
    std::string text(std::size_t a_limit = 4096)
    {
        const auto count = u32();
        if (!valid || count > a_limit || count > size - offset)
        {
            valid = false;
            return {};
        }
        std::string value(reinterpret_cast<const char *>(data + offset), count);
        offset += count;
        if ((!value.empty() && !is_valid_utf8_text(value)))
            valid = false;
        return value;
    }
    /// @brief 既知の型と有限値だけを受け取る
    ScriptPropertyValue property()
    {
        ScriptPropertyValue value;
        value.propertyId = text(128);
        value.kind = static_cast<ScriptPropertyKind>(u32());
        const auto bits = u64();
        std::memcpy(&value.number, &bits, sizeof(bits));
        const auto boolean = u32();
        value.boolean = boolean != 0;
        if ((value.kind != ScriptPropertyKind::Number && value.kind != ScriptPropertyKind::Boolean) || boolean > 1 ||
            !std::isfinite(value.number) || value.propertyId.empty())
            valid = false;
        return value;
    }
    /// @brief 不正値と余剰 Payload がないことを確認する
    bool complete() const noexcept
    {
        return valid && offset == size;
    }
};

struct Packet final
{
    Message kind;
    std::uint64_t request = 0;
    std::vector<std::uint8_t> payload;
};

/// @brief Named Pipe を非待機で進める。送受信の所有 Buffer は上限を設けて保持する
struct Endpoint final
{
    Handle pipe;
    bool isConnected = false;
    bool isParent = false;
    bool isClosed = false;
    bool isWriteClosed = false;
    DWORD expectedPeerProcess = 0;
    std::uint64_t session = 0;
    std::vector<std::uint8_t> incoming;
    std::vector<std::uint8_t> outgoing;
    std::size_t sent = 0;

    /// @brief Header と Payload を一つの送信 Queue に積む。容量不足では Queue を変更しない
    Result<void> queue(Message a_kind, std::uint64_t a_request, const Writer &a_payload)
    {
        if (a_payload.bytes.size() > k_maxMessageBytes ||
            outgoing.size() - sent + k_headerBytes + a_payload.bytes.size() > k_maxQueueBytes)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Play.queue_limit"});
        Writer header;
        header.u32(k_magic);
        header.u32(k_version);
        header.u32(static_cast<std::uint32_t>(a_kind));
        header.u32(static_cast<std::uint32_t>(a_payload.bytes.size()));
        header.u64(session);
        header.u64(a_request);
        if (sent)
        {
            outgoing.erase(outgoing.begin(), outgoing.begin() + static_cast<std::ptrdiff_t>(sent));
            sent = 0;
        }
        // 先に容量を確保し、途中確保失敗で Protocol の半端な Header を残さない
        outgoing.reserve(outgoing.size() + header.bytes.size() + a_payload.bytes.size());
        outgoing.insert(outgoing.end(), header.bytes.begin(), header.bytes.end());
        outgoing.insert(outgoing.end(), a_payload.bytes.begin(), a_payload.bytes.end());
        return Result<void>::success();
    }

    /// @brief 到着済み Message だけを回収する。一回の呼出しで無制限に CPU を占有しない
    Result<std::vector<Packet>> pump()
    {
        if (!isClosed && !isConnected && isParent)
        {
            const auto connected = ConnectNamedPipe(pipe.value, nullptr);
            const auto error = connected ? ERROR_SUCCESS : GetLastError();
            if (connected || error == ERROR_PIPE_CONNECTED || error == ERROR_NO_DATA)
            {
                isConnected = true;
                isWriteClosed = error == ERROR_NO_DATA;
                ULONG peer = 0;
                const bool hasPeer = GetNamedPipeClientProcessId(pipe.value, &peer) != FALSE;
                // 終了済み Client は PID の照会が失敗し得るが、残った応答を EOF まで検証する
                if ((hasPeer && peer != expectedPeerProcess) || (!hasPeer && error != ERROR_NO_DATA))
                    return Result<std::vector<Packet>>::failure({ErrorCategory::InvalidArgument, "Play.peer_process"});
            }
            else if (error == ERROR_PIPE_LISTENING)
                return Result<std::vector<Packet>>::success({});
            else
                return Result<std::vector<Packet>>::failure(transport_error("Play.connect"));
        }
        if (!isConnected)
            return Result<std::vector<Packet>>::success({});
        // PIPE_NOWAIT の Write は空き容量ぶんだけ進む。UI Thread で Pipe 排出を待たない
        for (std::size_t budget = 0; !isClosed && !isWriteClosed && sent < outgoing.size() && budget < 256 * 1024;)
        {
            DWORD written = 0;
            const auto count = static_cast<DWORD>(std::min<std::size_t>(outgoing.size() - sent, 65536));
            if (!WriteFile(pipe.value, outgoing.data() + sent, count, &written, nullptr))
            {
                const auto error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA)
                {
                    // 書込不能でも、反対方向の応答は EOF まで回収する
                    isWriteClosed = true;
                    break;
                }
                if (error == ERROR_PIPE_BUSY)
                    break;
                return Result<std::vector<Packet>>::failure(transport_error("Play.write"));
            }
            if (written == 0)
                break;
            sent += written;
            budget += written;
        }
        if (sent == outgoing.size())
        {
            outgoing.clear();
            sent = 0;
        }
        for (std::size_t budget = 0; !isClosed && budget < 256 * 1024;)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr))
            {
                if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED)
                {
                    isClosed = true;
                    break;
                }
                return Result<std::vector<Packet>>::failure(transport_error("Play.peek"));
            }
            if (!available)
                break;
            const auto count = std::min<DWORD>(available, 65536);
            if (incoming.size() + count > k_maxQueueBytes)
                return Result<std::vector<Packet>>::failure({ErrorCategory::InvalidArgument, "Play.input_limit"});
            const auto previous = incoming.size();
            incoming.resize(previous + count);
            DWORD received = 0;
            if (!ReadFile(pipe.value, incoming.data() + previous, count, &received, nullptr))
            {
                incoming.resize(previous);
                const auto error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
                {
                    isClosed = true;
                    break;
                }
                if (error == ERROR_NO_DATA)
                    break;
                return Result<std::vector<Packet>>::failure(transport_error("Play.read"));
            }
            incoming.resize(previous + received);
            budget += received;
            if (!received)
                break;
        }
        std::vector<Packet> packets;
        std::size_t consumed = 0;
        while (incoming.size() - consumed >= k_headerBytes && packets.size() < 256)
        {
            Reader header{incoming.data() + consumed, k_headerBytes};
            const auto magic = header.u32();
            const auto version = header.u32();
            const auto kind = static_cast<Message>(header.u32());
            const auto length = header.u32();
            const auto id = header.u64();
            const auto request = header.u64();
            if (magic != k_magic || version != k_version || id != session || length > k_maxMessageBytes)
                return Result<std::vector<Packet>>::failure({ErrorCategory::InvalidArgument, "Play.header"});
            if (incoming.size() - consumed - k_headerBytes < length)
                break;
            const auto begin = incoming.begin() + static_cast<std::ptrdiff_t>(consumed + k_headerBytes);
            packets.push_back({kind, request, std::vector<std::uint8_t>(begin, begin + length)});
            consumed += k_headerBytes + length;
        }
        incoming.erase(incoming.begin(), incoming.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (isClosed && !incoming.empty() && packets.size() < 256)
            return Result<std::vector<Packet>>::failure({ErrorCategory::InvalidArgument, "Play.truncated_message"});
        return Result<std::vector<Packet>>::success(std::move(packets));
    }
};

/// @brief Windows の引数規則に従い、埋め込み引用符と末尾 Backslash を Escape する
std::wstring quote(std::wstring_view a_value)
{
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const auto c : a_value)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        output.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        output += c;
    }
    output.append(slashes * 2, L'\\');
    output += L'"';
    return output;
}

/// @brief 子が返した Metadata を DLL と独立した値へ変換する
Result<std::vector<ScriptTypeDescriptor>> read_types(Reader &a_reader)
{
    const auto count = a_reader.u32();
    if (count > 256)
        return Result<std::vector<ScriptTypeDescriptor>>::failure({ErrorCategory::InvalidArgument, "Play.type_limit"});
    std::vector<ScriptTypeDescriptor> types;
    std::unordered_set<std::string> typeIds;
    for (std::uint32_t index = 0; index < count && a_reader.valid; ++index)
    {
        ScriptTypeDescriptor type;
        type.typeId = a_reader.text(128);
        type.displayName = a_reader.text(256);
        type.schemaVersion = a_reader.u32();
        const auto properties = a_reader.u32();
        if (properties > 64 || type.typeId.empty() || type.displayName.empty() || type.schemaVersion == 0)
            return Result<std::vector<ScriptTypeDescriptor>>::failure(
                {ErrorCategory::InvalidArgument, "Play.type_descriptor"});
        for (std::uint32_t property = 0; property < properties && a_reader.valid; ++property)
        {
            auto value = a_reader.property();
            auto name = a_reader.text(256);
            type.properties.push_back(
                {std::move(value.propertyId), std::move(name), value.kind, value.number, value.boolean});
        }
        auto validated = validate_script_type(type);
        if (!validated.has_value() || !typeIds.insert(type.typeId).second)
            return Result<std::vector<ScriptTypeDescriptor>>::failure(
                {ErrorCategory::InvalidArgument, "Play.type_metadata"});
        types.push_back(std::move(type));
    }
    if (!a_reader.complete())
        return Result<std::vector<ScriptTypeDescriptor>>::failure({ErrorCategory::InvalidArgument, "Play.types"});
    return Result<std::vector<ScriptTypeDescriptor>>::success(std::move(types));
}
} // namespace

struct PlayClient::State final
{
    PlayClientConfig config;
    std::thread::id owner = std::this_thread::get_id();
    PlayState state = PlayState::Idle;
    Endpoint endpoint;
    Handle job;
    Handle process;
    std::uint64_t deadline = 0;
    std::uint64_t nextRequest = 1;
    std::vector<ScriptTypeDescriptor> types;
    std::vector<PlayPropertyAck> acks;
    std::unordered_map<std::uint64_t, PlayPropertyAck> pending;
    std::string error;
    bool isDescribe = false;
    bool hasMetadata = false;
    bool hasReady = false;
    bool wasStopRequested = false;
    bool isDebug = false;
    bool hasDebugWaiting = false;
    bool hasDebugContinued = false;

    /// @brief 所有子 Process だけを終了して通信を閉じ、失敗を保存する
    void fail(std::string a_error)
    {
        error = std::move(a_error);
        state = PlayState::Failed;
        job.reset();
        endpoint.pipe.reset();
        process.reset();
    }
};

PlayClient::PlayClient(ConstructionKey, PlayClientConfig a_config) : m_state(std::make_unique<State>())
{
    m_state->config = std::move(a_config);
}

Result<std::unique_ptr<PlayClient>> PlayClient::create(PlayClientConfig a_config)
{
    if (!a_config.executable.is_absolute() || !a_config.module.is_absolute())
        return Result<std::unique_ptr<PlayClient>>::failure({ErrorCategory::InvalidArgument, "Play.absolute_paths"});
    try
    {
        return Result<std::unique_ptr<PlayClient>>::success(
            std::make_unique<PlayClient>(ConstructionKey{}, std::move(a_config)));
    }
    catch (...)
    {
        return Result<std::unique_ptr<PlayClient>>::failure({ErrorCategory::PlatformFailure, "Play.create"});
    }
}

PlayClient::~PlayClient()
{
    // 明示 Close は Stop Callback の機会を与える。停止しない子は期限後 Job で回収する
    if (m_state && m_state->process.value && std::this_thread::get_id() == m_state->owner)
    {
        const auto requested = stop();
        (void)requested;
        const auto deadline = GetTickCount64() + k_stopTimeoutMs;
        while (m_state->process.value && GetTickCount64() < deadline)
        {
            const auto progressed = poll();
            (void)progressed;
            if (m_state->process.value)
                Sleep(1);
        }
    }
}

Result<void> PlayClient::launch(bool a_describe, bool a_isDebug)
{
    auto &state = *m_state;
    if (std::this_thread::get_id() != state.owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Play.launch"});
    if (state.state == PlayState::Starting || state.state == PlayState::Running ||
        state.state == PlayState::Describing || state.state == PlayState::Stopping ||
        state.state == PlayState::WaitingForDebugger)
        return Result<void>::failure({ErrorCategory::InvalidState, "Play.busy"});
    state.job.reset();
    state.process.reset();
    state.endpoint = {};
    state.endpoint.isParent = true;
    state.endpoint.session = g_nextSession.fetch_add(1);
    state.nextRequest = 1;
    state.acks.clear();
    state.pending.clear();
    state.error.clear();
    state.isDescribe = a_describe;
    state.hasMetadata = false;
    state.hasReady = false;
    state.wasStopRequested = false;
    state.isDebug = a_isDebug;
    state.hasDebugWaiting = false;
    state.hasDebugContinued = false;
    const auto pipeName = L"\\\\.\\pipe\\CueEngine.Play." + std::to_wstring(GetCurrentProcessId()) + L"." +
                          std::to_wstring(state.endpoint.session);
    state.endpoint.pipe = Handle(CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    if (state.endpoint.pipe.value == INVALID_HANDLE_VALUE)
        return Result<void>::failure(transport_error("Play.create_pipe"));
    // 非待機 Connect を先に発行し、子からの接続を受け付ける
    (void)ConnectNamedPipe(state.endpoint.pipe.value, nullptr);
    state.job = Handle(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!state.job.value ||
        !SetInformationJobObject(state.job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Result<void>::failure(transport_error("Play.create_job"));
    auto executable = utf8_to_utf16(state.config.executable.utf8());
    auto module = utf8_to_utf16(state.config.module.utf8());
    auto directory = utf8_to_utf16(state.config.executable.parent().utf8());
    if (!executable.has_value() || !module.has_value() || !directory.has_value())
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Play.path_encoding"});
    auto command = quote(*executable.try_value()) + L" --pipe " + quote(pipeName) + L" --session " +
                   std::to_wstring(state.endpoint.session) + L" --module " + quote(*module.try_value());
    if (a_describe)
        command += L" --describe";
    if (a_isDebug)
        command += L" --debug-wait";
    if (state.config.isHeadless)
        command += L" --headless";
    if (state.config.maxTicks)
        command += L" --max-ticks " + std::to_wstring(state.config.maxTicks);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.try_value()->c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, directory.try_value()->c_str(), &startup,
                        &process))
        return Result<void>::failure(transport_error("Play.create_process"));
    state.process = Handle(process.hProcess);
    state.endpoint.expectedPeerProcess = process.dwProcessId;
    Handle thread(process.hThread);
    if (!AssignProcessToJobObject(state.job.value, state.process.value))
    {
        const auto error = transport_error("Play.assign_job");
        TerminateProcess(state.process.value, 1);
        state.process.reset();
        return Result<void>::failure(error);
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
        return Result<void>::failure(transport_error("Play.resume"));
    state.state = a_describe ? PlayState::Describing : PlayState::Starting;
    state.deadline = GetTickCount64() + k_startTimeoutMs;
    return Result<void>::success();
}

Result<void> PlayClient::describe()
{
    try
    {
        auto launched = launch(true);
        if (!launched.has_value() && launched.try_error()->category != ErrorCategory::WrongThread &&
            launched.try_error()->operation != "Play.busy")
            m_state->fail(launched.try_error()->operation);
        return launched;
    }
    catch (...)
    {
        m_state->fail("Play.describe.exception");
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Play.describe.exception"});
    }
}

Result<void> PlayClient::start(const Scene &a_scene)
{
    return start_session(a_scene, false);
}

Result<void> PlayClient::start_debug(const Scene &a_scene)
{
    return start_session(a_scene, true);
}

Result<void> PlayClient::start_session(const Scene &a_scene, bool a_isDebug)
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Play.start"});
    try
    {
        auto text = a_scene.serialize();
        if (!text.has_value())
            return Result<void>::failure(*text.try_error());
        Writer payload;
        payload.text(*text.try_value());
        auto launched = launch(false, a_isDebug);
        if (!launched.has_value())
        {
            if (launched.try_error()->category != ErrorCategory::WrongThread &&
                launched.try_error()->operation != "Play.busy")
                m_state->fail(launched.try_error()->operation);
            return launched;
        }
        auto queued = m_state->endpoint.queue(Message::Start, 0, payload);
        if (!queued.has_value())
            m_state->fail(queued.try_error()->operation);
        return queued;
    }
    catch (...)
    {
        m_state->fail("Play.start.exception");
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Play.start.exception"});
    }
}

Result<void> PlayClient::continue_debug()
{
    auto &state = *m_state;
    if (std::this_thread::get_id() != state.owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Play.continue_debug"});
    if (state.state != PlayState::WaitingForDebugger || !state.isDebug || !state.hasDebugWaiting ||
        state.hasDebugContinued)
        return Result<void>::failure({ErrorCategory::InvalidState, "Play.debug_not_waiting"});
    if (!is_debugger_attached())
        return Result<void>::failure({ErrorCategory::InvalidState, "Play.debugger_not_attached"});
    try
    {
        auto queued = state.endpoint.queue(Message::ContinueDebug, 0, {});
        if (!queued.has_value())
            return queued;
        state.hasDebugContinued = true;
        state.state = PlayState::Starting;
        state.deadline = GetTickCount64() + k_startTimeoutMs;
        return Result<void>::success();
    }
    catch (...)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Play.continue_debug.exception"});
    }
}

Result<void> PlayClient::poll()
{
    auto &state = *m_state;
    if (std::this_thread::get_id() != state.owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Play.poll"});
    if (!state.process.value)
        return Result<void>::success();
    try
    {
        auto packets = state.endpoint.pump();
        // Pipe EOF と Process 終了は競合するため、終了 Code を先に確認して診断を分類する
        DWORD exitCode = STILL_ACTIVE;
        const bool hasExited = WaitForSingleObject(state.process.value, 0) == WAIT_OBJECT_0;
        if (hasExited)
            GetExitCodeProcess(state.process.value, &exitCode);
        // 接続を外して継続した場合も、Breakpoint 中の経過時間を通常の起動期限へ持ち越さない
        if (state.state == PlayState::Starting && state.isDebug && is_debugger_attached())
            state.deadline = GetTickCount64() + k_startTimeoutMs;
        if (!packets.has_value())
        {
            // 子が Pipe を閉じてから Process が終了するまでの短い区間も停止期限で回収する
            if (packets.try_error()->nativeCode == ERROR_BROKEN_PIPE ||
                packets.try_error()->nativeCode == ERROR_PIPE_NOT_CONNECTED)
            {
                if (!hasExited && state.state != PlayState::Stopping)
                {
                    state.state = PlayState::Stopping;
                    state.deadline = GetTickCount64() + k_stopTimeoutMs;
                }
            }
            else
            {
                state.fail(packets.try_error()->operation);
                return Result<void>::success();
            }
        }
        if (packets.has_value())
        {
            for (const auto &packet : *packets.try_value())
            {
                Reader reader{packet.payload.data(), packet.payload.size()};
                if (packet.kind == Message::DebugWaiting && packet.request == 0 && reader.complete() && state.isDebug &&
                    !state.isDescribe && !state.hasDebugWaiting && !state.hasMetadata)
                {
                    state.hasDebugWaiting = true;
                    if (state.state != PlayState::Stopping)
                    {
                        state.state = PlayState::WaitingForDebugger;
                        state.deadline = GetTickCount64() + k_debugWaitTimeoutMs;
                    }
                }
                else if (packet.kind == Message::Types && packet.request == 0 &&
                         (!state.isDebug || state.hasDebugContinued))
                {
                    auto types = read_types(reader);
                    if (!types.has_value() || state.hasMetadata)
                    {
                        state.fail("Play.metadata_protocol");
                        return Result<void>::success();
                    }
                    state.types = types.take_value();
                    state.hasMetadata = true;
                }
                else if (packet.kind == Message::Ready && packet.request == 0 && reader.complete() &&
                         !state.isDescribe && state.hasMetadata && !state.hasReady)
                {
                    state.hasReady = true;
                    if (state.state != PlayState::Stopping)
                        state.state = PlayState::Running;
                }
                else if (packet.kind == Message::Ack && packet.request != 0 && !state.isDescribe)
                {
                    PlayPropertyAck ack;
                    ack.requestId = packet.request;
                    const auto accepted = reader.u32();
                    ack.isAccepted = accepted == 1;
                    ack.objectId = reader.text(128);
                    ack.scriptInstanceId = reader.text(128);
                    ack.value = reader.property();
                    ack.error = reader.text();
                    const auto expected = state.pending.find(packet.request);
                    if (!reader.complete() || accepted > 1 || state.acks.size() >= 4096 ||
                        expected == state.pending.end() || expected->second.objectId != ack.objectId ||
                        expected->second.scriptInstanceId != ack.scriptInstanceId ||
                        expected->second.value.propertyId != ack.value.propertyId ||
                        expected->second.value.kind != ack.value.kind)
                    {
                        state.fail("Play.ack_protocol");
                        return Result<void>::success();
                    }
                    state.pending.erase(expected);
                    state.acks.push_back(std::move(ack));
                }
                else if (packet.kind == Message::Error && packet.request == 0)
                {
                    auto error = reader.text();
                    state.fail(reader.complete() ? std::move(error) : "Play.error_protocol");
                    return Result<void>::success();
                }
                else
                {
                    state.fail("Play.message_protocol");
                    return Result<void>::success();
                }
            }
        }
        // 子が通信だけ閉じて停止しない場合も、有界な回収へ進める
        if (state.endpoint.isClosed && !hasExited && state.state != PlayState::Stopping)
        {
            state.state = PlayState::Stopping;
            state.deadline = GetTickCount64() + k_stopTimeoutMs;
        }
        if (hasExited && (!state.endpoint.isConnected || (state.endpoint.isClosed && state.endpoint.incoming.empty())))
        {
            if (exitCode != 0 ||
                (!state.wasStopRequested && (!state.hasMetadata || (!state.isDescribe && !state.hasReady))))
                state.fail("Play process exited: " + std::to_string(exitCode));
            else
            {
                state.state = PlayState::Stopped;
                state.process.reset();
                state.endpoint.pipe.reset();
                state.job.reset();
            }
        }
        else if ((state.state == PlayState::Starting || state.state == PlayState::Describing ||
                  state.state == PlayState::WaitingForDebugger || state.state == PlayState::Stopping) &&
                 GetTickCount64() >= state.deadline &&
                 // Script の初期化中に停止していても、接続中だけ通常の起動期限を抑止する
                 !(state.state != PlayState::Stopping && state.isDebug && is_debugger_attached()))
            state.fail(state.state == PlayState::Stopping             ? "Play.stop_timeout"
                       : state.state == PlayState::WaitingForDebugger ? "Play.debugger_timeout"
                                                                      : "Play.start_timeout");
        return Result<void>::success();
    }
    catch (...)
    {
        state.fail("Play.poll.exception");
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Play.poll.exception"});
    }
}

Result<void> PlayClient::stop()
{
    auto &state = *m_state;
    if (std::this_thread::get_id() != state.owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "Play.stop"});
    if (state.state == PlayState::Stopping || !state.process.value)
        return Result<void>::success();
    try
    {
        auto queued = state.endpoint.queue(Message::Stop, 0, {});
        // Queue が満杯でも停止期限を設定し、操作不能な子を必ず Job から回収する
        state.state = PlayState::Stopping;
        state.wasStopRequested = true;
        state.deadline = GetTickCount64() + k_stopTimeoutMs;
        return queued;
    }
    catch (...)
    {
        state.fail("Play.stop.exception");
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Play.stop.exception"});
    }
}

Result<std::uint64_t> PlayClient::set_property(std::string_view a_objectId, std::string_view a_scriptInstanceId,
                                               const ScriptPropertyValue &a_value)
{
    auto &state = *m_state;
    if (std::this_thread::get_id() != state.owner)
        return Result<std::uint64_t>::failure({ErrorCategory::WrongThread, "Play.set_property"});
    if (state.state != PlayState::Running || !is_valid_uuid_text(a_objectId) ||
        !is_valid_uuid_text(a_scriptInstanceId) || !is_valid_script_id(a_value.propertyId) ||
        !std::isfinite(a_value.number) || state.pending.size() >= 256 || !state.nextRequest ||
        (a_value.kind != ScriptPropertyKind::Number && a_value.kind != ScriptPropertyKind::Boolean))
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidArgument, "Play.set_property"});
    try
    {
        Writer payload;
        payload.text(a_objectId);
        payload.text(a_scriptInstanceId);
        payload.property(a_value);
        const auto request = state.nextRequest;
        state.pending.emplace(
            request,
            PlayPropertyAck{request, false, std::string(a_objectId), std::string(a_scriptInstanceId), a_value, {}});
        auto queued = state.endpoint.queue(Message::SetProperty, request, payload);
        if (!queued.has_value())
            state.pending.erase(request);
        else
            ++state.nextRequest;
        return queued.has_value() ? Result<std::uint64_t>::success(request)
                                  : Result<std::uint64_t>::failure(*queued.try_error());
    }
    catch (...)
    {
        state.pending.erase(state.nextRequest);
        return Result<std::uint64_t>::failure({ErrorCategory::PlatformFailure, "Play.set_property.exception"});
    }
}

PlayState PlayClient::state() const noexcept
{
    return m_state->state;
}
const std::vector<ScriptTypeDescriptor> &PlayClient::types() const noexcept
{
    return m_state->types;
}
std::string_view PlayClient::error() const noexcept
{
    return m_state->error;
}
std::uint64_t PlayClient::session_id() const noexcept
{
    return m_state->endpoint.session;
}

std::uint32_t PlayClient::process_id() const noexcept
{
    return std::this_thread::get_id() == m_state->owner && m_state->process.value &&
                   WaitForSingleObject(m_state->process.value, 0) == WAIT_TIMEOUT
               ? GetProcessId(m_state->process.value)
               : 0;
}

bool PlayClient::is_debugging() const noexcept
{
    return std::this_thread::get_id() == m_state->owner && m_state->isDebug;
}

bool PlayClient::is_debugger_attached() const noexcept
{
    BOOL attached = FALSE;
    return std::this_thread::get_id() == m_state->owner && m_state->process.value &&
           CheckRemoteDebuggerPresent(m_state->process.value, &attached) && attached;
}
std::vector<PlayPropertyAck> PlayClient::take_acknowledgements()
{
    return std::exchange(m_state->acks, {});
}

struct PlayServer::State final
{
    Endpoint endpoint;
    std::thread::id owner = std::this_thread::get_id();
    bool hasStart = false;
    bool isDebug = false;
    bool hasDebugWaiting = false;
    bool hasDebugContinued = false;
};

PlayServer::PlayServer(ConstructionKey, std::unique_ptr<State> a_state) : m_state(std::move(a_state))
{
}
PlayServer::~PlayServer() = default;

Result<std::unique_ptr<PlayServer>> PlayServer::connect(std::string_view a_pipeName, std::uint64_t a_session,
                                                        bool a_isDebug)
{
    if (!a_pipeName.starts_with("\\\\.\\pipe\\CueEngine.Play.") || a_pipeName.size() > 200 || !a_session)
        return Result<std::unique_ptr<PlayServer>>::failure({ErrorCategory::InvalidArgument, "Play.pipe_name"});
    auto name = utf8_to_utf16(a_pipeName);
    if (!name.has_value())
        return Result<std::unique_ptr<PlayServer>>::failure(*name.try_error());
    try
    {
        auto state = std::make_unique<State>();
        state->isDebug = a_isDebug;
        state->endpoint.pipe = Handle(CreateFileW(name.try_value()->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
        if (state->endpoint.pipe.value == INVALID_HANDLE_VALUE)
            return Result<std::unique_ptr<PlayServer>>::failure(transport_error("Play.open_pipe"));
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(state->endpoint.pipe.value, &mode, nullptr, nullptr))
            return Result<std::unique_ptr<PlayServer>>::failure(transport_error("Play.pipe_mode"));
        state->endpoint.isConnected = true;
        state->endpoint.session = a_session;
        return Result<std::unique_ptr<PlayServer>>::success(
            std::make_unique<PlayServer>(ConstructionKey{}, std::move(state)));
    }
    catch (...)
    {
        return Result<std::unique_ptr<PlayServer>>::failure({ErrorCategory::PlatformFailure, "Play.connect.exception"});
    }
}

Result<std::vector<PlayCommand>> PlayServer::poll()
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<std::vector<PlayCommand>>::failure({ErrorCategory::WrongThread, "PlayServer.poll"});
    try
    {
        auto packets = m_state->endpoint.pump();
        if (!packets.has_value())
            return Result<std::vector<PlayCommand>>::failure(*packets.try_error());
        if (m_state->endpoint.isClosed && packets.try_value()->empty() && m_state->endpoint.incoming.empty())
            return Result<std::vector<PlayCommand>>::failure(
                {ErrorCategory::PlatformFailure, "Play.parent_closed", ERROR_BROKEN_PIPE});
        std::vector<PlayCommand> commands;
        for (const auto &packet : *packets.try_value())
        {
            PlayCommand command;
            command.requestId = packet.request;
            Reader reader{packet.payload.data(), packet.payload.size()};
            if (packet.kind == Message::Start && !m_state->hasStart && packet.request == 0)
            {
                command.kind = PlayCommand::Kind::Start;
                command.sceneText = reader.text(Scene::k_maxFileBytes);
                m_state->hasStart = true;
            }
            else if (packet.kind == Message::Stop && packet.request == 0)
                command.kind = PlayCommand::Kind::Stop;
            else if (packet.kind == Message::ContinueDebug && packet.request == 0 && m_state->isDebug &&
                     m_state->hasDebugWaiting && m_state->hasStart && !m_state->hasDebugContinued)
            {
                if (!IsDebuggerPresent())
                    return Result<std::vector<PlayCommand>>::failure(
                        {ErrorCategory::InvalidState, "Play.debugger_not_attached"});
                command.kind = PlayCommand::Kind::ContinueDebug;
                m_state->hasDebugContinued = true;
            }
            else if (packet.kind == Message::SetProperty && m_state->hasStart && packet.request != 0)
            {
                command.kind = PlayCommand::Kind::SetProperty;
                command.objectId = reader.text(128);
                command.scriptInstanceId = reader.text(128);
                command.value = reader.property();
                if (!is_valid_uuid_text(command.objectId) || !is_valid_uuid_text(command.scriptInstanceId))
                    reader.valid = false;
            }
            else
                return Result<std::vector<PlayCommand>>::failure({ErrorCategory::InvalidArgument, "Play.command"});
            if (!reader.complete())
                return Result<std::vector<PlayCommand>>::failure(
                    {ErrorCategory::InvalidArgument, "Play.command_payload"});
            commands.push_back(std::move(command));
        }
        return Result<std::vector<PlayCommand>>::success(std::move(commands));
    }
    catch (...)
    {
        return Result<std::vector<PlayCommand>>::failure({ErrorCategory::PlatformFailure, "PlayServer.poll.exception"});
    }
}

Result<void> PlayServer::send_types(const std::vector<ScriptTypeDescriptor> &a_types)
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "PlayServer.send_types"});
    if (a_types.size() > 256)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "Play.types_limit"});
    if (m_state->isDebug && !m_state->hasDebugContinued)
        return Result<void>::failure({ErrorCategory::InvalidState, "Play.debug_not_continued"});
    Writer payload;
    payload.u32(static_cast<std::uint32_t>(a_types.size()));
    for (const auto &type : a_types)
    {
        if (type.properties.size() > 64)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Play.properties_limit"});
        payload.text(type.typeId);
        payload.text(type.displayName);
        payload.u32(type.schemaVersion);
        payload.u32(static_cast<std::uint32_t>(type.properties.size()));
        for (const auto &property : type.properties)
        {
            payload.property({property.propertyId, property.kind, property.defaultNumber, property.defaultBoolean});
            payload.text(property.displayName);
        }
    }
    return m_state->endpoint.queue(Message::Types, 0, payload);
}

Result<void> PlayServer::send_ready()
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "PlayServer.send_ready"});
    if (m_state->isDebug && !m_state->hasDebugContinued)
        return Result<void>::failure({ErrorCategory::InvalidState, "Play.debug_not_continued"});
    return m_state->endpoint.queue(Message::Ready, 0, {});
}

Result<void> PlayServer::send_debug_waiting()
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "PlayServer.send_debug_waiting"});
    if (!m_state->isDebug || m_state->hasDebugWaiting)
        return Result<void>::failure({ErrorCategory::InvalidState, "Play.debug_not_waiting"});
    auto queued = m_state->endpoint.queue(Message::DebugWaiting, 0, {});
    if (queued.has_value())
        m_state->hasDebugWaiting = true;
    return queued;
}

Result<void> PlayServer::send_ack(const PlayPropertyAck &a_ack)
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "PlayServer.send_ack"});
    Writer payload;
    payload.u32(a_ack.isAccepted ? 1 : 0);
    payload.text(a_ack.objectId);
    payload.text(a_ack.scriptInstanceId);
    payload.property(a_ack.value);
    payload.text(a_ack.error);
    return m_state->endpoint.queue(Message::Ack, a_ack.requestId, payload);
}

Result<void> PlayServer::send_error(std::string_view a_message)
{
    if (std::this_thread::get_id() != m_state->owner)
        return Result<void>::failure({ErrorCategory::WrongThread, "PlayServer.send_error"});
    Writer payload;
    payload.text(a_message.substr(0, 4096));
    return m_state->endpoint.queue(Message::Error, 0, payload);
}

bool PlayServer::has_pending_output() const noexcept
{
    return m_state->endpoint.sent < m_state->endpoint.outgoing.size();
}
} // namespace cue
