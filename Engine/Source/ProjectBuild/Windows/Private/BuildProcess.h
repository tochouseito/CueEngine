#pragma once

#include <span>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Foundation/Path.h>
#include <Foundation/Result.h>

namespace cue::detail
{
/// @brief Kernel Handle を一意所有する。無効値と Close の重複を防ぐ
class BuildHandle final
{
  public:
    /// @brief 未所有の状態を作る
    BuildHandle() = default;
    /// @brief Native Handle の所有権を受け取る
    explicit BuildHandle(HANDLE a_value) noexcept : m_value(a_value)
    {
    }
    /// @brief 最後の Handle を回収する
    ~BuildHandle()
    {
        reset();
    }
    BuildHandle(const BuildHandle &) = delete;
    BuildHandle &operator=(const BuildHandle &) = delete;
    /// @brief Handle の所有権だけを移譲する
    BuildHandle(BuildHandle &&a_other) noexcept : m_value(a_other.release())
    {
    }
    /// @brief 旧所有を閉じてから移譲する
    BuildHandle &operator=(BuildHandle &&a_other) noexcept
    {
        if (this != &a_other)
            reset(a_other.release());
        return *this;
    }
    /// @brief 呼出中だけ Native Handle を借用する
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_value;
    }
    /// @brief 有効な Handle を所有しているか返す
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_value && m_value != INVALID_HANDLE_VALUE;
    }
    /// @brief 所有を解放して Handle を返す
    [[nodiscard]] HANDLE release() noexcept
    {
        const auto value = m_value;
        m_value = nullptr;
        return value;
    }
    /// @brief 現所有を閉じ、次の Handle の所有へ切り替える
    void reset(HANDLE a_value = nullptr) noexcept
    {
        if (*this)
            CloseHandle(m_value);
        m_value = a_value;
    }

  private:
    HANDLE m_value = nullptr;
};

/// @brief Build Root Process と子孫の Job、出力 Pipe を所有する
struct BuildProcess final
{
    BuildHandle job;
    BuildHandle process;
    BuildHandle output;
};

/// @brief Command Processor を介さず、CMake と明示引数を継承 Handle 制限付きで起動する
[[nodiscard]] Result<BuildProcess> start_build_process(const Path &a_executable,
                                                       std::span<const std::string> a_arguments,
                                                       const Path &a_directory, const Path &a_vcpkgRoot);
/// @brief 製品を親の Build Job と独立して起動する。Handle を閉じても終了させない
[[nodiscard]] Result<BuildHandle> start_game_process(const Path &a_executable, const Path &a_directory);
/// @brief Package と共有 Tree の排他を共有なし File で非待機取得する。親 Directory は呼出前に用意する
[[nodiscard]] Result<BuildHandle> acquire_build_lock(const Path &a_projectRoot);
/// @brief 所有する排他 File の Handle を閉じ、別 Service の Build を許可する
void release_build_lock(BuildHandle &a_lock) noexcept;
/// @brief Path 識別用の固定長 Hash を生成する。File 内容や資格情報は含めない
[[nodiscard]] std::string build_path_identity(const Path &a_path);
/// @brief Native Error を呼出側へ返す分類へ揃える
[[nodiscard]] Error build_process_error(const char *a_operation, DWORD a_code = GetLastError());
} // namespace cue::detail
