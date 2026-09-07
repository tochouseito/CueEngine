#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Input/InputState.h>
#include <Cue/Input/Windows/WindowsInputMessageSink.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Platform/Windows/WindowsWindowInterop.h>

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 契約違反をTest Processの固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(75);
    }

    /// @brief Message付き契約違反をTest Processの固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

class RecordingSink final : public cue::WindowsMessageSink
{
  public:
    /// @brief Queueを非所有参照し、Downstream呼出し時点のEvent数を観測可能にする
    explicit RecordingSink(const cue::InputEventQueue &a_queue) noexcept : m_queue(&a_queue)
    {
    }

    /// @brief 全MessageをHandledとして返し、呼出し時点のQueue Event数を記録する
    [[nodiscard]] cue::WindowsMessageResult process_message(const cue::WindowsMessageView &) noexcept override
    {
        if (m_callCount < m_observedQueueSizes.size())
        {
            m_observedQueueSizes[m_callCount] = m_queue->size();
        }
        ++m_callCount;
        return {true, 67};
    }

    /// @brief Downstreamへ届いたMessage数を返す
    [[nodiscard]] std::size_t call_count() const noexcept
    {
        return m_callCount;
    }

    /// @brief 各Downstream呼出しより前に期待数のEventがQueueへ格納済みだった場合にtrueを返す
    template <std::size_t Size>
    [[nodiscard]] bool observed_queue_sizes(const std::array<std::size_t, Size> &a_expected) const noexcept
    {
        if (m_callCount < Size || Size > m_observedQueueSizes.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < Size; ++index)
        {
            if (m_observedQueueSizes[index] != a_expected[index])
            {
                return false;
            }
        }
        return true;
    }

  private:
    const cue::InputEventQueue *m_queue;
    std::array<std::size_t, 32> m_observedQueueSizes = {};
    std::size_t m_callCount = 0;
};

/// @brief Test用Loggerを追加Sinkなしで生成する
[[nodiscard]] std::unique_ptr<cue::Logger> create_logger(TestFatalHandler &a_handler)
{
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    return std::make_unique<cue::Logger>(a_handler, std::move(sinks));
}

/// @brief Queue先頭が期待するKey Eventか検証する
[[nodiscard]] bool pop_key(cue::InputEventQueue &a_queue, cue::InputEventType a_type, cue::InputKey a_key) noexcept
{
    cue::InputEvent event = {};
    return a_queue.try_pop(event) && event.type == a_type && event.key == a_key;
}

/// @brief Queue先頭が期待するMouse EventとClient座標か検証する
[[nodiscard]] bool pop_mouse(cue::InputEventQueue &a_queue, cue::InputEventType a_type, cue::InputMouseButton a_button,
                             cue::InputPoint a_position, std::int32_t a_wheelDelta = 0) noexcept
{
    cue::InputEvent event = {};
    return a_queue.try_pop(event) && event.type == a_type && event.mouseButton == a_button &&
           event.mousePosition.x == a_position.x && event.mousePosition.y == a_position.y &&
           event.wheelDelta == a_wheelDelta;
}

