#include <Cue/Platform/Windows/WindowsPlatform.h>

#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Cue/Foundation/Windows/UtfConversion.h>

namespace cue
{
namespace
{
class WindowsWindow;

/// @brief Win32の呼出ThreadとWindow Classを管理する
class WindowsWindowSystem final : public WindowSystem
{
public:
    /// @brief 呼出Threadと固有Window Class名を記録する
    WindowsWindowSystem();

    /// @brief Window破棄後にWindow Classを解除する
    ~WindowsWindowSystem() override;

    /// @brief 単一Main Windowを検証・生成する
    [[nodiscard]] Result<std::unique_ptr<Window>> create_window(const WindowDescriptor& a_descriptor) override;

    /// @brief Thread Queueを待機なしで処理する
    [[nodiscard]] Result<PumpStatus> pump_events() override;

    /// @brief Window ProcedureのEvent追加失敗を記録する
    void report_event_failure() noexcept;

    /// @brief Window破棄後に非所有参照を解除する
    void release_window(WindowsWindow* a_window) noexcept;

    /// @brief 公開済みMain Windowの終了要求を即時記録する
    void mark_quit_requested() noexcept;

private:
    /// @brief Window Classを必要なら登録する
    [[nodiscard]] Result<void> register_class();

    DWORD m_threadId;
    HINSTANCE m_instance;
    std::wstring m_className;
    WindowsWindow* m_active = nullptr;
    bool m_isClassRegistered = false;
    bool m_hasEventFailure = false;
    bool m_isQuitRequested = false;
};

/// @brief HWNDとEvent Queueを所有する
class WindowsWindow final : public Window
{
public:
    /// @brief 生成前のWindow Ownerを作る
    WindowsWindow(WindowsWindowSystem& a_system, DWORD a_threadId) noexcept
        : m_system(a_system), m_threadId(a_threadId)
    {
    }

    /// @brief Native Windowが残る場合は生成Thread上で解放する
    ~WindowsWindow() override
    {
        // HWND が残る場合は生成 Thread で破棄し、破棄失敗を見過ごさない
        if (m_handle)
        {
            if (m_threadId != GetCurrentThreadId() || !destroy().has_value())
            {
                std::terminate();
            }
        }
    }

    /// @brief Native生成成功後のHandleをRollback可能なOwnerへ関連付ける
    void attach(HWND a_handle) noexcept
    {
        m_handle = a_handle;
    }

    /// @brief Client Sizeを確定してMain Windowを公開状態へ移す
    void publish(WindowSize a_clientSize) noexcept
    {
        m_clientSize = a_clientSize;
        m_isPublished = true;
    }

    /// @brief Windowを表示する
    [[nodiscard]] Result<void> show() override
    {
        // Win32 操作と状態変更は生成 Thread だけで行う
        if (GetCurrentThreadId() != m_threadId)
        {
            return Result<void>::failure({ErrorCategory::WrongThread, "Window.show"});
        }
        if (!m_handle || m_state == WindowState::CloseRequested || m_state == WindowState::Destroyed)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "Window.show"});
        }

