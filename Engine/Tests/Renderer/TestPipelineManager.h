#pragma once

#include <utility>
#include <vector>

#include <RHI/PipelineManager.h>

/// @brief Build の生成依頼、失敗と回収だけを Native API なしで観測する
class TestPipelineManager final : public cue::IPipelineManager
{
  public:
    int activeRoots = 0;
    int activeShaders = 0;
    int activePipelines = 0;
    int shaderCalls = 0;
    int failShaderCall = 0;
    bool shouldFailPipeline = false;
    std::vector<cue::RootSignatureDesc> roots;
    std::vector<cue::ShaderCompileDesc> shaders;
    std::vector<cue::GraphicsPipelineStateDesc> graphics;

    /// @brief Pass が指定した Root 設定を記録する
    [[nodiscard]] cue::Result<cue::RootSignatureHandle> create_root_signature(cue::RootSignatureDesc a_desc) override
    {
        roots.push_back(std::move(a_desc));
        ++activeRoots;
        return cue::Result<cue::RootSignatureHandle>::success({static_cast<std::uint32_t>(roots.size()), 1, 99});
    }
    /// @brief 指定した順序の Shader 生成だけを失敗させる
    [[nodiscard]] cue::Result<cue::ShaderBlobHandle> create_shader_blob(cue::ShaderCompileDesc a_desc) override
    {
        if (++shaderCalls == failShaderCall)
        {
            return cue::Result<cue::ShaderBlobHandle>::failure(
                {cue::ErrorCategory::PlatformFailure, "Test.shader.failure"});
        }
        shaders.push_back(std::move(a_desc));
        ++activeShaders;
        return cue::Result<cue::ShaderBlobHandle>::success({static_cast<std::uint32_t>(shaders.size()), 1, 99});
    }
    /// @brief Pass の Pipeline 設定を記録し、失敗時は生成数を増やさない
    [[nodiscard]] cue::Result<cue::PipelineStateHandle> create_graphics_pipeline(
        cue::GraphicsPipelineStateDesc a_desc) override
    {
        if (shouldFailPipeline)
        {
            return cue::Result<cue::PipelineStateHandle>::failure(
                {cue::ErrorCategory::PlatformFailure, "Test.pipeline.failure"});
        }
        graphics.push_back(std::move(a_desc));
        ++activePipelines;
        return cue::Result<cue::PipelineStateHandle>::success({static_cast<std::uint32_t>(graphics.size()), 1, 99});
    }
    /// @brief この Graphics 用テストでは Compute を受け付けない
    [[nodiscard]] cue::Result<cue::PipelineStateHandle> create_compute_pipeline(cue::ComputePipelineStateDesc) override
    {
        return cue::Result<cue::PipelineStateHandle>::failure(
            {cue::ErrorCategory::InvalidArgument, "Test.compute.unsupported"});
    }
    /// @brief Graph が生成した Handle の返却回数を観測する
    [[nodiscard]] cue::Result<void> retire(cue::RootSignatureHandle) override
    {
        if (activeRoots == 0)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.retire.empty"});
        }
        --activeRoots;
        return cue::Result<void>::success();
    }
    /// @brief Graph が生成した Handle の返却回数を観測する
    [[nodiscard]] cue::Result<void> retire(cue::ShaderBlobHandle) override
    {
        if (activeShaders == 0)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.retire.empty"});
        }
        --activeShaders;
        return cue::Result<void>::success();
    }
    /// @brief Graph が生成した Handle の返却回数を観測する
    [[nodiscard]] cue::Result<void> retire(cue::PipelineStateHandle) override
    {
        if (activePipelines == 0)
        {
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.retire.empty"});
        }
        --activePipelines;
        return cue::Result<void>::success();
    }
};
