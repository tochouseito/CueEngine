#include <Cue/Package/Error.h>
#include <Cue/Package/RuntimeData.h>

/// @brief Cue.Package公開Headerが自己完結してCompileできるか検証する
int main()
{
    return cue::package::k_runtimeProjectDataSchemaVersion == 1U && cue::package::k_runtimeSceneDataSchemaVersion == 1U
               ? 0
               : 1;
}
