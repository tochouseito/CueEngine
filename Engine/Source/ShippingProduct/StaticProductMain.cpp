#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/GameModule/GameModuleAbi.h>
#include <Cue/RuntimeHost/GameModuleQueryProvider.h>
#include <Cue/RuntimeHost/RuntimeHostProcess.h>
#include <Cue/RuntimeHost/RuntimeHostStartup.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>

#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

#ifndef CUE_GAME_PRODUCT_PROJECT_ID
#error CUE_GAME_PRODUCT_PROJECT_ID must identify the linked Game Module
#endif

namespace
{
constexpr std::string_view k_startupSceneAssetId = "70000000-0000-4000-8000-000000000001";

/// @brief Product Startup中の予期しない例外をProcess Fatal境界へ渡す
[[noreturn]] void terminate_product_startup(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Static Game Product startup allocation failed");
    std::abort();
}

/// @brief #303まで使用する固定空Sceneを独立Snapshotとして構築する
[[nodiscard]] cue::Result<cue::scene::SceneSnapshot> make_startup_scene(
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::scene::SceneAssetId> sceneId =
        cue::scene::SceneAssetId::parse(k_startupSceneAssetId, a_assertContext);
    if (!sceneId)
    {
        return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*sceneId.try_error()));
    }
    cue::scene::SceneDocument document =
        cue::scene::SceneDocument::create(std::move(*sceneId.try_value()), a_assertContext);
    return cue::scene::create_scene_snapshot(document, a_assertContext);
}

/// @brief Link済みQuery EntryからStatic Runtime Host Startupを構築する
[[nodiscard]] cue::Result<cue::runtime_host::RuntimeHostStartup> make_product_startup(
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        cue::Result<std::unique_ptr<cue::runtime_host::GameModuleQueryProvider>> provider =
            cue::runtime_host::create_static_game_module_query_provider(
                &cue_game_module_query, a_assertContext);
        if (!provider)
        {
            return cue::Result<cue::runtime_host::RuntimeHostStartup>::failure(
                std::move(*provider.try_error()));
        }
        auto identitySource = std::make_unique<cue::schema::SchemaRegistryIdentitySource>();
        cue::Result<cue::runtime_host::PreparedGameModule> prepared =
            cue::runtime_host::connect_game_module(
                **provider.try_value(), CUE_GAME_PRODUCT_PROJECT_ID, std::move(identitySource), a_assertContext);
        if (!prepared)
        {
            return cue::Result<cue::runtime_host::RuntimeHostStartup>::failure(
                std::move(*prepared.try_error()));
        }
        cue::Result<cue::scene::SceneSnapshot> scene = make_startup_scene(a_assertContext);
        if (!scene)
        {
            return cue::Result<cue::runtime_host::RuntimeHostStartup>::failure(
                std::move(*scene.try_error()));
        }
        return cue::Result<cue::runtime_host::RuntimeHostStartup>::success(
            cue::runtime_host::RuntimeHostStartup(
                std::move(*prepared.try_value()), std::move(*scene.try_value())));
    }
    catch (...)
    {
        terminate_product_startup(a_assertContext);
    }
}
} // namespace

/// @brief Static Game Moduleを選択して共通Runtime Host Processを開始する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    const cue::runtime_host::RuntimeHostProcessDescriptor descriptor = {
        &make_product_startup,
        true,
    };
    return cue::runtime_host::run_runtime_host_process(a_argumentCount, a_arguments, descriptor);
}
