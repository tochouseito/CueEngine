#include <EditorHost/ImGuiManager.h>

#include <Foundation/TimingTrace.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <FrameGraph/FrameGraph.h>
#include <Platform/Diagnostics.h>
#include <Platform/Windows/WindowsFileSystem.h>
#include <Platform/Windows/WindowsPlatform.h>

#include "DX12ImGuiBackend.h"
#include "ImGuiSynchronization.h"

// 公式 Header の指示に従い、Win32 型を公開 Header に漏らさず実装側で宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace cue
{
namespace
{
// 所有権を持たず、この Thread の同期 Render Scope だけを識別する
thread_local const ImGuiManager *g_recordingManager = nullptr;

/// @brief API 呼出中だけ対象 Context を Current にし、呼出側の Context を復元する
class ScopedContext final
{
public:
    /// @brief 現在の Context を非所有で退避する
  explicit ScopedContext(ImGuiContext *a_context)
      : m_lock(imgui_context_mutex()), m_previous(ImGui::GetCurrentContext())
  {
      ImGui::SetCurrentContext(a_context);
  }

    /// @brief 借用期間の終わりに元の Context を Current に戻す
    ~ScopedContext()
    {
        ImGui::SetCurrentContext(m_previous);
    }

private:
  std::unique_lock<std::recursive_mutex> m_lock;
  ImGuiContext *m_previous = nullptr;
};

/// @brief 公式 Allocator と対になる方法で複製 DrawList を解放する
struct DrawListDeleter final
{
    /// @brief ImGui の Allocator 設定を維持して所有する複製を回収する
    void operator()(ImDrawList *a_list) const noexcept
    {
        IM_DELETE(a_list);
    }
};

/// @brief 次回 NewFrame が変更する配列を所有し、Texture ID と Viewport Metadata を固定する
struct DrawSnapshot final
{
    /// @brief 公式 Viewport の Owner を解放せず、借用 Metadata の終了を明示する
    ~DrawSnapshot()
    {
        viewport.RendererUserData = nullptr;
        viewport.PlatformUserData = nullptr;
    }

    ImDrawData draw;
    ImGuiViewport viewport;
    std::vector<std::unique_ptr<ImDrawList, DrawListDeleter>> lists;
    // Texture と Backend の所有者は Manager。QueueUserData により公式 Core の早期破棄を防ぐ
    std::vector<ImTextureData *> textures;
    std::uint64_t frame = 0;
    bool wasRecorded = false;
};
} // namespace

class ImGuiManager::State final
{
public:
    std::unique_ptr<IFileSystem> ownedFiles;
    IFileSystem *files = nullptr;
    ImGuiContext* context = nullptr;
    Window* window = nullptr;
    WindowsMessageHandlerToken handler;
    ImGuiManagerConfig config;
    ImGuiFrameInfo info;
    ImGuiContext* previousFrameContext = nullptr;
    ImGuiErrorRecoveryState recovery;
    std::unique_ptr<DX12ImGuiBackend> renderer;
    std::uint64_t lastRecordedFrame = 0;
    bool isWin32Initialized = false;
    bool isReady = false;
    bool isFrameOpen = false;
    bool isBuilding = false;
    bool isCpuAtlasBuilt = false;
    std::unique_lock<std::recursive_mutex> frameLock;
    std::condition_variable_any released;
    std::vector<std::unique_ptr<DrawSnapshot>> snapshots;
    // Render が返した枠は配列の容量を保持し、次の CPU 複製で再利用する
    std::vector<std::unique_ptr<DrawSnapshot>> cachedSnapshots;
    ImGuiTransferInfo transfer;
    DrawSnapshot *activeSnapshot = nullptr;
    std::uint64_t lastPublishedUi = 0;
    std::uint32_t frameCount = 0;
    TimingSamples uiBuildTimes;
    TimingSamples snapshotCopyTimes;
    TimingSamples contextWaitTimes;
    std::chrono::steady_clock::time_point frameStarted;
    // 一つの Native Message が生成した最後の ImGui Event が処理された Frame に帰属させる
    std::vector<std::pair<ImU32, InputEventTiming>> pendingInput;
    std::vector<InputEventTiming> frameInput;

    /// @brief 他の枠から借用中の Texture は残し、最後の CPU 参照だけ Queue Pin を解除する
    void release_snapshot(std::size_t a_slot)
    {
        auto snapshot = std::move(snapshots[a_slot]);
        if (!snapshot)
        {
            return;
        }
        for (auto *texture : snapshot->textures)
        {
            bool isReferenced = false;
            for (const auto &pending : snapshots)
            {
                isReferenced |= pending && std::find(pending->textures.begin(), pending->textures.end(), texture) !=
                                               pending->textures.end();
            }
            if (!isReferenced)
            {
                texture->QueueUserData = nullptr;
            }
        }
        snapshot->textures.clear();
        snapshot->draw.CmdLists.resize(0);
        snapshot->wasRecorded = false;
        cachedSnapshots[a_slot] = std::move(snapshot);
        --transfer.pendingFrames;
        released.notify_all();
    }
};

/// @brief 全 Context 操作を作成した Thread に固定する
ImGuiManager::ImGuiManager(CreateToken) noexcept : m_ownerId(std::this_thread::get_id())
{
}

/// @brief Window を借用して CPU UI 基盤だけを構築する
Result<std::unique_ptr<ImGuiManager>> ImGuiManager::create(Window& a_window, ImGuiManagerConfig a_config)
{
    using ManagerResult = Result<std::unique_ptr<ImGuiManager>>;
    if (!std::isfinite(a_config.fontSize) || a_config.fontSize <= 0.0f || a_config.fontSize > 256.0f ||
        a_config.settingsFile.find('\0') != std::string::npos)
    {
        return ManagerResult::failure({ErrorCategory::InvalidArgument, "ImGuiManager.config"});
    }
    auto handle = borrow_windows_window_handle(a_window);
    if (!handle.has_value())
    {
        return ManagerResult::failure(*handle.try_error());
    }
    try
    {
        auto result = std::make_unique<ImGuiManager>(CreateToken{});
        result->m_state = std::make_unique<State>();
        auto& state = *result->m_state;
        state.window = &a_window;
        state.config = std::move(a_config);
        state.files = state.config.fileSystem;
        if (!state.files)
        {
            auto files = create_windows_file_system();
            if (!files.has_value())
            {
                return ManagerResult::failure(*files.try_error());
            }
            state.ownedFiles = files.take_value();
            state.files = state.ownedFiles.get();
        }
        // Context の排他を取る前に File I/O を完了し、別 Context の描画を待たせない
        std::vector<std::byte> settings;
        if (!state.config.settingsFile.empty())
        {
            auto path = Path::create(state.config.settingsFile);
            if (!path.has_value())
            {
                return ManagerResult::failure(*path.try_error());
            }
            auto exists = state.files->exists(*path.try_value());
            if (!exists.has_value())
            {
                return ManagerResult::failure(*exists.try_error());
            }
            if (*exists.try_value())
            {
                auto loaded = state.files->read_all(*path.try_value(), 4 * 1024 * 1024);
                if (!loaded.has_value())
                {
                    return ManagerResult::failure(*loaded.try_error());
                }
                settings = loaded.take_value();
            }
        }
        TimingTraceScope traceLock("ImGui.Lock");
        std::lock_guard lock(imgui_context_mutex());
        traceLock.finish();

        // CreateContext が Current を変更する場合も、生成処理の外へ漏らさない
        IMGUI_CHECKVERSION();
        auto* previous = ImGui::GetCurrentContext();
        state.context = ImGui::CreateContext();
        ImGui::SetCurrentContext(previous);
        if (!state.context)
        {
            return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGui.CreateContext"});
        }
        ScopedContext current(state.context);
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (state.config.isDockingEnabled)
        {
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        }
        // Multi-Viewport は Window / GPU 資源の寿命が別途必要なため初期構成では有効にしない
        // 自動 File I/O を使わず、保存失敗を Result で返せるよう保存先を Manager が所有する
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().FrameRounding = 4.0f;
        ImFontConfig font;
        font.SizePixels = state.config.fontSize;
        if (!io.Fonts->AddFontDefault(&font))
        {
            return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.fonts"});
        }

        // 初回は Layout が存在しなくてもよい。既存 File の読込み失敗は初期化失敗にする
        if (!state.config.settingsFile.empty())
        {
            if (!settings.empty())
            {
                ImGui::LoadIniSettingsFromMemory(reinterpret_cast<const char *>(settings.data()), settings.size());
            }
        }

        // 接続枠を先に確保し、二重生成失敗が既存 Backend の Window Property を変更しないようにする
        State* borrowed = &state;
        auto registered = register_windows_message_handler(
            a_window,
            [borrowed](const WindowsMessage &a_message)
            {
                TimingTraceScope traceLock("ImGui.Lock");
                std::lock_guard lock(imgui_context_mutex());
                traceLock.finish();
                if (!borrowed->isWin32Initialized)
                {
                    return WindowsMessageResult{};
                }
                // 他の Context が Current の場合も、この Window の Context へ入力を配送する
                ScopedContext active(borrowed->context);
                const auto nextInput = borrowed->context->InputEventsNextEventId;
                const auto value = ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(a_message.window),
                                                                a_message.message, a_message.wParam, a_message.lParam);
                if (a_message.inputTiming.eventId != 0 && borrowed->context->InputEventsNextEventId != nextInput)
                {
                    borrowed->pendingInput.emplace_back(borrowed->context->InputEventsNextEventId - 1,
                                                        a_message.inputTiming);
                }
                // Capture Flag は上位の Gameplay 入力に使う。Win32 の必須処理は Platform が維持する
                return WindowsMessageResult{value != 0, static_cast<std::intptr_t>(value)};
            });
        if (!registered.has_value())
        {
            return ManagerResult::failure(*registered.try_error());
        }
        state.handler = registered.take_value();
        if (!ImGui_ImplWin32_Init(handle.take_value()))
        {
            return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGui_ImplWin32_Init"});
        }
        state.isWin32Initialized = true;
        state.isReady = true;
        return ManagerResult::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return ManagerResult::failure({ErrorCategory::PlatformFailure, "ImGuiManager.create.allocation"});
    }
}

