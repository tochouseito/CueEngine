#include <Cue/GameModule/GameModuleAbi.h>

#if defined(CUE_TEST_CRASH_QUERY) || defined(CUE_TEST_HANG_QUERY)
#include <Windows.h>
#endif

#include <cstdint>

namespace
{
#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEBUG;
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_RELEASE;
#else
#error CUE_TEST_BUILD_CONFIGURATION must identify a supported configuration
#endif

constexpr CueGameUuidV1 k_projectId = {
    sizeof(CueGameUuidV1),
    CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    {0x41U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0x4cU, 0xdeU, 0x8fU, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU}};

/// @brief Test用Module Handleを一つだけ生成する
CueGameModuleResult CUE_GAME_MODULE_CALL create_module(CueGameModuleHandle *a_module,
                                                       CueGameModuleDiagnosticV1 *) noexcept
{
    static std::uint8_t moduleState = 0U;
    if (a_module == nullptr || *a_module != nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    *a_module = &moduleState;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Test用Descriptor集合を空の登録成功として返す
CueGameModuleResult CUE_GAME_MODULE_CALL register_empty(CueGameModuleHandle a_module,
                                                        const CueGameRegistrationSinkV1 *a_sink,
                                                        CueGameModuleDiagnosticV1 *) noexcept
{
    return a_module != nullptr && a_sink != nullptr ? CUE_GAME_MODULE_RESULT_SUCCESS
                                                    : CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
}

/// @brief Test用Module Handleの非所有参照を終了する
void CUE_GAME_MODULE_CALL destroy_module(CueGameModuleHandle) noexcept
{
}

constexpr CueGameModuleApiV1 k_api = {sizeof(CueGameModuleApiV1),
                                      CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                      CUE_GAME_MODULE_ABI_VERSION_1,
                                      k_configuration,
                                      CUE_GAME_MODULE_ARCHITECTURE_X64,
                                      0U,
                                      k_projectId,
#if defined(CUE_TEST_INVALID_API)
                                      nullptr,
#else
                                      &create_module,
#endif
                                      &register_empty,
                                      &register_empty,
                                      &register_empty,
                                      &destroy_module,
#if defined(CUE_TEST_INVALID_API)
                                      {1U, 0U, 0U, 0U}};
#else
                                      {0U, 0U, 0U, 0U}};
#endif
} // namespace

/// @brief Test用Game Moduleの固定ABI Tableを返す
CUE_GAME_MODULE_EXTERN_C CUE_GAME_MODULE_EXPORT CueGameModuleResult CUE_GAME_MODULE_CALL
cue_game_module_query(uint32_t a_requestedAbiVersion, CueGameModuleQueryOutputV1 *a_output,
                      CueGameModuleDiagnosticV1 *) CUE_GAME_MODULE_NOEXCEPT
{
    static_cast<void>(a_requestedAbiVersion);
    static_cast<void>(a_output);
#if defined(CUE_TEST_CRASH_QUERY)
    static_cast<void>(TerminateProcess(GetCurrentProcess(), 0xc0000409U));
    for (;;)
    {
        Sleep(1000U);
    }
#elif defined(CUE_TEST_HANG_QUERY)
    for (;;)
    {
        Sleep(1000U);
    }
#else
    if (a_requestedAbiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || a_output == nullptr ||
        a_output->structSize != sizeof(CueGameModuleQueryOutputV1) ||
        a_output->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    a_output->api = &k_api;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
#endif
}
