#pragma once

#include "D3D12Presentation.h"
#include "D3D12PipelineLibrary.h"
#include "D3D12ResourcePool.h"

#include <Cue/Renderer/RHI/GpuCommands.h>

#include <array>
#include <memory>

namespace cue::detail
{
/// @brief 固定 Mesh Pass の Shader、Root Signature、PSO と CBV を所有して再利用する
class D3D12PipelineCache final
{
public:
    /// @brief Shader のコンパイルと PSO 構築を完了してから公開する
    [[nodiscard]] static Result<std::unique_ptr<D3D12PipelineCache>> create(D3D12DeviceContext& a_device,
                                                                              D3D12ResourcePool& a_resources,
                                                                              D3D12PipelineLibrary& a_library);

    /// @brief 固定 Mesh の View と定数 Buffer を Resource Owner へ返す
    ~D3D12PipelineCache();

    /// @brief 対応 Frame Slot の Fence 完了後に定数を書き換えて Pipeline を設定する
    [[nodiscard]] Result<void> bind(IGpuCommandRecorder& a_commands, UINT a_slot,
                                     const std::array<float, 4>& a_tint);

private:
    D3D12PipelineLibrary* m_library = nullptr;
    GpuShaderHandle m_vertex;
    GpuShaderHandle m_pixel;
    GpuRootSignatureHandle m_root;
    GpuPipelineHandle m_pipeline;
    D3D12ResourcePool* m_resources = nullptr;
    GpuResourceHandle m_constants;
    std::array<GpuViewHandle, k_backBufferCount> m_cbvs{};
};
} // namespace cue::detail
