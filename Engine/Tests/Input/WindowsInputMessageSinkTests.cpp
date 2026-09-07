#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Input/Windows/WindowsInputMessageSink.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Platform/Windows/WindowsWindowInterop.h>

#include <Windows.h>

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
    /// @brief 全MessageをHandledとして返し、Decoratorの転送を観測可能にする
    [[nodiscard]] cue::WindowsMessageResult process_message(const cue::WindowsMessageView &) noexcept override
    {
        ++m_callCount;
        return {true, 67};
    }

    /// @brief Downstreamへ届いたMessage数を返す
    [[nodiscard]] std::size_t call_count() const noexcept
    {
        return m_callCount;
    }

  private:
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
    RecordingSink downstream;
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
        pop_mouse(queue, cue::InputEventType::MouseMove, cue::InputMouseButton::None, {-7, 9}) &&
        pop_mouse(queue, cue::InputEventType::MouseButtonDown, cue::InputMouseButton::X1, {11, 13}) &&
        pop_mouse(queue, cue::InputEventType::MouseWheel, cue::InputMouseButton::None, {23, 17}, WHEEL_DELTA) &&
        queue.try_pop(event) && event.type == cue::InputEventType::FocusLost && queue.try_pop(event) &&
        event.type == cue::InputEventType::DeviceReset && !queue.try_pop(event) && downstream.call_count() == 10;
    if (!eventsMatch)
    {
        return false;
    }

    cue::WindowsMessageView invalidWheel = {nullptr, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0};
    static_cast<void>(sink.process_message(invalidWheel));
    if (sink.conversion_failure_count() != 1 || !queue.try_pop(event) ||
        event.type != cue::InputEventType::DeviceReset || queue.try_pop(event))
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
