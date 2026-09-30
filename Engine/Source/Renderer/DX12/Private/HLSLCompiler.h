#pragma once

#include "DX12RenderDevice.h"

#include <Cue/Renderer/RHI/GpuPipelines.h>

#include <dxcapi.h>
#include <wrl/client.h>

#include <memory>

namespace cue::detail
{
/// @brief DXC の Compiler と Utils を所有し、Shader Blob を Pipeline Manager へ返す
class HLSLCompiler final
{
public:
    /// @brief DXC Service の設定前に空の Owner を作る
    HLSLCompiler() = default;

    /// @brief DXC の二つの Service を生成してから公開する
    [[nodiscard]] static Result<std::unique_ptr<HLSLCompiler>> create();

    /// @brief UTF-8 HLSL Source を Stage に適合する Profile で Compile する
    [[nodiscard]] Result<Microsoft::WRL::ComPtr<IDxcBlob>> compile_shader_raw(const GpuShaderDesc& a_desc) const;

private:
    Microsoft::WRL::ComPtr<IDxcUtils> m_utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> m_compiler;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> m_includeHandler;
};
} // namespace cue::detail
