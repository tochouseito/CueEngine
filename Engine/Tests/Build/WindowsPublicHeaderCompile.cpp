#include <Cue/Build/Windows/WindowsArtifactPublisher.h>
#include <Cue/Build/Windows/WindowsProductSecurity.h>
#include <Cue/Build/Windows/WindowsToolchain.h>

#include <type_traits>

static_assert(std::is_polymorphic_v<cue::BuildArtifactPublisher>);
static_assert(!std::is_copy_constructible_v<cue::BuildWorkspaceLease>);

/// @brief Cue.Build.Windows公開Headerを単独ConsumerとしてCompileできるか検証する
int main()
{
    return 0;
}
