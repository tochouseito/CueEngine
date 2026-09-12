#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#if defined(CUE_TEST_PRODUCT_D3D12_ORDINAL_IMPORT)
#include <d3d12.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifndef CUE_TEST_PRODUCT_PROJECT_ID
#error CUE_TEST_PRODUCT_PROJECT_ID must identify the probe product
#endif

#if defined(CUE_TEST_PRODUCT_PACKAGED_LOADER_IMPORT)
/// @brief Security Test用にPackaged Loader Importだけを最終PEへ残す
void exercise_packaged_loader_import() noexcept
{
    static_cast<void>(LoadPackagedLibrary(L"kernel32.dll", 0U));
}
#endif

#ifndef CUE_TEST_PRODUCT_CONFIGURATION
#error CUE_TEST_PRODUCT_CONFIGURATION must identify the probe product configuration
#endif

#if defined(CUE_TEST_PRODUCT_D3D12_ORDINAL_IMPORT)
extern "C" decltype(&D3D12CreateDevice) __imp_D3D12CreateDevice;
#endif

namespace
{
constexpr std::string_view k_marker = "CueGameProductProbe:v1\n";

#if defined(CUE_TEST_PRODUCT_D3D12_ORDINAL_IMPORT)
constexpr std::size_t k_guardMetadataStrideReserveBytes = 1024U;

struct alignas(8) GuardMetadataStrideReserve final
{
    std::array<std::uint8_t, 16> begin;
    std::array<std::uint8_t, k_guardMetadataStrideReserveBytes> storage;
    std::array<std::uint8_t, 16> end;
};

/// @brief Guard Tableの可変Stride Test用Read-only領域を生成する
[[nodiscard]] consteval GuardMetadataStrideReserve make_guard_metadata_stride_reserve() noexcept
{
    GuardMetadataStrideReserve reserve{
        {'C', 'u', 'e', 'G', 't', 'S', 't', 'r', 'i', 'd', 'e', 'B', 'e', 'g', 'i', 'n'},
        {},
        {'C', 'u', 'e', 'G', 't', 'S', 't', 'r', 'i', 'd', 'e', 'E', 'n', 'd', '!', '!'},
    };
    for (std::size_t index = 0U; index < reserve.storage.size(); ++index)
    {
        reserve.storage[index] = static_cast<std::uint8_t>(index % 251U + 1U);
    }
    return reserve;
}

#pragma section(".cuegt", read)
__declspec(allocate(".cuegt")) const GuardMetadataStrideReserve k_guardMetadataStrideReserve =
    make_guard_metadata_stride_reserve();

/// @brief Security Test用にD3D12の序数Importを最終PEへ残す
void exercise_d3d12_ordinal_import() noexcept
{
    const std::size_t reserveIndex = GetCurrentProcessId() % k_guardMetadataStrideReserve.storage.size();
    const volatile std::uint8_t reserveByte = k_guardMetadataStrideReserve.storage[reserveIndex];
    static_cast<void>(reserveByte);
    decltype(&D3D12CreateDevice) const volatile createDevice = __imp_D3D12CreateDevice;
    decltype(&GetCurrentProcessId) const volatile currentProcessId = &GetCurrentProcessId;
    decltype(&GetTickCount64) const volatile tickCount = &GetTickCount64;
    ID3D12Device *device = nullptr;
    const volatile HRESULT result =
        createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&device));
    static_cast<void>(currentProcessId());
    static_cast<void>(tickCount());
    if (device != nullptr)
    {
        device->Release();
    }
    static_cast<void>(result);
}
#endif

#if defined(CUE_TEST_PRODUCT_LOADER_IMPORT)
/// @brief Security Test用に禁止対象Loader Importを最終PEへ残す
void exercise_loader_import() noexcept
{
    HMODULE module = LoadLibraryExW(L"kernel32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module != nullptr)
    {
        FreeLibrary(module);
    }
}
#endif

/// @brief UTF-16 ArgumentがASCII Compile-time Identityと一致するか判定する
[[nodiscard]] bool matches_ascii(std::wstring_view a_value, std::string_view a_expected) noexcept
{
    if (a_value.size() != a_expected.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_expected.size(); ++index)
    {
        if (a_value[index] != static_cast<wchar_t>(a_expected[index]))
        {
            return false;
        }
    }
    return true;
}

/// @brief Publisher専用Protocolを検証して固定完了Markerを耐久書込みする
[[nodiscard]] int run_probe(int a_argumentCount, wchar_t **a_arguments) noexcept
{
#if defined(CUE_TEST_PRODUCT_D3D12_ORDINAL_IMPORT)
    exercise_d3d12_ordinal_import();
#endif
#if defined(CUE_TEST_PRODUCT_LOADER_IMPORT)
    exercise_loader_import();
#endif
#if defined(CUE_TEST_PRODUCT_PACKAGED_LOADER_IMPORT)
    exercise_packaged_loader_import();
#endif
    if (a_argumentCount != 4 || std::wstring_view(a_arguments[1]) != L"--cue-artifact-probe" ||
        !matches_ascii(a_arguments[2], CUE_TEST_PRODUCT_CONFIGURATION) ||
        !matches_ascii(a_arguments[3], CUE_TEST_PRODUCT_PROJECT_ID))
    {
        return 1;
    }
    HANDLE marker =
        CreateFileW(L".probe-complete", GENERIC_WRITE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE)
    {
        return 2;
    }
    DWORD written = 0U;
    const BOOL writeSucceeded =
        WriteFile(marker, k_marker.data(), static_cast<DWORD>(k_marker.size()), &written, nullptr);
    const BOOL flushSucceeded = writeSucceeded != FALSE ? FlushFileBuffers(marker) : FALSE;
    const BOOL closeSucceeded = CloseHandle(marker);
    if (writeSucceeded == FALSE || written != k_marker.size() || flushSucceeded == FALSE || closeSucceeded == FALSE)
    {
        return 3;
    }
#if defined(CUE_TEST_PRODUCT_EXTRA_FILE)
    HANDLE unexpected =
        CreateFileW(L"unexpected.dll", GENERIC_WRITE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (unexpected == INVALID_HANDLE_VALUE)
    {
        return 4;
    }
    constexpr std::string_view unexpectedBytes = "probe-created-file";
    written = 0U;
    const BOOL unexpectedWrite =
        WriteFile(unexpected, unexpectedBytes.data(), static_cast<DWORD>(unexpectedBytes.size()), &written, nullptr);
    const BOOL unexpectedFlush = unexpectedWrite != FALSE ? FlushFileBuffers(unexpected) : FALSE;
    const BOOL unexpectedClose = CloseHandle(unexpected);
    if (unexpectedWrite == FALSE || written != unexpectedBytes.size() || unexpectedFlush == FALSE ||
        unexpectedClose == FALSE)
    {
        return 5;
    }
#endif
    return 0;
}
} // namespace

/// @brief Shipping Product Artifact Probe Fixtureを実行する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    return run_probe(a_argumentCount, a_arguments);
}
