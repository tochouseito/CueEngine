#include <EditorHost/EditorHost.h>

#include <DX12/DX12Backend.h>
#include <DX12/DX12RenderDevice.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

#include "../Scene/SceneFixtures.h"

/// @brief Window 前の Preview 生成・Backend 失敗回収・成功後の寿命と Thread 分離を確認する
int main()
{
    for (const bool failBackend : {true, false})
    {
        auto scene = cue::Scene::parse(cue::tests::k_sceneText);
        if (!scene.has_value())
            return 1;
        cue::EditorHost *owner = nullptr;
        bool sawPreparedWorld = false;
        cue::EditorHostConfig config;
        config.window.title = "CueEngine Scene Integration Test";
        config.window.clientSize = {320, 180};
        config.initialScene = scene.take_value();
        config.createBackend = [&](cue::IFileSystem *a_files,
                                   cue::Clock &a_clock) -> cue::Result<std::unique_ptr<cue::IBackend>>
        {
            auto *world = owner->preview_world();
            const auto instance = owner->preview_instance();
            if (!world || !instance || !owner->scene())
                return cue::Result<std::unique_ptr<cue::IBackend>>::failure(
                    {cue::ErrorCategory::InvalidState, "Test.preview_missing"});
            auto entity = world->find_by_id(*instance, cue::tests::k_cubeId);
            if (entity.has_value())
            {
                auto transform = world->get_component<cue::TransformComponent>(*entity.try_value());
                sawPreparedWorld = transform.has_value() && !(*transform.try_value())->is_dirty() &&
                                   (*transform.try_value())->world_matrix().values[3][0] == 1;
            }
            if (failBackend)
                return cue::Result<std::unique_ptr<cue::IBackend>>::failure(
                    {cue::ErrorCategory::InvalidState, "Test.backend_failure"});
            auto backend = cue::dx12::DX12Backend::create(cue::dx12::AdapterSelection::Warp, {}, a_files, &a_clock);
            if (!backend.has_value())
                return cue::Result<std::unique_ptr<cue::IBackend>>::failure(*backend.try_error());
            return cue::Result<std::unique_ptr<cue::IBackend>>::success(backend.take_value());
        };
        cue::EditorHost host(std::move(config));
        owner = &host;
        auto initialized = host.initialize();
        if (!sawPreparedWorld || initialized.has_value() == failBackend)
            return 2;
        if (failBackend)
        {
            if (host.preview_world() || host.preview_instance())
                return 3;
        }
        else
        {
            if (!host.preview_world() || !host.preview_instance() || host.initialize().has_value())
                return 4;
            for (int index = 0; index < 3; ++index)
                if (!host.step().has_value())
                    return 5;
        }
        auto stopped = host.shutdown();
        if (host.preview_world() || host.preview_instance() || host.initialize().has_value())
            return 6;
        if (!failBackend && !stopped.has_value())
            return 7;
    }
    return 0;
}
