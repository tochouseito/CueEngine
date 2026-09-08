#include <Cue/GameModule/GameModuleAbi.h>

/// @brief C11 Translation Unitから公開ABI構造体を利用できることを確認する
int main(void)
{
    CueGameModuleQueryOutputV1 output = {0};
    output.structSize = (uint32_t)sizeof(output);
    output.version = CUE_GAME_MODULE_STRUCTURE_VERSION_1;
    return output.api == 0 ? 0 : 1;
}
