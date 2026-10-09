#include <Platform/Windows/WindowsDebugLogSink.h>

#include <limits>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace cue
{
namespace
{
/// @brief Win32 の具体的な出力先を公開契約の外へ閉じ込める
class WindowsDebugLogSink final : public ILogSink
{
  public:
    /// @brief 構成に依存しない有効設定を受け取る
    explicit WindowsDebugLogSink(bool a_isEnabled) : m_isEnabled(a_isEnabled)
    {
    }
    /// @brief 同じ整形内容を UTF-16 へ変換し、Visual Studio 等の Debugger へ渡す
    Result<void> write(std::string_view a_line) override
    {
        if (m_isClosed)
        {
            return Result<void>::failure({ErrorCategory::InvalidState, "DebugLogSink.closed"});
        }
        if (!m_isEnabled || a_line.empty())
        {
            return Result<void>::success();
        }
        if (a_line.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
            a_line.find('\0') != std::string_view::npos)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "DebugLogSink.input"});
        }
        const auto size = static_cast<int>(a_line.size());
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_line.data(), size, nullptr, 0);
        if (count == 0)
        {
            return Result<void>::failure({ErrorCategory::InvalidArgument, "DebugLogSink.utf8", GetLastError()});
        }
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_line.data(), size, wide.data(), count) != count)
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "DebugLogSink.convert", GetLastError()});
        }
        OutputDebugStringW(wide.c_str());
        return Result<void>::success();
    }
    /// @brief Native 側に Flush がないため、開いていることだけを検証する
    Result<void> flush() override
    {
        return m_isClosed ? Result<void>::failure({ErrorCategory::InvalidState, "DebugLogSink.closed"})
                          : Result<void>::success();
    }
    /// @brief 以後の配送を停止する
    Result<void> close() override
    {
        m_isClosed = true;
        return Result<void>::success();
    }

  private:
    bool m_isEnabled;
    bool m_isClosed = false;
};
} // namespace

std::unique_ptr<ILogSink> create_windows_debug_log_sink(bool a_isEnabled)
{
    return std::make_unique<WindowsDebugLogSink>(a_isEnabled);
}
} // namespace cue
