#pragma once

#include <memory>

#include <Windows.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <RHI/PipelineManager.h>

namespace cue
{
class IFileSystem;
}

namespace cue::dx12
{
/// @brief DXC の Module と Compiler を所有し、Blob の所有者より長く維持する
class DX12ShaderCompiler final
{
  public:
    /// @brief create 内部で空の状態を構築する
    DX12ShaderCompiler() noexcept;
    /// @brief DXC の Module、Utils と Compiler を生成する
    /// 指定 FileSystem は Compiler より長く生存させる。未指定なら内部所有する
    [[nodiscard]] static Result<std::unique_ptr<DX12ShaderCompiler>> create(IFileSystem *a_files);
    /// @brief COM 参照を先に解放してから Module を解放する
    ~DX12ShaderCompiler();
    /// @brief Module の所有を複製しない
    DX12ShaderCompiler(const DX12ShaderCompiler &) = delete;
    /// @brief Module の所有を複製しない
    DX12ShaderCompiler &operator=(const DX12ShaderCompiler &) = delete;
    /// @brief UTF-8 の設定を検証し、失敗した DXC 診断を Error に保持する
    [[nodiscard]] Result<Microsoft::WRL::ComPtr<IDxcBlob>> compile(const ShaderCompileDesc &a_desc);

  private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace cue::dx12
