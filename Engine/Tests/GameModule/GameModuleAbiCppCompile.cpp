#include <Cue/GameModule/GameModuleAbi.h>

#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout_v<CueGameModuleApiV1>);
static_assert(std::is_trivially_copyable_v<CueGameModuleApiV1>);
static_assert(sizeof(CueGameUuidV1::bytes) == 16U);

/// @brief C++20 Translation UnitからC互換公開ABI構造体を利用できることを確認する
int main()
{
    CueGameModuleQueryOutputV1 output{};
    output.structSize = static_cast<std::uint32_t>(sizeof(output));
    output.version = CUE_GAME_MODULE_STRUCTURE_VERSION_1;
    return output.api == nullptr ? 0 : 1;
}
