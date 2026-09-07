#include <Cue/EditorCore/EditorController.h>
#include <Cue/EditorCore/EditorDocument.h>
#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/EditorCore/Error.h>
#include <Cue/EditorCore/FilesWorkspace.h>
#include <Cue/EditorCore/Persistence.h>
#include <Cue/EditorCore/SceneCommand.h>

#include <utility>

namespace
{
/// @brief 匿名 Braced Init で Factory Passkey を回避できるか検査する
template <typename T>
concept SupportsAnonymousConstructionKey =
    requires(cue::ProjectDescriptor &&a_descriptor, const cue::AssertContext &a_assertContext) {
        T({}, std::move(a_descriptor), a_assertContext);
    };

static_assert(!SupportsAnonymousConstructionKey<cue::editor_core::EditorController>);
} // namespace

/// @brief Cue.EditorCore Public Header が自己完結して Compile できることを検証する
int main()
{
    return 0;
}
