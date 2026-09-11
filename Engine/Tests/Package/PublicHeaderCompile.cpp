#include <Cue/Package/Error.h>
#include <Cue/Package/Manifest.h>
#include <Cue/Package/Publisher.h>
#include <Cue/Package/RuntimeData.h>

/// @brief Cue.Package公開Headerが自己完結してCompileできるか検証する
int main()
{
    return cue::package::k_packageManifestSchemaVersion == 1U &&
                   cue::package::k_monolithicPackageManifestSchemaVersion == 2U &&
                   cue::package::k_runtimeProjectDataSchemaVersion == 1U &&
                   cue::package::k_runtimeSceneDataSchemaVersion == 1U &&
                   cue::package::PackagePublishOutcome::Committed != cue::package::PackagePublishOutcome::NotPublished
               ? 0
               : 1;
}
