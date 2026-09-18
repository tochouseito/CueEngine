#pragma once

#include <Cue/Renderer/Camera.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cue::renderer
{
/// @brief M22でRendererが解決できるEngine所有Mesh Identity
enum class RenderMesh : std::uint8_t
{
    Cube
};

/// @brief 一つのRuntimeまたはAuthoring Objectから抽出した描画Instance
struct RenderMeshInstance final
{
    math::Transform transform;
    RenderMesh mesh = RenderMesh::Cube;
};

/// @brief GameView用Main Camera選択結果
enum class MainCameraStatus : std::uint8_t
{
    Ready,
    Missing,
    Multiple,
    InvalidProjection
};

/// @brief World／Entity Pointerを保持しない一Frame分のPortable CPU Render Data
class RenderSnapshot final
{
  public:
    /// @brief 空の描画不能Snapshotを生成する
    RenderSnapshot() noexcept = default;
    /// @brief Main Camera選択結果、Camera、Mesh Instanceを所有する
    RenderSnapshot(MainCameraStatus a_cameraStatus, std::optional<PerspectiveCamera> a_mainCamera,
                   std::vector<RenderMeshInstance> a_meshes, std::uint64_t a_generation) noexcept;

    /// @brief Main Camera選択結果を返す
    [[nodiscard]] MainCameraStatus main_camera_status() const noexcept;
    /// @brief 描画可能なMain Cameraまたはnullptrを返す
    [[nodiscard]] const PerspectiveCamera *try_main_camera() const noexcept;
    /// @brief Stable抽出順のMesh Instance Viewを返す
    [[nodiscard]] std::span<const RenderMeshInstance> meshes() const noexcept;
    /// @brief Snapshot更新世代を返す
    [[nodiscard]] std::uint64_t generation() const noexcept;

  private:
    MainCameraStatus m_cameraStatus = MainCameraStatus::Missing;
    std::optional<PerspectiveCamera> m_mainCamera;
    std::vector<RenderMeshInstance> m_meshes;
    std::uint64_t m_generation = 0U;
};

/// @brief 同一Owner Threadで最新Render Snapshotを値所有するMailbox
class RenderSnapshotStore final
{
  public:
    /// @brief Snapshot所有権を最新値として置き換える
    void publish(RenderSnapshot a_snapshot) noexcept;
    /// @brief Runtime終了時に描画不能な空状態へ戻す
    void clear() noexcept;
    /// @brief 次のpublishまたはclearまで有効な最新Snapshot参照を返す
    [[nodiscard]] const RenderSnapshot &snapshot() const noexcept;

  private:
    RenderSnapshot m_snapshot;
};
} // namespace cue::renderer