        // ShowWindow の呼出後に公開状態を更新する
        ShowWindow(m_handle, SW_SHOW);
        m_state = WindowState::Visible;
        return Result<void>::success();
    }

    /// @brief HWNDを明示的に破棄する
    [[nodiscard]] Result<void> destroy() override
    {
        // HWND を所有する Thread 以外からの破棄を拒否する
        if (GetCurrentThreadId() != m_threadId)
        {
            return Result<void>::failure({ErrorCategory::WrongThread, "Window.destroy"});
        }
        if (!m_handle)
        {
            return Result<void>::success();
        }
        // WM_DESTROY と WM_NCDESTROY は DestroyWindow の呼出中に同期して届く
        if (!DestroyWindow(m_handle))
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "DestroyWindow", GetLastError()});
        }

        // System 側に残る非所有参照も解除する
        m_system.release_window(this);
        return Result<void>::success();
    }

    /// @brief 現在のWindow状態を返す
    [[nodiscard]] WindowState state() const noexcept override
    {
        return m_state;
    }

    /// @brief 最後の非最小化Client Sizeを返す
    [[nodiscard]] WindowSize client_size() const noexcept override
    {
        return m_clientSize;
    }

    /// @brief Event Queueから一件を取得する
    [[nodiscard]] bool try_pop_event(WindowEvent& a_event) noexcept override
    {
        // Event がなければ出力引数を変更しない
        if (m_events.empty())
        {
            return false;
        }
        a_event = m_events.front();
        m_events.pop_front();
        return true;
    }

    /// @brief Window MessageをEventとLifecycle状態へ変換する
    static LRESULT CALLBACK window_proc(HWND a_handle, UINT a_message, WPARAM a_wParam, LPARAM a_lParam) noexcept
    {
        // 生成途中から届く Message を Window Owner へ関連付ける
        if (a_message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(a_lParam);
            auto* window = static_cast<WindowsWindow*>(create->lpCreateParams);
            SetLastError(0);
            if (SetWindowLongPtrW(a_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window)) == 0 &&
                GetLastError() != 0)
            {
                return FALSE;
            }
        }

        // 関連付け前または解除後の Message は Win32 標準処理へ委ねる
        auto* window = reinterpret_cast<WindowsWindow*>(GetWindowLongPtrW(a_handle, GWLP_USERDATA));
        if (!window)
        {
            return DefWindowProcW(a_handle, a_message, a_wParam, a_lParam);
        }

        switch (a_message)
        {
        case WM_CLOSE:
            // Close 要求を通知し、破棄するかどうかは Host に任せる
            if (window->m_state != WindowState::CloseRequested && window->m_state != WindowState::Destroyed)
            {
                window->m_state = WindowState::CloseRequested;
                window->queue_event({WindowEventType::CloseRequested, {}});
            }
            return 0;

        case WM_SIZE:
            // 最小化中は直前の有効な Client Size を保持する
            if (a_wParam == SIZE_MINIMIZED)
            {
                if (!window->m_isMinimized)
                {
                    window->m_isMinimized = true;
                    window->queue_event({WindowEventType::Minimized, {}});
                }
            }
            else
            {
                // 復元か通常の Size 変更かを一件の Event として通知する
                RECT clientRect{};
                if (GetClientRect(a_handle, &clientRect))
                {
                    const auto width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
                    const auto height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);
                    if (width != 0 && height != 0)
                    {
                        window->m_clientSize = {width, height};
                        const auto type = window->m_isMinimized ? WindowEventType::Restored : WindowEventType::Resized;
                        window->m_isMinimized = false;
                        window->queue_event({type, window->m_clientSize});
                    }
                }
            }
            return 0;

        case WM_DESTROY:
            // 公開前の Rollback では Application 全体の終了を通知しない
            window->m_state = WindowState::Destroyed;
            window->queue_event({WindowEventType::Destroyed, {}});
            if (window->m_isPublished)
            {
                window->m_system.mark_quit_requested();
                PostQuitMessage(0);
            }
            return 0;

        case WM_NCDESTROY:
            // HWND が無効になる前に Callback 用 Pointer と借用を解除する
            SetWindowLongPtrW(a_handle, GWLP_USERDATA, 0);
            window->m_handle = nullptr;
            window->m_system.release_window(window);
            return DefWindowProcW(a_handle, a_message, a_wParam, a_lParam);

        default:
            return DefWindowProcW(a_handle, a_message, a_wParam, a_lParam);
        }
    }

private:
    /// @brief Win32 Callback内のAllocation失敗をPump側へ渡す
    void queue_event(WindowEvent a_event) noexcept
    {
        // Win32 Callback から例外を出さず、後続の Pump で失敗を返す
        try
        {
            m_events.push_back(a_event);
        }
        catch (...)
        {
            m_system.report_event_failure();
        }
    }

    WindowsWindowSystem& m_system;
    DWORD m_threadId;
    HWND m_handle = nullptr;
    WindowSize m_clientSize{};
    WindowState m_state = WindowState::Created;
    std::deque<WindowEvent> m_events;
    bool m_isMinimized = false;
    bool m_isPublished = false;
};

/// @brief Thread固有のWindow Class名を作る
WindowsWindowSystem::WindowsWindowSystem()
    : m_threadId(GetCurrentThreadId()),
      m_instance(GetModuleHandleW(nullptr)),
      m_className(L"CueEngineWindow_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this)))
{
}

/// @brief 生成済みWindowがないことを確認してClassを解除する
WindowsWindowSystem::~WindowsWindowSystem()
{
    // Window より先に System が破棄されると非所有参照が失効する
    if (m_threadId != GetCurrentThreadId() || m_active)
    {
        std::terminate();
    }
    if (m_isClassRegistered)
    {
        UnregisterClassW(m_className.c_str(), m_instance);
    }
}

/// @brief Window Classを初回だけ登録する
Result<void> WindowsWindowSystem::register_class()
{
    // 同じ System での再試行では登録済み Class を再利用する
    if (m_isClassRegistered)
    {
        return Result<void>::success();
    }

    // Window Procedure と Class 名を Win32 へ登録する
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &WindowsWindow::window_proc;
    windowClass.hInstance = m_instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = m_className.c_str();
    if (!RegisterClassExW(&windowClass))
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "RegisterClassExW", GetLastError()});
    }

    m_isClassRegistered = true;
    return Result<void>::success();
}

