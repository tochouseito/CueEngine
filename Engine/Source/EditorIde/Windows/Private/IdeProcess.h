#pragma once

#include <span>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Foundation/Path.h>

namespace cue::ide_detail
{
/// @brief Bridge 用 Native Handle を一意所有する
class IdeHandle final
{
  public:
    /// @brief 未所有の状態を作る
    IdeHandle() = default;
    /// @brief Native Handle の所有権を受け取る
    explicit IdeHandle(HANDLE a_value) noexcept : m_value(a_value)
    {
    }
    /// @brief 所有中の Handle を回収する
    ~IdeHandle()
    {
        reset();
    }
    IdeHandle(const IdeHandle &) = delete;
    IdeHandle &operator=(const IdeHandle &) = delete;
    /// @brief 一意所有だけを移譲する
    IdeHandle(IdeHandle &&a_other) noexcept : m_value(a_other.release())
    {
    }
    /// @brief 旧 Handle を回収してから所有を移譲する
    IdeHandle &operator=(IdeHandle &&a_other) noexcept
    {
        if (this != &a_other)
            reset(a_other.release());
        return *this;
    }
    /// @brief 呼出中だけ Handle を借用する
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_value;
    }
    /// @brief 有効な Handle を持つか返す
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_value && m_value != INVALID_HANDLE_VALUE;
    }
    /// @brief 所有を放棄して Handle を返す
    [[nodiscard]] HANDLE release() noexcept
    {
        const auto value = m_value;
        m_value = nullptr;
        return value;
    }
    /// @brief 現所有を解放する
    void reset(HANDLE a_value = nullptr) noexcept
    {
        if (*this)
            CloseHandle(m_value);
        m_value = a_value;
    }

  private:
    HANDLE m_value = nullptr;
};
/// @brief 所有 Bridge の Job / Process / 出力を保持する。IDE 自身は Job から離脱して起動する
struct IdeProcess final
{
    IdeHandle job;
    IdeHandle process;
    IdeHandle output;
};
/// @brief Shell を介さず制限した標準 Handle と一意 Job で Bridge を起動する
[[nodiscard]] Result<IdeProcess> start_bridge(const Path &a_executable, std::span<const std::string> a_arguments);
/// @brief Native 診断を Authoring Error に揃える
[[nodiscard]] Error process_error(const char *a_operation, DWORD a_code = GetLastError());
} // namespace cue::ide_detail
