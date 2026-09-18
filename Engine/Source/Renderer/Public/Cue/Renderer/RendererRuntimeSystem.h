#pragma once

#include <Cue/Renderer/RenderSnapshot.h>
#include <Cue/Runtime/RuntimeSystemFactory.h>

namespace cue::renderer
{
/// @brief SessionごとにCamera／Mesh Runtime Bindingと抽出Systemを生成するProject Scope Factory
class RendererRuntimeSystemFactory final : public runtime::RuntimeSystemFactory
{
  public:
    /// @brief 最新SnapshotのSession外Ownerを非所有で保持する
    explicit RendererRuntimeSystemFactory(RenderSnapshotStore &a_snapshotStore) noexcept;

    /// @brief Session-local SystemとCamera／Mesh Builder Factoryを一組生成する
    [[nodiscard]] Result<runtime::RuntimeSystemRegistration> create_system(
        const AssertContext &a_assertContext) const noexcept override;

  private:
    RenderSnapshotStore *m_snapshotStore;
};
} // namespace cue::renderer
