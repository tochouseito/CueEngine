#include "WindowsWindow.h"

#include "UtfConversion.h"
#include "WindowsUtilities.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>

#include <Windows.h>

#include <atomic>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace
{
using cue::windows_private::make_error;
using cue::windows_private::make_native_error;
using cue::windows_private::query_client_size;

static_assert(sizeof(std::uintptr_t) >= sizeof(std::uint64_t),
              "Windows Dialog Owner Generation requires a 64-bit native handle representation");

constexpr wchar_t k_windowClassName[] = L"CueEngine.Window";
constexpr DWORD k_windowStyle = WS_OVERLAPPEDWINDOW;

constexpr std::int64_t k_invalidDescriptor = 3;
constexpr std::int64_t k_windowRectangleFailed = 4;
constexpr std::int64_t k_classRegistrationFailed = 5;
constexpr std::int64_t k_windowCreationFailed = 6;
constexpr std::int64_t k_windowDestroyFailed = 8;
constexpr std::int64_t k_windowAlreadyExists = 9;
constexpr std::int64_t k_classUnregistrationFailed = 10;

/// @brief Process内で再利用しないNative Dialog Owner Generationを発行する
[[nodiscard]] std::uint64_t next_dialog_owner_generation() noexcept
{
    static std::atomic<std::uint64_t> nextGeneration{1U};
    const std::uint64_t generation = nextGeneration.fetch_add(1U, std::memory_order_relaxed);
    if (generation == 0U)
    {
        std::abort();
    }
    return generation;
}

/// @brief Allocation 失敗を追加 Allocation なしで Fatal 終了境界へ渡し、復帰時も Process を停止する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Windows Window allocation failed");
    std::abort();
}

/// @brief Process 内の WindowSystem 間で Win32 Window Class の登録寿命を共有する
///
/// Win32 Window Class は個々の HWND ではなく Module に属するため、最初の参照で登録し最後の参照で解除する
class WindowClassRegistry final
{
  public:
    /// @brief Win32 Window を所有権と Lifecycle 規則を守って関連付ける
    [[nodiscard]] cue::Result<void> acquire(const cue::AssertContext &a_context, HINSTANCE a_instance) noexcept
    {
        AcquireSRWLockExclusive(&m_lock);

        if (m_referenceCount > 0)
        {
            // 同名 Class を異なる Module 定義として共有すると WndProc と Instance の対応が崩れる
            CUE_ASSERT(a_context, m_instance == a_instance,
                       "Window Class Registry can only manage one Module instance");
            ++m_referenceCount;
            ReleaseSRWLockExclusive(&m_lock);
            return cue::Result<void>::success();
        }

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = cue::WindowsWindow::window_procedure;
        windowClass.hInstance = a_instance;
        windowClass.lpszClassName = k_windowClassName;

        if (RegisterClassExW(&windowClass) == 0)
        {
            DWORD nativeCode = GetLastError();
            WNDCLASSEXW existingClass = {};
            existingClass.cbSize = sizeof(existingClass);
            // 同じ WndProc の登録だけを共有し、外部 Class との名前衝突は明示的な失敗にする
            bool canAcquireExisting = nativeCode == ERROR_CLASS_ALREADY_EXISTS &&
                                      GetClassInfoExW(a_instance, k_windowClassName, &existingClass) != FALSE &&
                                      existingClass.lpfnWndProc == cue::WindowsWindow::window_procedure;

            if (!canAcquireExisting)
            {
                ReleaseSRWLockExclusive(&m_lock);
                return cue::Result<void>::failure(make_native_error(
                    a_context, k_classRegistrationFailed, "Windows Window Class registration failed", nativeCode));
            }
        }

        m_instance = a_instance;
        m_referenceCount = 1;
        ReleaseSRWLockExclusive(&m_lock);
        return cue::Result<void>::success();
    }

