#include "RuntimeModuleIdentity.h"

#include <Windows.h>

#include <filesystem>

/// @brief Load済みModuleのFile ID照合が同一実体を許可し別Copyを拒否することを検証する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    if (a_argumentCount != 3)
    {
        return 1;
    }

    const std::filesystem::path sourcePath = a_arguments[1];
    const std::filesystem::path copiedPath = a_arguments[2];
    std::error_code filesystemError;
    std::filesystem::create_directories(copiedPath.parent_path(), filesystemError);
    std::filesystem::copy_file(sourcePath, copiedPath, std::filesystem::copy_options::overwrite_existing,
                               filesystemError);
    if (filesystemError)
    {
        return 2;
    }

    HMODULE library = LoadLibraryExW(sourcePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HANDLE sourceFile = CreateFileW(sourcePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    HANDLE copiedFile = CreateFileW(copiedPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (library == nullptr || sourceFile == INVALID_HANDLE_VALUE || copiedFile == INVALID_HANDLE_VALUE)
    {
        if (copiedFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(copiedFile);
        }
        if (sourceFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(sourceFile);
        }
        if (library != nullptr)
        {
            FreeLibrary(library);
        }
        return 3;
    }

    const cue::runtime_host::detail::RuntimeModuleIdentityResult matchingIdentity =
        cue::runtime_host::detail::inspect_loaded_module_identity(library, sourceFile);
    const cue::runtime_host::detail::RuntimeModuleIdentityResult copiedIdentity =
        cue::runtime_host::detail::inspect_loaded_module_identity(library, copiedFile);
    CloseHandle(copiedFile);
    CloseHandle(sourceFile);
    FreeLibrary(library);

    return matchingIdentity.status == cue::runtime_host::detail::RuntimeModuleIdentityStatus::Match &&
                   copiedIdentity.status == cue::runtime_host::detail::RuntimeModuleIdentityStatus::Mismatch
               ? 0
               : 4;
}
