#pragma once

#include "DX12SwapChain.h"
#include "DX12PipelineManager.h"
#include "DX12ResourcePool.h"

#include <Cue/Renderer/RHI/GpuCommands.h>

#include <array>
#include <memory>

namespace cue::detail
{
/// @brief 固定 Mesh Pass の Shader、Root Signature、PSO と CBV を所有して再利用する
class DX12PipelineCache final
{
public:
    /// @brief Shader のコンパイルと PSO 構築を完了してから公開する
    [[nodiscard]] static Result<std::unique_ptr<DX12PipelineCache>> create(DX12RenderDevice& a_device,
                                                                              DX12ResourcePool& a_resources,
                                                                              DX12PipelineManager& a_library);

    /// @brief 固定 Mesh の View と定数 Buffer を Resource Owner へ返す
    ~DX12PipelineCache();

    /// @brief 対応 Frame Slot の Fence 完了後に定数を書き換えて Pipeline を設定する
    [[nodiscard]] Result<void> bind(IGpuCommandRecorder& a_commands, UINT a_slot,
                                     const std::array<float, 4>& a_tint);

private:
    DX12PipelineManager* m_library = nullptr;
    GpuShaderHandle m_vertex;
    GpuShaderHandle m_pixel;
    GpuRootSignatureHandle m_root;
    GpuPipelineHandle m_pipeline;
    DX12ResourcePool* m_resources = nullptr;
    GpuResourceHandle m_constants;
    std::array<GpuViewHandle, k_backBufferCount> m_cbvs{};
};
} // namespace cue::detail
