#include "HLSLCompiler.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Cue/Foundation/Windows/UtfConversion.h>

#include "DX12RenderDevice.h"

namespace cue::detail
{
namespace
{
/// @brief 公開 Stage を DXC の標準 Profile に変換する
const wchar_t* shader_profile(GpuShaderStage a_stage) noexcept
{
    switch (a_stage)
    {
    case GpuShaderStage::Vertex:
        return L"vs_6_0";
    case GpuShaderStage::Pixel:
        return L"ps_6_0";
    case GpuShaderStage::Compute:
        return L"cs_6_0";
    default:
        return nullptr;
    }
}
}

/// @brief DXC の初期化失敗を Pipeline 生成前に Result へ返す
Result<std::unique_ptr<HLSLCompiler>> HLSLCompiler::create()
{
    using CompilerResult = Result<std::unique_ptr<HLSLCompiler>>;
    auto compiler = std::make_unique<HLSLCompiler>();
    HRESULT result = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&compiler->m_utils));
    if (FAILED(result))
    {
        return CompilerResult::failure(gpu_error("DxcCreateInstance.Utils", result));
    }
    result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler->m_compiler));
    if (FAILED(result))
    {
        return CompilerResult::failure(gpu_error("DxcCreateInstance.Compiler", result));
    }
    result = compiler->m_utils->CreateDefaultIncludeHandler(&compiler->m_includeHandler);
    if (FAILED(result))
    {
        return CompilerResult::failure(gpu_error("IDxcUtils.CreateDefaultIncludeHandler", result));
    }
    return CompilerResult::success(std::move(compiler));
}

/// @brief Compile診断と Blob の取得を一つの失敗境界に閉じ込める
Result<Microsoft::WRL::ComPtr<IDxcBlob>> HLSLCompiler::compile_shader_raw(const GpuShaderDesc& a_desc) const
{
    using BlobResult = Result<Microsoft::WRL::ComPtr<IDxcBlob>>;
    const wchar_t* defaultProfile = shader_profile(a_desc.stage);
    const std::string prefix = a_desc.stage == GpuShaderStage::Vertex ? "vs_"
                               : a_desc.stage == GpuShaderStage::Pixel ? "ps_" : "cs_";
    if (!defaultProfile || (a_desc.source.empty() == a_desc.filePath.empty()) || a_desc.entry.empty() ||
        (!std::isalpha(static_cast<unsigned char>(a_desc.entry.front())) && a_desc.entry.front() != '_') ||
        a_desc.entry.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos ||
        (!a_desc.profile.empty() &&
         (a_desc.profile.starts_with(prefix) == false ||
          a_desc.profile.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)))
    {
        return BlobResult::failure({ErrorCategory::InvalidArgument, "HLSLCompiler.compile_shader_raw"});
    }
    const auto entry = std::wstring(a_desc.entry.begin(), a_desc.entry.end());
    const auto explicitProfile = std::wstring(a_desc.profile.begin(), a_desc.profile.end());
    const wchar_t* profile = a_desc.profile.empty() ? defaultProfile : explicitProfile.c_str();
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> loadedFile;
    std::wstring includeDirectory;
    DxcBuffer source{a_desc.source.data(), a_desc.source.size(), DXC_CP_UTF8};
    if (!a_desc.filePath.empty())
    {
        auto pathResult = utf8_to_utf16(a_desc.filePath);
        if (!pathResult.has_value())
        {
            return BlobResult::failure(*pathResult.try_error());
        }
        const auto path = pathResult.take_value();
        UINT32 codePage = DXC_CP_UTF8;
        const HRESULT load = m_utils->LoadFile(path.c_str(), &codePage, &loadedFile);
        if (FAILED(load))
        {
            return BlobResult::failure(gpu_error("IDxcUtils.LoadFile", load));
        }
        source = {loadedFile->GetBufferPointer(), loadedFile->GetBufferSize(), codePage};
        includeDirectory = std::filesystem::path(path).parent_path().wstring();
    }
    std::vector<LPCWSTR> arguments = {L"-E", entry.c_str(), L"-T", profile, L"-HV", L"2021"};
    if (!includeDirectory.empty())
    {
        arguments.insert(arguments.end(), {L"-I", includeDirectory.c_str()});
    }
    if (a_desc.enableDebugInfo)
    {
        arguments.insert(arguments.end(), {L"-Zi", L"-Qembed_debug", L"-Od"});
    }
    else
    {
        arguments.push_back(L"-O3");
    }
    Microsoft::WRL::ComPtr<IDxcResult> compilation;
    HRESULT result = m_compiler->Compile(&source, arguments.data(), static_cast<UINT32>(arguments.size()),
                                         m_includeHandler.Get(), IID_PPV_ARGS(&compilation));
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcCompiler3.Compile", result));
    }
    HRESULT status = S_OK;
    result = compilation->GetStatus(&status);
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcResult.GetStatus", result));
    }
    if (FAILED(status))
    {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        compilation->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        const std::string details = errors ? std::string(errors->GetStringPointer(), errors->GetStringLength())
                                            : std::string{};
        return BlobResult::failure({ErrorCategory::PlatformFailure, "DXC: " + details,
                                    static_cast<std::int64_t>(status)});
    }
    Microsoft::WRL::ComPtr<IDxcBlob> blob;
    result = compilation->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    if (FAILED(result))
    {
        return BlobResult::failure(gpu_error("IDxcResult.GetOutput", result));
    }
    return BlobResult::success(std::move(blob));
}
} // namespace cue::detail
