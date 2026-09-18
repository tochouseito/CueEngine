#include <Cue/Renderer/RenderSnapshot.h>

#include <utility>

namespace cue::renderer
{
RenderSnapshot::RenderSnapshot(MainCameraStatus a_cameraStatus, std::optional<PerspectiveCamera> a_mainCamera,
                               std::vector<RenderMeshInstance> a_meshes, std::uint64_t a_generation) noexcept
    : m_cameraStatus(a_cameraStatus), m_mainCamera(std::move(a_mainCamera)), m_meshes(std::move(a_meshes)),
      m_generation(a_generation)
{
}

MainCameraStatus RenderSnapshot::main_camera_status() const noexcept
{
    return m_cameraStatus;
}

const PerspectiveCamera *RenderSnapshot::try_main_camera() const noexcept
{
    return m_mainCamera ? &*m_mainCamera : nullptr;
}

std::span<const RenderMeshInstance> RenderSnapshot::meshes() const noexcept
{
    return m_meshes;
}

std::uint64_t RenderSnapshot::generation() const noexcept
{
    return m_generation;
}

void RenderSnapshotStore::publish(RenderSnapshot a_snapshot) noexcept
{
    m_snapshot = std::move(a_snapshot);
}

void RenderSnapshotStore::clear() noexcept
{
    m_snapshot = RenderSnapshot{};
}

const RenderSnapshot &RenderSnapshotStore::snapshot() const noexcept
{
    return m_snapshot;
}
} // namespace cue::renderer