/// @brief Context より先に入力接続を解除し、解除できない破棄を続けない
ImGuiManager::~ImGuiManager()
{
    auto result = shutdown();
    if (!result.has_value())
    {
        report_log_error("ImGuiManager cleanup", *result.try_error(), LogLevel::Error);
        if (m_state && m_state->context)
        {
            std::terminate();
        }
    }
}

/// @brief ImGui を触る前に Thread と Lifecycle を検証する
Result<void> ImGuiManager::validate(const char* a_operation) const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, a_operation});
    }
    if (!m_state || !m_state->isReady)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, a_operation});
    }
    return Result<void>::success();
}

/// @brief CPU 基盤の生成後に Renderer を接続し、途中失敗でも Context を維持する
Result<void> ImGuiManager::initialize_renderer(IBackend &a_backend, std::uint32_t a_frameCount)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.initialize_renderer"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.initialize_renderer");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->renderer || m_state->isFrameOpen || m_state->isCpuAtlasBuilt)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.initialize_renderer"});
    }
    auto created = DX12ImGuiBackend::create(a_backend, *m_state->context, a_frameCount,
                                            m_state->config.rendererDescriptorCapacity);
    if (!created.has_value())
    {
        return Result<void>::failure(*created.try_error());
    }
    m_state->renderer = created.take_value();
    return Result<void>::success();
}