    /// @brief 保持する Native Resource を完了条件と所有権規則に従って解放する
    [[nodiscard]] cue::Result<void> release(const cue::AssertContext &a_context, HINSTANCE a_instance) noexcept
    {
        AcquireSRWLockExclusive(&m_lock);
        CUE_ASSERT(a_context, m_referenceCount > 0, "Window Class reference count must not underflow");
        CUE_ASSERT(a_context, m_instance == a_instance,
                   "Window Class reference must be released by its Module instance");
        --m_referenceCount;

        if (m_referenceCount > 0)
        {
            ReleaseSRWLockExclusive(&m_lock);
            return cue::Result<void>::success();
        }

        // 最後の Window が解放された時点でのみ解除し、存続中 HWND の WndProc を失わせない
        BOOL didUnregister = UnregisterClassW(k_windowClassName, a_instance);
        DWORD nativeCode = didUnregister != FALSE ? ERROR_SUCCESS : GetLastError();
        m_instance = nullptr;
        ReleaseSRWLockExclusive(&m_lock);

        if (didUnregister != FALSE || nativeCode == ERROR_CLASS_DOES_NOT_EXIST)
        {
            return cue::Result<void>::success();
        }

        return cue::Result<void>::failure(make_native_error(a_context, k_classUnregistrationFailed,
                                                            "Windows Window Class unregistration failed", nativeCode));
    }

  private:
    SRWLOCK m_lock = SRWLOCK_INIT;
    HINSTANCE m_instance = nullptr;
    std::uint32_t m_referenceCount = 0;
};

/// @brief Process 内で共有する Win32 Window Class 登録状態への Access を提供する
[[nodiscard]] WindowClassRegistry &window_class_registry() noexcept
{
    // Win32 Window Class は Module 単位 Resource のため、WindowSystem 間で参照数を共有する
    static WindowClassRegistry registry;
    return registry;
}

/// @brief Win32 Window の Descriptor が期待する契約を満たすか検証する
[[nodiscard]] cue::Result<void> validate_descriptor(const cue::WindowDescriptor &a_descriptor,
                                                    const cue::AssertContext &a_context) noexcept
{
    // Native API 呼出前に拒否し、生成途中の HWND や Window Class 参照を残さない
    if (a_descriptor.clientSize.width == 0 || a_descriptor.clientSize.height == 0 ||
        a_descriptor.title.find('\0') != std::string_view::npos)
    {
        return cue::Result<void>::failure(make_error(a_context, k_invalidDescriptor, "Window descriptor is invalid"));
    }

    constexpr std::uint32_t k_maxSignedSize = static_cast<std::uint32_t>(std::numeric_limits<int>::max());

    if (a_descriptor.clientSize.width > k_maxSignedSize || a_descriptor.clientSize.height > k_maxSignedSize)
    {
        return cue::Result<void>::failure(
            make_error(a_context, k_invalidDescriptor, "Window client size is out of range"));
    }

    return cue::Result<void>::success();
}

/// @brief 要求された Client Size から Win32 Window 全体の Rectangle を計算する
[[nodiscard]] cue::Result<RECT> calculate_window_rectangle(const cue::WindowDescriptor &a_descriptor,
                                                           const cue::AssertContext &a_context) noexcept
{
    // Runtime が要求する Client Area を確保するため、Frame と Title Bar を含む外寸へ変換する
    RECT rectangle = {0, 0, static_cast<LONG>(a_descriptor.clientSize.width),
                      static_cast<LONG>(a_descriptor.clientSize.height)};

    if (AdjustWindowRectEx(&rectangle, k_windowStyle, FALSE, 0) == FALSE)
    {
        DWORD nativeCode = GetLastError();
        return cue::Result<RECT>::failure(
            make_native_error(a_context, k_windowRectangleFailed, "Window rectangle calculation failed", nativeCode));
    }

    std::int64_t width = static_cast<std::int64_t>(rectangle.right) - rectangle.left;
    std::int64_t height = static_cast<std::int64_t>(rectangle.bottom) - rectangle.top;

    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max())
    {
        return cue::Result<RECT>::failure(
            make_error(a_context, k_invalidDescriptor, "Window outer size is out of range"));
    }

    rectangle.left = 0;
    rectangle.top = 0;
    rectangle.right = static_cast<LONG>(width);
    rectangle.bottom = static_cast<LONG>(height);
    return cue::Result<RECT>::success(std::move(rectangle));
}
} // namespace