/// @brief Descriptorを検証してWin32 Windowを生成する
Result<std::unique_ptr<Window>> WindowsWindowSystem::create_window(const WindowDescriptor& a_descriptor)
{
    using WindowResult = Result<std::unique_ptr<Window>>;

    // 呼び出し Thread が正しいか、既に Window があるか、終了要求が出ていないかを確認する
    if (GetCurrentThreadId() != m_threadId)
    {
        return WindowResult::failure({ErrorCategory::WrongThread, "WindowSystem.create_window"});
    }
    if (m_active || m_isQuitRequested)
    {
        return WindowResult::failure({ErrorCategory::InvalidState, "WindowSystem.create_window"});
    }
    // Descriptorの妥当性を確認する
    // タイトルが空、NUL文字、クライアントサイズが0、int型の最大値を超える場合は失敗とする
    if (a_descriptor.title.empty() || a_descriptor.title.find('\0') != std::string::npos ||
        a_descriptor.clientSize.width == 0 || a_descriptor.clientSize.height == 0 ||
        a_descriptor.clientSize.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        a_descriptor.clientSize.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return WindowResult::failure({ErrorCategory::InvalidArgument, "WindowDescriptor"});
    }

    // タイトルをUTF-16に変換する
    auto titleResult = utf8_to_utf16(a_descriptor.title);
    if (!titleResult.has_value())
    {
        return WindowResult::failure(std::move(*titleResult.try_error()));
    }
    std::wstring title = titleResult.take_value();

    // クライアントサイズからウィンドウの外枠サイズを計算する
    RECT outerRect{0, 0, static_cast<LONG>(a_descriptor.clientSize.width),
                   static_cast<LONG>(a_descriptor.clientSize.height)};
    constexpr DWORD k_windowStyle = WS_OVERLAPPEDWINDOW;
    if (!AdjustWindowRectEx(&outerRect, k_windowStyle, FALSE, 0))
    {
        return WindowResult::failure({ErrorCategory::PlatformFailure, "AdjustWindowRectEx", GetLastError()});
    }
    const auto outerWidth = static_cast<std::int64_t>(outerRect.right) - outerRect.left;
    const auto outerHeight = static_cast<std::int64_t>(outerRect.bottom) - outerRect.top;
    if (outerWidth <= 0 || outerHeight <= 0 || outerWidth > std::numeric_limits<int>::max() ||
        outerHeight > std::numeric_limits<int>::max())
    {
        return WindowResult::failure({ErrorCategory::InvalidArgument, "WindowDescriptor.clientSize"});
    }

    // Window Classを登録する
    auto registered = register_class();
    if (!registered.has_value())
    {
        return WindowResult::failure(std::move(*registered.try_error()));
    }

    // ウィンドウを作成する
    auto window = std::make_unique<WindowsWindow>(*this, m_threadId);
    const auto handle = CreateWindowExW(0, m_className.c_str(), title.c_str(), k_windowStyle,
                                       CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(outerWidth),
                                       static_cast<int>(outerHeight), nullptr, nullptr, m_instance, window.get());
    if (!handle)
    {
        return WindowResult::failure({ErrorCategory::PlatformFailure, "CreateWindowExW", GetLastError()});
    }

    // 作成した HWND を WindowsWindow の破棄対象として関連付ける
    window->attach(handle);

    // クライアントサイズを取得する
    RECT clientRect{};
    if (!GetClientRect(handle, &clientRect))
    {
        // Size を確定できなければ公開前の Window を破棄する
        const auto nativeCode = GetLastError();
        auto rollback = window->destroy();
        if (!rollback.has_value())
        {
            return WindowResult::failure(std::move(*rollback.try_error()));
        }
        return WindowResult::failure({ErrorCategory::PlatformFailure, "GetClientRect", nativeCode});
    }

    // 実際の Client Size を Window の状態に保存する
    window->publish({static_cast<std::uint32_t>(clientRect.right - clientRect.left),
                     static_cast<std::uint32_t>(clientRect.bottom - clientRect.top)});

    // Window の一意所有権は呼出側へ渡し、System は非所有 Pointer だけ保持する
    m_active = window.get();
    return WindowResult::success(std::move(window));
}

/// @brief Message Queueを最後まで処理し、終了要求を保持する
Result<PumpStatus> WindowsWindowSystem::pump_events()
{
    // Message Queue は生成 Thread に属する
    if (GetCurrentThreadId() != m_threadId)
    {
        return Result<PumpStatus>::failure({ErrorCategory::WrongThread, "WindowSystem.pump_events"});
    }

    // WM_QUIT は Dispatch せず終了状態として保持する
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            m_isQuitRequested = true;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Callback 内で Event を保持できなかった場合は成功を返さない
    if (m_hasEventFailure)
    {
        return Result<PumpStatus>::failure({ErrorCategory::PlatformFailure, "WindowEventQueue"});
    }
    return Result<PumpStatus>::success(m_isQuitRequested ? PumpStatus::QuitRequested : PumpStatus::Running);
}

/// @brief Event Queueの失敗を次回Pumpの診断へ伝える
void WindowsWindowSystem::report_event_failure() noexcept
{
    m_hasEventFailure = true;
}

/// @brief 明示破棄後にWindowの借用を解除する
void WindowsWindowSystem::release_window(WindowsWindow* a_window) noexcept
{
    // 別の Window からの解除で現在の借用を消さない
    if (m_active == a_window)
    {
        m_active = nullptr;
    }
}

/// @brief Main Window破棄後の再生成をWM_QUIT消費前から禁止する
void WindowsWindowSystem::mark_quit_requested() noexcept
{
    m_isQuitRequested = true;
}
} // namespace

/// @brief Windows用WindowSystemを生成する
Result<std::unique_ptr<WindowSystem>> create_windows_window_system()
{
    return Result<std::unique_ptr<WindowSystem>>::success(std::make_unique<WindowsWindowSystem>());
}
} // namespace cue