/// @brief Renderer 接続後にだけ固定数の転送枠を使用可能にする
Result<void> ImGuiManager::enable_frame_transfer(std::uint32_t a_cpuFrameCount)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.enable_frame_transfer"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.enable_frame_transfer");
    if (!valid.has_value())
    {
        return valid;
    }
    if (!m_state->renderer || m_state->frameCount || m_state->info.frames || m_state->isFrameOpen)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.enable_frame_transfer"});
    }
    const auto frameCount = a_cpuFrameCount ? a_cpuFrameCount : m_state->renderer->info().frameCount;
    if (frameCount > 8)
        return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiManager.cpu_capacity"});
    try
    {
        // GPU Ring の容量を流用せず、CPU Snapshot の所有枠を確保する
        m_state->snapshots.resize(frameCount);
        m_state->cachedSnapshots.resize(frameCount);
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.cpu_allocation"});
    }
    m_state->frameCount = frameCount;
    return Result<void>::success();
}

/// @brief Snapshot を完全排出した後、公式 Backend の Ring と CPU 所有枠を変更する
Result<void> ImGuiManager::reconfigure_renderer(IBackend &a_backend, std::uint32_t a_cpuFrames,
                                               std::uint32_t a_gpuFrames)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.reconfigure"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.reconfigure");
    if (!valid.has_value())
    {
        return valid;
    }
    if (!m_state->renderer || m_state->isFrameOpen || m_state->activeSnapshot ||
        m_state->transfer.pendingFrames || a_cpuFrames == 0 || a_cpuFrames > 8 ||
        a_gpuFrames == 0 || a_gpuFrames > 8)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.reconfigure.pending"});
    }
    try
    {
        m_state->snapshots.resize(a_cpuFrames);
        m_state->cachedSnapshots.resize(a_cpuFrames);
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.reconfigure.allocation"});
    }
    if (m_state->renderer->info().frameCount != a_gpuFrames)
    {
        auto stopped = m_state->renderer->shutdown();
        if (!stopped.has_value())
        {
            return stopped;
        }
        m_state->renderer.reset();
        // 公式 DX12 Shutdown は MainViewport の PlatformUserData も破棄する
        // Context と入力 Handler は維持し、Win32 Backend を再接続して DPI 参照を復元する
        ScopedContext current(m_state->context);
        ImGui_ImplWin32_Shutdown();
        m_state->isWin32Initialized = false;
        auto handle = borrow_windows_window_handle(*m_state->window);
        if (!handle.has_value())
        {
            return Result<void>::failure(*handle.try_error());
        }
        if (!ImGui_ImplWin32_Init(*handle.try_value()))
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.reconfigure.win32"});
        }
        m_state->isWin32Initialized = true;
        auto created = DX12ImGuiBackend::create(a_backend, *m_state->context, a_gpuFrames,
                                                m_state->config.rendererDescriptorCapacity);
        if (!created.has_value())
        {
            return Result<void>::failure(*created.try_error());
        }
        m_state->renderer = created.take_value();
    }
    m_state->frameCount = a_cpuFrames;
    // Worker 再生成で Thread ID は変わる。公開 / 消費の論理 Frame ID は維持する
    m_state->transfer.renderThreadId = {};
    return Result<void>::success();
}

