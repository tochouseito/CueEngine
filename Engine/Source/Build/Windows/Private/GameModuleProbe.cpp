#include <Cue/GameModule/GameModuleAbi.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>

namespace
{
/// @brief Probe Processの安定した終了Code
enum class ProbeExitCode : int
{
    Success = 0,
    InvalidArguments = 2,
    ModuleLoadFailed,
    EntryPointMissing,
    QueryRejected,
    ContractMismatch
};

/// @brief lower-case UUID Hex文字を4-bit値へ変換する
[[nodiscard]] std::optional<std::uint8_t> parse_nibble(wchar_t a_value) noexcept
{
    if (a_value >= L'0' && a_value <= L'9')
    {
        return static_cast<std::uint8_t>(a_value - L'0');
    }
    if (a_value >= L'a' && a_value <= L'f')
    {
        return static_cast<std::uint8_t>(a_value - L'a' + 10);
    }
    return std::nullopt;
}

/// @brief Canonical Project UUIDをABI比較用Byte列へ変換する
[[nodiscard]] std::optional<std::array<std::uint8_t, 16U>> parse_project_id(std::wstring_view a_text) noexcept
{
    if (a_text.size() != 36U || a_text[8] != L'-' || a_text[13] != L'-' || a_text[18] != L'-' || a_text[23] != L'-')
    {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16U> bytes{};
    std::size_t output = 0U;
    for (std::size_t index = 0U; index < a_text.size();)
    {
        if (a_text[index] == L'-')
        {
            ++index;
            continue;
        }
        if (index + 1U >= a_text.size() || output >= bytes.size())
        {
            return std::nullopt;
        }
        const std::optional<std::uint8_t> high = parse_nibble(a_text[index]);
        const std::optional<std::uint8_t> low = parse_nibble(a_text[index + 1U]);
        if (!high || !low)
        {
            return std::nullopt;
        }
        bytes[output++] = static_cast<std::uint8_t>((*high << 4U) | *low);
        index += 2U;
    }
    return output == bytes.size() ? std::optional<std::array<std::uint8_t, 16U>>(bytes) : std::nullopt;
}

/// @brief Build Configuration名をGame Module ABI値へ変換する
[[nodiscard]] std::optional<std::uint32_t> parse_configuration(std::wstring_view a_text) noexcept
{
    if (a_text == L"Debug")
    {
        return CUE_GAME_MODULE_CONFIGURATION_DEBUG;
    }
    if (a_text == L"Development")
    {
        return CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
    }
    if (a_text == L"Release")
    {
        return CUE_GAME_MODULE_CONFIGURATION_RELEASE;
    }
    return std::nullopt;
}

/// @brief Game ModuleのABI Table全体を期待契約と照合する
[[nodiscard]] bool validate_api(const CueGameModuleApiV1 &a_api, std::uint32_t a_configuration,
                                const std::array<std::uint8_t, 16U> &a_projectId) noexcept
{
    return a_api.structSize == sizeof(CueGameModuleApiV1) && a_api.version == CUE_GAME_MODULE_STRUCTURE_VERSION_1 &&
           a_api.abiVersion == CUE_GAME_MODULE_ABI_VERSION_1 && a_api.configuration == a_configuration &&
           a_api.architecture == CUE_GAME_MODULE_ARCHITECTURE_X64 && a_api.reserved == 0U &&
           a_api.projectId.structSize == sizeof(CueGameUuidV1) &&
           a_api.projectId.version == CUE_GAME_MODULE_STRUCTURE_VERSION_1 && a_api.createModule != nullptr &&
           a_api.registerSchemas != nullptr && a_api.registerComponents != nullptr &&
           a_api.registerSystems != nullptr && a_api.destroyModule != nullptr && a_api.reservedTail[0] == 0U &&
           a_api.reservedTail[1] == 0U && a_api.reservedTail[2] == 0U && a_api.reservedTail[3] == 0U &&
           std::equal(a_projectId.begin(), a_projectId.end(), a_api.projectId.bytes);
}
} // namespace

/// @brief Game ModuleをEditor外の短命ProcessへLoadしてABI契約を検証する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    if (a_argumentCount != 4)
    {
        return static_cast<int>(ProbeExitCode::InvalidArguments);
    }
    const std::optional<std::uint32_t> configuration = parse_configuration(a_arguments[2]);
    const std::optional<std::array<std::uint8_t, 16U>> projectId = parse_project_id(a_arguments[3]);
    if (!configuration || !projectId)
    {
        return static_cast<int>(ProbeExitCode::InvalidArguments);
    }

    const HMODULE module =
        LoadLibraryExW(a_arguments[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr)
    {
        return static_cast<int>(ProbeExitCode::ModuleLoadFailed);
    }
    const FARPROC address = GetProcAddress(module, "cue_game_module_query");
    if (address == nullptr)
    {
        FreeLibrary(module);
        return static_cast<int>(ProbeExitCode::EntryPointMissing);
    }
    using Query = CueGameModuleResult(CUE_GAME_MODULE_CALL *)(uint32_t, CueGameModuleQueryOutputV1 *,
                                                              CueGameModuleDiagnosticV1 *) noexcept;
    const Query query = std::bit_cast<Query>(address);
    CueGameModuleQueryOutputV1 output{sizeof(output), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, {0U, 0U}};
    CueGameModuleDiagnosticV1 diagnostic{sizeof(diagnostic),
                                         CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                         0U,
                                         0U,
                                         {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, 0U}};
    const CueGameModuleResult result = query(CUE_GAME_MODULE_ABI_VERSION_1, &output, &diagnostic);
    if (result != CUE_GAME_MODULE_RESULT_SUCCESS || output.structSize != sizeof(CueGameModuleQueryOutputV1) ||
        output.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 || output.api == nullptr || output.reserved[0] != 0U ||
        output.reserved[1] != 0U)
    {
        FreeLibrary(module);
        return static_cast<int>(ProbeExitCode::QueryRejected);
    }
    const bool valid = validate_api(*output.api, *configuration, *projectId);
    FreeLibrary(module);
    return static_cast<int>(valid ? ProbeExitCode::Success : ProbeExitCode::ContractMismatch);
}
