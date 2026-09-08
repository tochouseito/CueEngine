#include <Cue/GameModule/GameModuleAbi.h>

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
constexpr CueGameModuleApiV1 k_api = {sizeof(CueGameModuleApiV1),
                                      CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                      CUE_GAME_MODULE_ABI_VERSION_1,
                                      k_configuration,
                                      CUE_GAME_MODULE_ARCHITECTURE_X64,
                                      0U,
                                      k_projectId,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      {0U, 0U, 0U, 0U}};
} // namespace

/// @brief Test用Game Moduleの固定ABI Tableを返す
CUE_GAME_MODULE_EXTERN_C CUE_GAME_MODULE_EXPORT CueGameModuleResult CUE_GAME_MODULE_CALL
cue_game_module_query(uint32_t a_requestedAbiVersion, CueGameModuleQueryOutputV1 *a_output,
                      CueGameModuleDiagnosticV1 *) CUE_GAME_MODULE_NOEXCEPT
{
    if (a_requestedAbiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || a_output == nullptr ||
        a_output->structSize != sizeof(CueGameModuleQueryOutputV1) ||
        a_output->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    a_output->api = &k_api;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}