/// @brief Texture の GPU 更新後に CPU 配列を複製し、原本への可変参照を残さない
Result<void> ImGuiManager::publish_frame(std::uint64_t a_frame, std::stop_token a_token)
{
    TimingTraceScope traceScope("ImGui.Snapshot");
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.publish_frame"});
    }
    const auto lockStarted = std::chrono::steady_clock::now();
    TimingTraceScope traceLock("ImGui.Lock");
    std::unique_lock lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.publish_frame");
    if (!valid.has_value())
    {
        return valid;
    }
    auto &state = *m_state;
    state.contextWaitTimes.add(std::chrono::steady_clock::now() - lockStarted);
    if (!state.frameCount || state.isFrameOpen || state.lastPublishedUi == state.info.frames ||
        a_frame != state.transfer.publishedFrames || state.snapshots[a_frame % state.frameCount])
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.publish_frame"});
    }
    // RefCount は Font Atlas の Context 数であり、転送中 Frame の参照数には流用しない
    // wait は唯一の Context Lock を解放する。Worker の失敗 / 停止でも Main を解除する
    const bool canUpdate = state.released.wait(
        lock, a_token,
        [&state]()
        {
            ScopedContext current(state.context);
            const auto *draw = ImGui::GetDrawData();
            if (draw && draw->Textures)
            {
                for (const auto *texture : *draw->Textures)
                {
                    if (texture && texture->Status == ImTextureStatus_WantUpdates && texture->QueueUserData)
                    {
                        return false;
                    }
                }
            }
            return true;
        });
    if (!canUpdate || a_token.stop_requested())
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.publish.cancelled"});
    }
    ScopedContext current(state.context);
    auto *source = ImGui::GetDrawData();
    if (!source || !source->Valid || !source->OwnerViewport)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.publish.draw_data"});
    }
    // CloneOutput が複製しない Callback UserData を Worker に持ち出さない
    const auto &platform = ImGui::GetPlatformIO();
    for (const auto *list : source->CmdLists)
    {
        for (const auto &command : list->CmdBuffer)
        {
            if (!is_imgui_draw_callback_supported(command.UserCallback, platform))
            {
                return Result<void>::failure({ErrorCategory::InvalidArgument, "ImGuiManager.publish.callback"});
            }
        }
    }
    auto prepared = state.renderer->prepare(*source);
    if (!prepared.has_value())
    {
        return prepared;
    }
    try
    {
        const auto copyStarted = std::chrono::steady_clock::now();
        const auto slot = static_cast<std::size_t>(a_frame % state.frameCount);
        auto snapshot = std::move(state.cachedSnapshots[slot]);
        if (!snapshot)
        {
            snapshot = std::make_unique<DrawSnapshot>();
            ++state.transfer.snapshotAllocations;
        }
        snapshot->frame = a_frame;
        snapshot->viewport = *source->OwnerViewport;
        snapshot->draw.Valid = true;
        snapshot->draw.FrameCount = source->FrameCount;
        snapshot->draw.TotalIdxCount = source->TotalIdxCount;
        snapshot->draw.TotalVtxCount = source->TotalVtxCount;
        snapshot->draw.DisplayPos = source->DisplayPos;
        snapshot->draw.DisplaySize = source->DisplaySize;
        snapshot->draw.FramebufferScale = source->FramebufferScale;
        snapshot->draw.OwnerViewport = &snapshot->viewport;
        snapshot->lists.reserve(source->CmdLists.Size);
        std::size_t listIndex = 0;
        for (const auto *list : source->CmdLists)
        {
            if (listIndex == snapshot->lists.size())
            {
                // 公式 CloneOutput と同じ非所有 SharedData なしの出力専用 DrawList を保つ
                snapshot->lists.emplace_back(IM_NEW(ImDrawList(nullptr)));
                ++state.transfer.snapshotAllocations;
            }
            auto *clone = snapshot->lists[listIndex++].get();
            state.transfer.snapshotAllocations += clone->CmdBuffer.Capacity < list->CmdBuffer.Size ? 1 : 0;
            state.transfer.snapshotAllocations += clone->IdxBuffer.Capacity < list->IdxBuffer.Size ? 1 : 0;
            state.transfer.snapshotAllocations += clone->VtxBuffer.Capacity < list->VtxBuffer.Size ? 1 : 0;
            state.transfer.copiedBytes +=
                list->CmdBuffer.size_in_bytes() + list->IdxBuffer.size_in_bytes() + list->VtxBuffer.size_in_bytes();
            // ImVector の operator= は clear() で容量を捨てるため、resize と所有配列への Copy を使う
            clone->CmdBuffer.resize(list->CmdBuffer.Size);
            clone->IdxBuffer.resize(list->IdxBuffer.Size);
            clone->VtxBuffer.resize(list->VtxBuffer.Size);
            if (list->CmdBuffer.Size)
            {
                std::memcpy(clone->CmdBuffer.Data, list->CmdBuffer.Data, list->CmdBuffer.size_in_bytes());
            }
            if (list->IdxBuffer.Size)
            {
                std::memcpy(clone->IdxBuffer.Data, list->IdxBuffer.Data, list->IdxBuffer.size_in_bytes());
            }
            if (list->VtxBuffer.Size)
            {
                std::memcpy(clone->VtxBuffer.Data, list->VtxBuffer.Data, list->VtxBuffer.size_in_bytes());
            }
            clone->Flags = list->Flags;
            for (auto &command : clone->CmdBuffer)
            {
                auto *texture = command.TexRef._TexData;
                if (!texture && source->Textures && command.ElemCount && !command.UserCallback)
                {
                    for (auto *candidate : *source->Textures)
                    {
                        if (candidate && candidate->GetTexID() == command.GetTexID())
                        {
                            texture = candidate;
                            break;
                        }
                    }
                }
                if (texture && command.ElemCount && !command.UserCallback)
                {
                    if (texture->QueueUserData && texture->QueueUserData != &state)
                    {
                        return Result<void>::failure(
                            {ErrorCategory::InvalidState, "ImGuiManager.publish.texture_owner"});
                    }
                    if (std::find(snapshot->textures.begin(), snapshot->textures.end(), texture) ==
                        snapshot->textures.end())
                    {
                        snapshot->textures.push_back(texture);
                    }
                }
                // Atlas が次の NewFrame で差し替わっても、提出済み Frame は元の Native ID を参照する
                command.TexRef = ImTextureRef(command.GetTexID());
            }
            snapshot->draw.CmdLists.push_back(clone);
        }
        snapshot->draw.CmdListsCount = snapshot->draw.CmdLists.Size;
        for (auto *texture : snapshot->textures)
        {
            texture->QueueUserData = &state;
        }
        state.snapshots[a_frame % state.frameCount] = std::move(snapshot);
        state.lastPublishedUi = state.info.frames;
        ++state.transfer.publishedFrames;
        ++state.transfer.pendingFrames;
        state.snapshotCopyTimes.add(std::chrono::steady_clock::now() - copyStarted);
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.publish.allocation"});
    }
}

