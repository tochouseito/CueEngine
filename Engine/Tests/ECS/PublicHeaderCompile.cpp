#include <ECS/ECSManager.h>

#include <type_traits>

#if defined(_WINDOWS_) || defined(__d3d12_h__)
#error ECS 公開 Header は Windows / DX12 を要求してはならない
#endif

/// @brief ECS の公開契約を Platform・Runtime・Math・Renderer なしで利用できることを確認する
int main()
{
    static_assert(std::is_same_v<cue::ecs::Entity, std::uint32_t>);
    static_assert(cue::ecs::Archetype{}.size() == 256);
    cue::ecs::ECSManager ecs;
    const auto entity = ecs.generate_entity();
    return ecs.is_entity_active(entity) ? 0 : 1;
}
