#pragma once

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Result.h>

#include <cstdint>

#include <Windows.h>

namespace cue::windows_io
{
/// @brief Mount Manager Volume Objectと128-bit File IDを比較可能な整数列として保持する
struct NativeFilesystemIdentity final
{
    std::uint64_t volumeHigh = 0U;
    std::uint64_t volumeLow = 0U;
    std::uint64_t entryHigh = 0U;
    std::uint64_t entryLow = 0U;
};

/// @brief Open済みHandleからVolume GUIDと128-bit File IDを取得する
[[nodiscard]] Result<NativeFilesystemIdentity> inspect_native_filesystem_identity(
    HANDLE a_handle, const AssertContext &a_assertContext) noexcept;
} // namespace cue::windows_io