/// @brief Owner Thread の UI 構築前に、次の GPU Ring の再利用可否を照会する
Result<bool> ImGuiManager::is_renderer_ready() const
{
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.is_renderer_ready");
    if (!valid.has_value() || !m_state->renderer)
    {
        return Result<bool>::failure(valid.has_value()
                                         ? Error{ErrorCategory::InvalidState, "ImGuiManager.is_renderer_ready.renderer"}
                                         : *valid.try_error());
    }
    return m_state->renderer->is_frame_ready();
}

/// @brief Snapshot の借用だけを排他し、通常 Graph 記録中に Main の Context 操作を止めない
Result<void> ImGuiManager::render_frame(std::uint64_t a_frame, std::stop_token a_token, const FrameCallback &a_record,
                                        bool *a_wasRecorded)
{
    TimingTraceScope traceScope("ImGui.RenderScope");
    if (a_wasRecorded)
    {
        *a_wasRecorded = false;
    }
    const auto lockStarted = std::chrono::steady_clock::now();
    TimingTraceScope traceLock("ImGui.Lock");
    std::unique_lock lock(imgui_context_mutex());
    traceLock.finish();
    if (!m_state || !m_state->isReady || !m_state->frameCount || m_state->activeSnapshot || !a_record)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.render_frame"});
    }
    auto &state = *m_state;
    state.contextWaitTimes.add(std::chrono::steady_clock::now() - lockStarted);
    const auto thread = std::this_thread::get_id();
    if (state.transfer.renderThreadId != std::thread::id{} && state.transfer.renderThreadId != thread)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.render_frame"});
    }
    const auto slot = a_frame % state.frameCount;
    if (!state.snapshots[slot] || state.snapshots[slot]->frame != a_frame || a_frame != state.transfer.consumedFrames)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.render_frame.order"});
    }
    state.transfer.renderThreadId = thread;
    state.activeSnapshot = state.snapshots[slot].get();
    const auto *previous = g_recordingManager;
    g_recordingManager = this;
    // Snapshot は枠に残して Main の再利用と Texture 更新を禁止する。通常 Pass は Context を借用しない
    lock.unlock();
    Result<void> result = Result<void>::success();
    try
    {
        // 取消時も Host の Graph Scope を呼び、下位の Skip / Rollback 規約に従う
        result = a_record(a_frame, a_token);
    }
    catch (...)
    {
        result = Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.render.callback"});
    }
    TimingTraceScope traceReacquire("ImGui.Reacquire");
    lock.lock();
    traceReacquire.finish();
    auto finished = state.renderer->finish_submission();
    if (result.has_value() && !finished.has_value())
    {
        result = std::move(finished);
    }
    state.transfer.discardedFrames += state.activeSnapshot->wasRecorded ? 0 : 1;
    if (a_wasRecorded)
    {
        *a_wasRecorded = result.has_value() && state.activeSnapshot->wasRecorded;
    }
    g_recordingManager = previous;
    state.activeSnapshot = nullptr;
    state.release_snapshot(slot);
    ++state.transfer.consumedFrames;
    return result;
}

