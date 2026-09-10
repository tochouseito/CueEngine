#pragma once

#include <Windows.h>

namespace cue::runtime_host::detail
{
enum class RuntimeModuleIdentityStatus
{
    Match,
    Mismatch,
    QueryFailed,
};

struct RuntimeModuleIdentityResult final
{
    RuntimeModuleIdentityStatus status = RuntimeModuleIdentityStatus::QueryFailed;
    DWORD nativeError = ERROR_SUCCESS;
};

/// @brief Load済みModuleと固定済みPackage Fileが同一実体かFile IDで照合する
[[nodiscard]] RuntimeModuleIdentityResult inspect_loaded_module_identity(HMODULE a_library,
                                                                          HANDLE a_guardedFile) noexcept;
} // namespace cue::runtime_host::detail
