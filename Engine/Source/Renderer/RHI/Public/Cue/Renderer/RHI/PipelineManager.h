#pragma once

#include <Cue/Renderer/RHI/GpuPipelines.h>

#include <string_view>

namespace cue
{
/// @brief Shader、Root Signature、Graphics／Compute PSO の管理契約
/// @details Backend が所有し、生成した Handle は依存する PSO と GPU 作業の完了まで維持する
class IPipelineManager : public IGpuPipelines
{
public:
    ~IPipelineManager() override = default;

    /// @brief 名前付き Shader の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuShaderHandle> get_shader(std::string_view a_name) const = 0;

    /// @brief 名前付き Root Signature の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuRootSignatureHandle> get_root_signature(std::string_view a_name) const = 0;

    /// @brief 名前付き Graphics PSO の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuPipelineHandle> get_graphics_pipeline(std::string_view a_name) const = 0;

    /// @brief 名前付き Compute PSO の現行 Handle を返す
    [[nodiscard]] virtual Result<GpuPipelineHandle> get_compute_pipeline(std::string_view a_name) const = 0;
};
} // namespace cue