/// @brief 可変な計測配列を公開せず、Owner へ同じ時点の集計を返す
Result<ImGuiTimingInfo> ImGuiManager::timing_info() const
{
    TimingTraceScope traceScope("ImGui.Statistics");
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiTimingInfo>::failure({ErrorCategory::WrongThread, "ImGuiManager.timing_info"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.timing_info");
    if (!valid.has_value())
    {
        return Result<ImGuiTimingInfo>::failure(*valid.try_error());
    }
    return Result<ImGuiTimingInfo>::success(
        {m_state->uiBuildTimes.statistics(), m_state->snapshotCopyTimes.statistics(),
         m_state->contextWaitTimes.statistics(),
         m_state->renderer ? m_state->renderer->info().gpuWait : TimingStatistics{}});
}

/// @brief 内部の排他状態を公開せず Owner へ進行数を返す
Result<ImGuiTransferInfo> ImGuiManager::transfer_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiTransferInfo>::failure({ErrorCategory::WrongThread, "ImGuiManager.transfer_info"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.transfer_info");
    return valid.has_value() ? Result<ImGuiTransferInfo>::success(m_state->transfer)
                             : Result<ImGuiTransferInfo>::failure(*valid.try_error());
}

/// @brief Owner の直接記録または有効な Render Scope 内から、一度だけ描画を記録する
Result<void> ImGuiManager::record_draw_data(FrameGraphContext &a_context)
{
    TimingTraceScope traceScope("ImGui.Record");
    if (std::this_thread::get_id() != m_ownerId && g_recordingManager != this)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.record_draw_data"});
    }
    const auto lockStarted = std::chrono::steady_clock::now();
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    if (m_state)
    {
        m_state->contextWaitTimes.add(std::chrono::steady_clock::now() - lockStarted);
    }
    if (m_state && m_state->activeSnapshot && m_state->transfer.renderThreadId == std::this_thread::get_id())
    {
        auto &snapshot = *m_state->activeSnapshot;
        // Snapshot は論理 Frame ID、Context は GPU 枠の番号であり一致を要求しない
        if (snapshot.wasRecorded)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.record.duplicate"});
        }
        auto result = m_state->renderer->record(snapshot.draw, a_context);
        snapshot.wasRecorded = result.has_value();
        return result;
    }
    auto valid = validate("ImGuiManager.record_draw_data");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->frameCount || !m_state->renderer || m_state->isFrameOpen || m_state->info.frames == 0 ||
        m_state->lastRecordedFrame == m_state->info.frames)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.record_draw_data"});
    }
    ScopedContext current(m_state->context);
    auto *draw = ImGui::GetDrawData();
    if (!draw || !draw->Valid)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.draw_data"});
    }
    auto recorded = m_state->renderer->record(*draw, a_context);
    if (recorded.has_value())
    {
        m_state->lastRecordedFrame = m_state->info.frames;
    }
    return recorded;
}

/// @brief 生成済み GPU Backend の所有状態だけを返す
Result<ImGuiRendererInfo> ImGuiManager::renderer_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiRendererInfo>::failure({ErrorCategory::WrongThread, "ImGuiManager.renderer_info"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.renderer_info");
    if (!valid.has_value())
    {
        return Result<ImGuiRendererInfo>::failure(*valid.try_error());
    }
    return m_state->renderer
               ? Result<ImGuiRendererInfo>::success(m_state->renderer->info())
               : Result<ImGuiRendererInfo>::failure({ErrorCategory::InvalidState, "ImGuiManager.renderer_info"});
}

/// @brief Message Pump 後の採用 Frame で Platform 入力を取り込み、UI の構築を開始する
Result<void> ImGuiManager::begin_frame()
{
    TimingTraceScope traceScope("ImGui.Begin");
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.begin_frame"});
    }
    const auto lockStarted = std::chrono::steady_clock::now();
    TimingTraceScope traceLock("ImGui.Lock");
    std::unique_lock lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.begin_frame");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->isFrameOpen || m_state->isBuilding)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.begin_frame"});
    }
    m_state->contextWaitTimes.add(std::chrono::steady_clock::now() - lockStarted);
    m_state->frameStarted = std::chrono::steady_clock::now();
    if (m_state->renderer)
    {
        auto renderer = m_state->renderer->new_frame();
        if (!renderer.has_value())
        {
            return renderer;
        }
    }
    m_state->previousFrameContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_state->context);
    // GPU 接続前に Legacy Atlas を焼かず、Renderer 未接続の CPU 利用だけ初回に生成する
    // 動的 Texture 対応 Backend は NewFrame 内の公式 Atlas 更新へ任せる
    if (!m_state->renderer && !m_state->isCpuAtlasBuilt)
    {
        if (!ImGui::GetIO().Fonts->Build())
        {
            ImGui::SetCurrentContext(m_state->previousFrameContext);
            m_state->previousFrameContext = nullptr;
            return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.fonts"});
        }
        m_state->isCpuAtlasBuilt = true;
    }
    {
        TimingTraceScope tracePlatform("ImGui.PlatformNewFrame");
        ImGui_ImplWin32_NewFrame();
    }
    try
    {
        m_state->frameInput.reserve(m_state->pendingInput.size());
    }
    catch (const std::bad_alloc &)
    {
        ImGui::SetCurrentContext(m_state->previousFrameContext);
        m_state->previousFrameContext = nullptr;
        return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.input.allocation"});
    }
    {
        TimingTraceScope traceNewFrame("ImGui.NewFrame");
        ImGui::NewFrame();
    }
    m_state->frameInput.clear();
    for (auto input = m_state->pendingInput.begin(); input != m_state->pendingInput.end();)
    {
        const auto contains = [id = input->first](const auto &a_events)
        {
            return std::any_of(a_events.begin(), a_events.end(),
                               [id](const auto &a_event) { return a_event.EventId == id; });
        };
        if (contains(m_state->context->InputEventsTrail))
        {
            m_state->frameInput.push_back(input->second);
            input = m_state->pendingInput.erase(input);
        }
        else if (!contains(m_state->context->InputEventsQueue))
        {
            // Focus 変更等で公式 Core が破棄した入力を、別の Frame へ誤って付け替えない
            input = m_state->pendingInput.erase(input);
        }
        else
        {
            ++input;
        }
    }
    // 固定 Version が提供する回復 API で Callback 前の Begin / Style / Table Stack を保存する
    ImGui::ErrorRecoveryStoreState(&m_state->recovery);
    m_state->isFrameOpen = true;
    // begin / end を分けて呼ぶ利用でも、その間の ImGui API を同じ排他期間に含める
    m_state->frameLock = std::move(lock);
    return Result<void>::success();
}