/// @brief 実Window配送でKey、Mouse、Wheel、Focus、Decorator順を検証する
[[nodiscard]] bool test_windows_input(cue::AssertContext &a_context)
{
    cue::Result<std::unique_ptr<cue::WindowSystem>> systemResult = cue::create_windows_window_system(a_context);
    if (!systemResult)
    {
        return false;
    }
    std::unique_ptr<cue::WindowSystem> system = std::move(*systemResult.try_value());
    cue::Result<std::unique_ptr<cue::Window>> windowResult =
        system->create_window({"Windows Input Message Sink Test", {640, 360}});
    if (!windowResult)
    {
        return false;
    }
    std::unique_ptr<cue::Window> window = std::move(*windowResult.try_value());
    if (!window->show())
    {
        return false;
    }

    cue::Result<cue::NativeWindowView> nativeViewResult = cue::get_native_window_view(*window, a_context);
    if (!nativeViewResult)
    {
        return false;
    }
    const void *nativeValue = nativeViewResult.try_value()->value();
    HWND nativeWindow = static_cast<HWND>(const_cast<void *>(nativeValue));
    if (nativeWindow == nullptr)
    {
        return false;
    }

    cue::InputEventQueue queue;
    RecordingSink downstream(queue);
    cue::WindowsInputMessageSink sink(queue, &downstream);
    if (!cue::attach_windows_message_sink(*window, sink, a_context))
    {
        return false;
    }

    constexpr LPARAM k_repeatState = static_cast<LPARAM>(1) << 30;
    if (SendMessageW(nativeWindow, WM_KEYDOWN, 'A', 0) != 67 ||
        SendMessageW(nativeWindow, WM_KEYDOWN, 'A', k_repeatState) != 67 ||
        SendMessageW(nativeWindow, WM_KEYUP, 'A', 0) != 67)
    {
        return false;
    }

    const UINT leftShiftScanCode = MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
    const UINT rightShiftScanCode = MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC);
    const LPARAM leftShiftParameter = static_cast<LPARAM>(leftShiftScanCode) << 16;
    const LPARAM rightShiftParameter = static_cast<LPARAM>(rightShiftScanCode) << 16;
    SendMessageW(nativeWindow, WM_KEYDOWN, VK_SHIFT, leftShiftParameter);
    SendMessageW(nativeWindow, WM_KEYDOWN, VK_SHIFT, rightShiftParameter);
    SendMessageW(nativeWindow, WM_KEYUP, VK_SHIFT, leftShiftParameter);

    constexpr LPARAM k_extendedKeyState = static_cast<LPARAM>(1) << 24;
    SendMessageW(nativeWindow, WM_KEYDOWN, VK_CONTROL, 0);
    SendMessageW(nativeWindow, WM_KEYDOWN, VK_CONTROL, k_extendedKeyState);
    SendMessageW(nativeWindow, WM_KEYUP, VK_CONTROL, 0);
    SendMessageW(nativeWindow, WM_SYSKEYDOWN, VK_MENU, 0);
    SendMessageW(nativeWindow, WM_SYSKEYDOWN, VK_MENU, k_extendedKeyState);
    SendMessageW(nativeWindow, WM_SYSKEYUP, VK_MENU, 0);

    SendMessageW(nativeWindow, WM_MOUSEMOVE, 0,
                 MAKELPARAM(static_cast<WORD>(static_cast<SHORT>(-7)), static_cast<WORD>(9)));
    SendMessageW(nativeWindow, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), MAKELPARAM(11, 13));

    POINT wheelPoint = {23, 17};
    if (ClientToScreen(nativeWindow, &wheelPoint) == FALSE)
    {
        return false;
    }
    SendMessageW(nativeWindow, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                 MAKELPARAM(static_cast<WORD>(wheelPoint.x), static_cast<WORD>(wheelPoint.y)));
    SendMessageW(nativeWindow, WM_KILLFOCUS, 0, 0);
    SendMessageW(nativeWindow, WM_CANCELMODE, 0, 0);
    SendMessageW(nativeWindow, WM_KEYDOWN, 0xFF, 0);
    SendMessageW(nativeWindow, WM_NULL, 0, 0);

    cue::InputEvent event = {};
    const bool eventsMatch =
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::A) &&
        pop_key(queue, cue::InputEventType::KeyRepeat, cue::InputKey::A) &&
        pop_key(queue, cue::InputEventType::KeyUp, cue::InputKey::A) &&
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::LeftShift) &&
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::RightShift) &&
        pop_key(queue, cue::InputEventType::KeyUp, cue::InputKey::LeftShift) &&
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::LeftControl) &&
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::RightControl) &&
        pop_key(queue, cue::InputEventType::KeyUp, cue::InputKey::LeftControl) &&
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::LeftAlt) &&
        pop_key(queue, cue::InputEventType::KeyDown, cue::InputKey::RightAlt) &&
        pop_key(queue, cue::InputEventType::KeyUp, cue::InputKey::LeftAlt) &&
        pop_mouse(queue, cue::InputEventType::MouseMove, cue::InputMouseButton::None, {-7, 9}) &&
        pop_mouse(queue, cue::InputEventType::MouseButtonDown, cue::InputMouseButton::X1, {11, 13}) &&
        pop_mouse(queue, cue::InputEventType::MouseWheel, cue::InputMouseButton::None, {23, 17}, WHEEL_DELTA) &&
        queue.try_pop(event) && event.type == cue::InputEventType::FocusLost && queue.try_pop(event) &&
        event.type == cue::InputEventType::DeviceReset && !queue.try_pop(event) && downstream.call_count() == 19;
    if (!eventsMatch)
    {
        return false;
    }

    constexpr std::array<std::size_t, 19> k_expectedQueueSizes = {1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
                                                                  11, 12, 13, 14, 15, 16, 17, 17, 17};
    if (!downstream.observed_queue_sizes(k_expectedQueueSizes))
    {
        return false;
    }

    cue::WindowsMessageView invalidWheel = {nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0};
    static_cast<void>(sink.process_message(invalidWheel));
    if (sink.conversion_failure_count() != 1 || !queue.try_pop(event) ||
        event.type != cue::InputEventType::DeviceReset || queue.try_pop(event) || downstream.call_count() != 20)
    {
        return false;
    }
    constexpr std::array<std::size_t, 20> k_expectedSizesWithFailure = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                                                        11, 12, 13, 14, 15, 16, 17, 17, 17, 1};
    if (!downstream.observed_queue_sizes(k_expectedSizesWithFailure))
    {
        return false;
    }

    cue::InputState modifierState;
    modifierState.begin_frame({});
    modifierState.apply_event({cue::InputEventType::KeyDown, cue::InputKey::LeftShift});
    modifierState.apply_event({cue::InputEventType::KeyDown, cue::InputKey::RightShift});
    modifierState.apply_event({cue::InputEventType::KeyUp, cue::InputKey::LeftShift});
    modifierState.apply_event({cue::InputEventType::KeyDown, cue::InputKey::LeftControl});
    modifierState.apply_event({cue::InputEventType::KeyDown, cue::InputKey::RightControl});
    modifierState.apply_event({cue::InputEventType::KeyUp, cue::InputKey::LeftControl});
    modifierState.apply_event({cue::InputEventType::KeyDown, cue::InputKey::LeftAlt});
    modifierState.apply_event({cue::InputEventType::KeyDown, cue::InputKey::RightAlt});
    modifierState.apply_event({cue::InputEventType::KeyUp, cue::InputKey::LeftAlt});
    if (modifierState.snapshot().is_key_down(cue::InputKey::LeftShift) ||
        !modifierState.snapshot().is_key_down(cue::InputKey::RightShift) ||
        modifierState.snapshot().is_key_down(cue::InputKey::LeftControl) ||
        !modifierState.snapshot().is_key_down(cue::InputKey::RightControl) ||
        modifierState.snapshot().is_key_down(cue::InputKey::LeftAlt) ||
        !modifierState.snapshot().is_key_down(cue::InputKey::RightAlt))
    {
        return false;
    }

    if (!cue::detach_windows_message_sink(*window, sink, a_context))
    {
        return false;
    }
    return window->destroy().has_value();
}
} // namespace

/// @brief Win32 MessageからPortable Inputへの変換とUI Sink DecoratorをProcess単位で検証する
int main()
{
    TestFatalHandler handler;
    std::unique_ptr<cue::Logger> logger = create_logger(handler);
    cue::AssertContext context(*logger, handler);
    return test_windows_input(context) ? 0 : 1;
}