namespace cue
{
WindowsWindowSystem::WindowsWindowSystem(const AssertContext &a_assertContext, HINSTANCE a_instance) noexcept
    : m_assertContext(&a_assertContext), m_instance(a_instance), m_threadId(GetCurrentThreadId())
{
}

WindowsWindowSystem::~WindowsWindowSystem() noexcept
{
    CUE_ASSERT(*m_assertContext, GetCurrentThreadId() == m_threadId,
               "Window System must be destroyed on its creation thread");
    CUE_ASSERT(*m_assertContext, m_window == nullptr, "Window System must outlive all Windows Window owners");
}

Result<std::unique_ptr<Window>> WindowsWindowSystem::create_window(const WindowDescriptor &a_descriptor) noexcept
{
    CUE_ASSERT(*m_assertContext, GetCurrentThreadId() == m_threadId,
               "Window must be created on the Window System thread");

    const AssertContext &context = *m_assertContext;

    if (m_window != nullptr)
    {
        return Result<std::unique_ptr<Window>>::failure(
            make_error(context, k_windowAlreadyExists, "Window System already has a Main Window"));
    }

    // Native Resource を取得する前に入力と変換を完了し、失敗時の Rollback 対象を増やさない
    Result<void> descriptorResult = validate_descriptor(a_descriptor, context);

    if (!descriptorResult)
    {
        return Result<std::unique_ptr<Window>>::failure(std::move(*descriptorResult.try_error()));
    }

    Result<std::wstring> titleResult = utf8_to_utf16(a_descriptor.title, context);

    if (!titleResult)
    {
        return Result<std::unique_ptr<Window>>::failure(std::move(*titleResult.try_error()));
    }

    Result<RECT> rectangleResult = calculate_window_rectangle(a_descriptor, context);

    if (!rectangleResult)
    {
        return Result<std::unique_ptr<Window>>::failure(std::move(*rectangleResult.try_error()));
    }

    try
    {
        std::unique_ptr<WindowsWindow> window = std::make_unique<WindowsWindow>(*this);
        // Class 参照を Window 所有へ移してから HWND を作り、以降の全失敗を RAII で回収する
        Result<void> classResult = register_window_class();

        if (!classResult)
        {
            return Result<std::unique_ptr<Window>>::failure(std::move(*classResult.try_error()));
        }

        window->acquire_class_reference();
        RECT *rectangle = rectangleResult.try_value();
        std::wstring *title = titleResult.try_value();
        Result<void> createResult =
            window->create_native(*title, static_cast<int>(rectangle->right), static_cast<int>(rectangle->bottom));

        if (!createResult)
        {
            return Result<std::unique_ptr<Window>>::failure(std::move(*createResult.try_error()));
        }

        // 生成中 Message を Runtime Event と誤認しないよう、完全な Client Size の取得後に公開する
        window->publish();
        std::unique_ptr<Window> result = std::move(window);
        return Result<std::unique_ptr<Window>>::success(std::move(result));
    }
    catch (...)
    {
        terminate_allocation(context);
    }
}

Result<PumpStatus> WindowsWindowSystem::pump_events() noexcept
{
    CUE_ASSERT(*m_assertContext, GetCurrentThreadId() == m_threadId,
               "Window events must be pumped on the Window System thread");
    MSG message = {};
    bool isQuitRequested = false;

    // Frame Loop を Blocking せず、現在届いている Message を全て処理して Window の応答性を保つ
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
    {
        if (message.message == WM_QUIT)
        {
            // WM_QUIT 後も Queue を Drain し、同一 Frame に到着済みの状態通知を取りこぼさない
            isQuitRequested = true;
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return Result<PumpStatus>::success(isQuitRequested ? PumpStatus::QuitRequested : PumpStatus::Running);
}

const AssertContext &WindowsWindowSystem::assert_context() const noexcept
{
    return *m_assertContext;
}

HINSTANCE WindowsWindowSystem::instance() const noexcept
{
    return m_instance;
}

DWORD WindowsWindowSystem::thread_id() const noexcept
{
    return m_threadId;
}

void WindowsWindowSystem::publish_window(WindowsWindow &a_window) noexcept
{
    CUE_ASSERT(*m_assertContext, GetCurrentThreadId() == m_threadId, "Window must be published on the Window thread");
    CUE_ASSERT(*m_assertContext, m_window == nullptr, "Window System can publish only one Main Window");
    m_window = &a_window;
}

Result<void> WindowsWindowSystem::release_window(WindowsWindow &a_window) noexcept
{
    CUE_ASSERT(*m_assertContext, GetCurrentThreadId() == m_threadId,
               "Window must release its System reference on the Window thread");
    CUE_ASSERT(*m_assertContext, m_window == nullptr || m_window == &a_window,
               "Window System can only release its current Main Window");

    if (m_window == &a_window)
    {
        m_window = nullptr;
    }

    return unregister_window_class();
}

Result<void> WindowsWindowSystem::register_window_class() noexcept
{
    return window_class_registry().acquire(*m_assertContext, m_instance);
}

Result<void> WindowsWindowSystem::unregister_window_class() noexcept
{
    return window_class_registry().release(*m_assertContext, m_instance);
}

WindowsWindow::WindowsWindow(WindowsWindowSystem &a_system) noexcept
    : m_system(&a_system), m_dialogOwnerGeneration(next_dialog_owner_generation())
{
}

WindowsWindow::~WindowsWindow() noexcept
{
    verify_thread();

    if (m_window != nullptr)
    {
        // 明示的な destroy() がなくても HWND を残さず、基底所有権の破棄だけで Native 寿命を閉じる
        static_cast<void>(DestroyWindow(m_window));
    }

    Result<void> released = release_system_reference();
    if (!released)
    {
        static_cast<void>(m_system->assert_context().logger().log(
            LogLevel::Warning, "Windows Window Class could not be unregistered", std::move(*released.try_error())));
    }
}

Result<void> WindowsWindow::show() noexcept
{
    verify_thread();
    CUE_ASSERT(m_system->assert_context(), m_state == WindowState::Created,
               "Window can only be shown from Created state");
    // 生成と表示を分離し、RHI が表示前に Native View から SwapChain を準備できるようにする
    ShowWindow(m_window, SW_SHOW);
    m_state = WindowState::Visible;
    return Result<void>::success();
}

Result<void> WindowsWindow::destroy() noexcept
{
    verify_thread();

    if (m_state == WindowState::Destroyed)
    {
        return Result<void>::success();
    }

    if (DestroyWindow(m_window) == FALSE)
    {
        DWORD nativeCode = GetLastError();
        return Result<void>::failure(make_native_error(m_system->assert_context(), k_windowDestroyFailed,
                                                       "Windows Window destruction failed", nativeCode));
    }

    // DestroyWindow は同期的に WM_NCDESTROY まで配送し、そこで所有者との関連を切る契約とする
    CUE_ASSERT(m_system->assert_context(), m_window == nullptr,
               "DestroyWindow must synchronously detach the Window owner");
    return release_system_reference();
}

WindowState WindowsWindow::state() const noexcept
{
    verify_thread();
    return m_state;
}

WindowSize WindowsWindow::client_size() const noexcept
{
    verify_thread();
    return m_clientSize;
}

bool WindowsWindow::try_pop_event(WindowEvent &a_event) noexcept
{
    verify_thread();

    if (m_eventReadIndex == m_events.size())
    {
        return false;
    }

    a_event = m_events[m_eventReadIndex];
    ++m_eventReadIndex;

    if (a_event.type == WindowEventType::CloseRequested)
    {
        // Runtime が要求を消費した後は、OS からの新しい Close 要求を再び通知可能にする
        m_isClosePending = false;
    }

    if (m_eventReadIndex == m_events.size())
    {
        // 全 Event を消費した時点で Storage を再利用し、未読 Queue の順序は維持する
        m_events.clear();
        m_eventReadIndex = 0;
    }

    return true;
}

const void *WindowsWindow::native_view_value() const noexcept
{
    verify_thread();
    CUE_ASSERT(m_system->assert_context(), m_state != WindowState::Destroyed,
               "Destroyed Window does not have a Native View");
    return m_window;
}

std::uint64_t WindowsWindow::dialog_owner_generation() const noexcept
{
    return m_dialogOwnerGeneration;
}

void WindowsWindow::acquire_class_reference() noexcept
{
    verify_thread();
    CUE_ASSERT(m_system->assert_context(), !m_hasClassReference, "Window Class reference must only be acquired once");
    m_hasClassReference = true;
}

Result<void> WindowsWindow::create_native(std::wstring_view a_title, int a_width, int a_height) noexcept
{
    verify_thread();
    CUE_ASSERT(m_system->assert_context(), m_hasClassReference,
               "Window Class must be registered before native creation");
    CUE_ASSERT(m_system->assert_context(), m_window == nullptr, "Native Window must only be created once");

    // this を lpParam へ渡し、最初の WM_NCCREATE から WndProc を本 Object へ関連付ける
    HWND window = CreateWindowExW(0, k_windowClassName, a_title.data(), k_windowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                                  a_width, a_height, nullptr, nullptr, m_system->instance(), this);

    if (window == nullptr)
    {
        DWORD nativeCode = GetLastError();
        return Result<void>::failure(make_native_error(m_system->assert_context(), k_windowCreationFailed,
                                                       "Windows Window creation failed", nativeCode));
    }

    CUE_ASSERT(m_system->assert_context(), m_window == window,
               "Window Procedure must attach the native handle during creation");

    if (SetPropW(window, k_windowsDialogOwnerGenerationProperty,
                 reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(m_dialogOwnerGeneration))) == FALSE)
    {
        const DWORD nativeCode = GetLastError();
        static_cast<void>(DestroyWindow(window));
        return Result<void>::failure(make_native_error(m_system->assert_context(), k_windowCreationFailed,
                                                       "Windows Window owner generation publication failed",
                                                       nativeCode));
    }

    Result<WindowSize> clientSizeResult = query_client_size(window, m_system->assert_context());

    if (!clientSizeResult)
    {
        static_cast<void>(DestroyWindow(window));
        return Result<void>::failure(std::move(*clientSizeResult.try_error()));
    }

    m_clientSize = *clientSizeResult.try_value();
    m_state = WindowState::Created;
    return Result<void>::success();
}

void WindowsWindow::publish() noexcept
{
    verify_thread();
    CUE_ASSERT(m_system->assert_context(), m_window != nullptr, "Native Window must exist before publication");
    // WindowSystem へ公開してからだけ Event を蓄積し、呼出側が未取得の Window を通知対象にしない
    m_isPublished = true;
    m_system->publish_window(*this);
}

LRESULT CALLBACK WindowsWindow::window_procedure(HWND a_window, UINT a_message, WPARAM a_wParam,
                                                 LPARAM a_lParam) noexcept
{
    // Win32 の静的 Callback から、HWND に保存した WindowsWindow の Message 処理へ橋渡しする
    WindowsWindow *owner = reinterpret_cast<WindowsWindow *>(GetWindowLongPtrW(a_window, GWLP_USERDATA));

    if (a_message == WM_NCCREATE)
    {
        // HWND 生成の最初期に Owner を保存し、CreateWindowExW 中の後続 Message も同じ Object で処理する
        CREATESTRUCTW *createData = reinterpret_cast<CREATESTRUCTW *>(a_lParam);
        owner = static_cast<WindowsWindow *>(createData->lpCreateParams);

        SetLastError(ERROR_SUCCESS);
        LONG_PTR previousOwner = SetWindowLongPtrW(a_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));

        if (previousOwner == 0 && GetLastError() != ERROR_SUCCESS)
        {
            return FALSE;
        }

        owner->m_window = a_window;
    }

    if (owner == nullptr)
    {
        // Owner 関連付け前または解除後の Message は Win32 の既定処理へ戻す
        return DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
    }

    return owner->process_message(a_message, a_wParam, a_lParam);
}

LRESULT WindowsWindow::process_message(UINT a_message, WPARAM a_wParam, LPARAM a_lParam) noexcept
{
    if (a_message == WM_CLOSE)
    {
        // OS の既定破棄を抑止し、保存確認などを行う Runtime 側へ終了判断を委ねる
        if (m_isPublished && m_state != WindowState::Destroyed && !m_isClosePending)
        {
            push_event({WindowEventType::CloseRequested, {0, 0}});
            m_state = WindowState::CloseRequested;
            m_isClosePending = true;
        }

        return 0;
    }

    if (a_message == WM_DESTROY)
    {
        // 公開済み Window だけ破棄を Event 化し、Runtime Loop の終了要求を Thread Message Queue へ送る
        // 公開前の生成 Rollback では通知先が存在しないため Event 追加と PostQuitMessage を行わない
        m_state = WindowState::Destroyed;
        m_messageSink = nullptr;

        if (m_isPublished)
        {
            push_event({WindowEventType::Destroyed, {0, 0}});
            PostQuitMessage(0);
        }

        return 0;
    }

    if (a_message == WM_SIZE && m_isPublished && m_state != WindowState::Destroyed)
    {
        if (a_wParam == SIZE_MINIMIZED)
        {
            // 最小化中の 0 Size を Resize として扱わず、描画休止用の意味ある Event へ変換する
            push_event({WindowEventType::Minimized, {0, 0}});
            m_isMinimized = true;
            return 0;
        }

        Result<WindowSize> clientSizeResult = query_client_size(m_window, m_system->assert_context());

        if (!clientSizeResult)
        {
            report_fatal(m_system->assert_context().logger(), m_system->assert_context().fatal_handler(),
                         "Window Event conversion failed", std::move(*clientSizeResult.try_error()));
        }

        WindowSize clientSize = *clientSizeResult.try_value();

        if (clientSize.width == 0 || clientSize.height == 0)
        {
            // SwapChain の Resize 対象にならない 0 Size は、復帰後の有効 Size 通知まで保留する
            return 0;
        }

        bool isRestored = a_wParam == SIZE_RESTORED && m_isMinimized;
        WindowEventType type = isRestored ? WindowEventType::Restored : WindowEventType::Resized;
        push_event({type, clientSize});
        m_clientSize = clientSize;
        m_isMinimized = false;
        return 0;
    }

    if (a_message == WM_NCDESTROY)
    {
        // HWND が完全に無効になる最後の通知で関連付けを消し、Interop へ失効 Handle を返さない
        HWND window = m_window;
        static_cast<void>(RemovePropW(window, k_windowsDialogOwnerGenerationProperty));
        LRESULT result = DefWindowProcW(window, a_message, a_wParam, a_lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        m_window = nullptr;
        return result;
    }

    if (m_messageSink != nullptr)
    {
        WindowsMessageView message = {
            m_window,
            static_cast<std::uint32_t>(a_message),
            static_cast<std::uintptr_t>(a_wParam),
            static_cast<std::intptr_t>(a_lParam),
        };
        m_isDispatchingMessageSink = true;
        WindowsMessageResult result = m_messageSink->process_message(message);
        m_isDispatchingMessageSink = false;

        if (result.isHandled)
        {
            return static_cast<LRESULT>(result.nativeResult);
        }
    }

    return DefWindowProcW(m_window, a_message, a_wParam, a_lParam);
}

void WindowsWindow::push_event(WindowEvent a_event) noexcept
{
    try
    {
        if (m_eventReadIndex > 0 && m_eventReadIndex >= m_events.size() / 2)
        {
            // 読み取り済み領域が大きくなった時だけ詰め、通常の Pop ごとの要素移動を避ける
            m_events.erase(m_events.begin(), m_events.begin() + m_eventReadIndex);
            m_eventReadIndex = 0;
        }

        m_events.push_back(a_event);
    }
    catch (...)
    {
        m_system->assert_context().fatal_handler().terminate("Window Event Queue allocation failed");
        std::abort();
    }
}

Result<void> WindowsWindow::release_system_reference() noexcept
{
    if (!m_hasClassReference)
    {
        return Result<void>::success();
    }

    // WindowSystem の Main Window 参照と共有 Window Class 参照を同じ一回の解放へまとめる
    m_hasClassReference = false;
    m_isPublished = false;
    return m_system->release_window(*this);
}

void WindowsWindow::verify_thread() const noexcept
{
    CUE_ASSERT(m_system->assert_context(), GetCurrentThreadId() == m_system->thread_id(),
               "Windows Window operation must run on the Window thread");
}
} // namespace cue