/// @brief NewFrame の処理済み入力を移し、配列容量は次の Frame に使う
Result<void> ImGuiManager::take_input_events(std::vector<InputEventTiming> &a_events)
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.take_input_events"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.take_input_events");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->isFrameOpen || m_state->isBuilding)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.take_input_events.frame"});
    }
    a_events.clear();
    a_events.swap(m_state->frameInput);
    return Result<void>::success();
}

/// @brief Renderer に依存せず Draw Data を確定し、呼出元の Context を復元する
Result<void> ImGuiManager::end_frame()
{
    TimingTraceScope traceScope("ImGui.End");
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.end_frame"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::unique_lock lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.end_frame");
    if (!valid.has_value())
    {
        return valid;
    }
    if (!m_state->isFrameOpen || m_state->isBuilding || ImGui::GetCurrentContext() != m_state->context)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.end_frame"});
    }
    TimingTraceScope traceCpuRender("ImGui.CpuRender");
    ImGui::Render();
    traceCpuRender.finish();
    const auto* draw = ImGui::GetDrawData();
    const auto& io = ImGui::GetIO();
    ++m_state->info.frames;
    m_state->info.vertexCount = static_cast<std::uint32_t>(draw->TotalVtxCount);
    m_state->info.indexCount = static_cast<std::uint32_t>(draw->TotalIdxCount);
    m_state->info.wantsMouse = io.WantCaptureMouse;
    m_state->info.wantsKeyboard = io.WantCaptureKeyboard;
    m_state->info.wantsTextInput = io.WantTextInput;
    m_state->isFrameOpen = false;
    ImGui::SetCurrentContext(m_state->previousFrameContext);
    m_state->previousFrameContext = nullptr;
    const bool wantsSave = io.WantSaveIniSettings;
    m_state->uiBuildTimes.add(std::chrono::steady_clock::now() - m_state->frameStarted);
    m_state->frameLock.unlock();
    lock.unlock();
    return wantsSave ? save_settings() : Result<void>::success();
}

/// @brief UI Callback をこの Context の開いた Frame に限定して実行する
Result<void> ImGuiManager::build_frame(const editorUiCallback& a_buildUi)
{
    TimingTraceScope traceScope("ImGui.Build");
    auto started = begin_frame();
    if (!started.has_value())
    {
        return started;
    }
    m_state->isBuilding = true;
    Result<void> result = Result<void>::success();
    try
    {
        if (a_buildUi)
        {
            TimingTraceScope traceCallback("ImGui.Callback");
            result = a_buildUi();
        }
    }
    catch (...)
    {
        result = Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.ui_callback"});
    }
    m_state->isBuilding = false;
    if (result.has_value() && ImGui::GetCurrentContext() != m_state->context)
    {
        result = Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.ui_context"});
    }
    if (!result.has_value())
    {
        cancel_frame();
        return result;
    }
    return end_frame();
}

