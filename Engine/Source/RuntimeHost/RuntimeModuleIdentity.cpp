#include "RuntimeModuleIdentity.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace cue::runtime_host::detail
{
RuntimeModuleIdentityResult inspect_loaded_module_identity(HMODULE a_library, HANDLE a_guardedFile) noexcept
{
    std::vector<wchar_t> pathBuffer(MAX_PATH);
    DWORD pathLength = 0U;
    for (;;)
    {
        pathLength = GetModuleFileNameW(a_library, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
        if (pathLength == 0U)
        {
            return {RuntimeModuleIdentityStatus::QueryFailed, GetLastError()};
        }
        if (pathLength < pathBuffer.size())
        {
            break;
        }
        if (pathBuffer.size() >= 32768U)
        {
            return {RuntimeModuleIdentityStatus::QueryFailed, ERROR_FILENAME_EXCED_RANGE};
        }
        pathBuffer.resize(std::min<std::size_t>(pathBuffer.size() * 2U, 32768U));
    }

    HANDLE loadedFile = CreateFileW(pathBuffer.data(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (loadedFile == INVALID_HANDLE_VALUE)
    {
        return {RuntimeModuleIdentityStatus::QueryFailed, GetLastError()};
    }

    FILE_ID_INFO guardedIdentity{};
    FILE_ID_INFO loadedIdentity{};
    DWORD nativeError = ERROR_SUCCESS;
    if (GetFileInformationByHandleEx(a_guardedFile, FileIdInfo, &guardedIdentity, sizeof(guardedIdentity)) == FALSE ||
        GetFileInformationByHandleEx(loadedFile, FileIdInfo, &loadedIdentity, sizeof(loadedIdentity)) == FALSE)
    {
        nativeError = GetLastError();
    }
    CloseHandle(loadedFile);
    if (nativeError != ERROR_SUCCESS)
    {
        return {RuntimeModuleIdentityStatus::QueryFailed, nativeError};
    }

    const bool matches =
        guardedIdentity.VolumeSerialNumber == loadedIdentity.VolumeSerialNumber &&
        std::memcmp(guardedIdentity.FileId.Identifier, loadedIdentity.FileId.Identifier,
                    sizeof(guardedIdentity.FileId.Identifier)) == 0;
    return {matches ? RuntimeModuleIdentityStatus::Match : RuntimeModuleIdentityStatus::Mismatch, ERROR_SUCCESS};
}
} // namespace cue::runtime_host::detail