/// @brief 中断した Frame を確定せず回収し、開始前の Context を復元する
void ImGuiManager::cancel_frame() noexcept
{
    if (m_state->isFrameOpen)
    {
        ImGui::SetCurrentContext(m_state->context);
        // Callback 失敗は Result で通知し、残された Stack の回収中だけ Assert を抑制する
        // 通常 UI 構築時の Assert 設定と ImGui の回復診断は維持する
        auto& io = ImGui::GetIO();
        const bool wasAssertEnabled = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::ErrorRecoveryTryToRecoverState(&m_state->recovery);
        ImGui::EndFrame();
        io.ConfigErrorRecoveryEnableAssert = wasAssertEnabled;
        m_state->isFrameOpen = false;
        ImGui::SetCurrentContext(m_state->previousFrameContext);
        m_state->previousFrameContext = nullptr;
        m_state->frameLock.unlock();
    }
}

/// @brief Context の内部 Pointer を公開せず Frame と Capture の Snapshot を返す
Result<ImGuiFrameInfo> ImGuiManager::frame_info() const
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<ImGuiFrameInfo>::failure({ErrorCategory::WrongThread, "ImGuiManager.frame_info"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::lock_guard lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.frame_info");
    return valid.has_value() ? Result<ImGuiFrameInfo>::success(m_state->info)
                             : Result<ImGuiFrameInfo>::failure(*valid.try_error());
}

/// @brief Memory の Layout を一時 File へ書き、書込み成功後に保存先を置き換える
Result<void> ImGuiManager::save_settings()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.save_settings"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::unique_lock lock(imgui_context_mutex());
    traceLock.finish();
    auto valid = validate("ImGuiManager.save_settings");
    if (!valid.has_value())
    {
        return valid;
    }
    if (m_state->config.settingsFile.empty())
    {
        return Result<void>::success();
    }
    if (m_state->isFrameOpen || m_state->isBuilding)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.save_settings.frame"});
    }
    try
    {
        auto path = Path::create(m_state->config.settingsFile);
        if (!path.has_value())
        {
            return Result<void>::failure(*path.try_error());
        }
        std::string settings;
        {
            // ImGui の内部文字列は次の UI 更新で変更されるため、Lock 内で所有値へ複製する
            ScopedContext current(m_state->context);
            std::size_t size = 0;
            const auto *data = ImGui::SaveIniSettingsToMemory(&size);
            settings.assign(data, size);
        }
        // File I/O 中は Render の公式記録や別 Context の操作を待たせない
        lock.unlock();
        auto created = m_state->files->create_directories(path.try_value()->parent());
        if (!created.has_value())
        {
            return created;
        }
        auto saved = m_state->files->replace_file(*path.try_value(), std::as_bytes(std::span(settings)));
        if (!saved.has_value())
        {
            return Result<void>::failure(*saved.try_error());
        }
        lock.lock();
        ScopedContext current(m_state->context);
        ImGui::GetIO().WantSaveIniSettings = false;
        return Result<void>::success();
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "ImGuiManager.save_settings.allocation"});
    }
}

/// @brief Context を参照する Handler を先に失効させ、途中生成の Backend も回収する
Result<void> ImGuiManager::shutdown()
{
    if (std::this_thread::get_id() != m_ownerId)
    {
        return Result<void>::failure({ErrorCategory::WrongThread, "ImGuiManager.shutdown"});
    }
    TimingTraceScope traceLock("ImGui.Lock");
    std::unique_lock lock(imgui_context_mutex());
    traceLock.finish();
    if (!m_state || !m_state->context)
    {
        return Result<void>::success();
    }
    if (m_state->isBuilding || m_state->activeSnapshot)
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "ImGuiManager.shutdown"});
    }
    // 呼出側が Worker / Graph を停止した後に、未消費 Snapshot の Pin を先に回収する
    for (std::size_t slot = 0; slot < m_state->snapshots.size(); ++slot)
    {
        if (m_state->snapshots[slot])
        {
            ++m_state->transfer.discardedFrames;
            m_state->release_snapshot(slot);
        }
    }
    // GPU 完了の証明に失敗した場合は Context、Heap と下位 Backend の借用を保全する
    if (m_state->renderer)
    {
        auto stopped = m_state->renderer->shutdown();
        if (!stopped.has_value())
        {
            return stopped;
        }
        m_state->renderer.reset();
    }
    if (m_state->handler.generation != 0)
    {
        auto removed = unregister_windows_message_handler(*m_state->window, m_state->handler);
        if (!removed.has_value())
        {
            return removed;
        }
        m_state->handler = {};
    }
    cancel_frame();
    // 保存失敗は通知するが、登録解除済みの Context と Win32 Backend の解放は続ける
    lock.unlock();
    auto saved = m_state->isReady ? save_settings() : Result<void>::success();
    lock.lock();
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_state->context);
    if (m_state->isWin32Initialized)
    {
        ImGui_ImplWin32_Shutdown();
        m_state->isWin32Initialized = false;
    }
    ImGui::DestroyContext(m_state->context);
    ImGui::SetCurrentContext(previous == m_state->context ? nullptr : previous);
    m_state->context = nullptr;
    m_state->window = nullptr;
    m_state->isReady = false;
    return saved;
}
} // namespace cue
