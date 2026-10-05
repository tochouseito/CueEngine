#include "DX12ShaderCompiler.h"

#include <filesystem>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

#include <Foundation/Windows/UtfConversion.h>

#include "DX12ShaderConfig.h"

namespace cue::dx12
{
namespace
{
/// @brief HRESULT と DXC の診断を所有する Error に変換する
Error compiler_error(std::string a_operation, HRESULT a_result)
{
    return {ErrorCategory::PlatformFailure, std::move(a_operation), static_cast<std::int64_t>(a_result)};
}
/// @brief 埋め込み NUL を Win32 Path や Compiler 引数へ渡さない
bool is_valid_text(const std::string &a_text)
{
    return !a_text.empty() && a_text.find('\0') == std::string::npos;
}
} // namespace

struct DX12ShaderCompiler::State final
{
    HMODULE module = nullptr;
    std::filesystem::path shaderDirectory;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includes;

    /// @brief DXC の COM Object を残したまま DLL を解放しない
    ~State()
    {
        includes.Reset();
        compiler.Reset();
        utils.Reset();
        if (module)
            FreeLibrary(module);
    }
};

/// @brief Factory が成功するまで空の状態を保持する
DX12ShaderCompiler::DX12ShaderCompiler() noexcept = default;
/// @brief Blob の Owner が先に参照を返却する契約で Compiler を解放する
DX12ShaderCompiler::~DX12ShaderCompiler() = default;

/// @brief 配置済み Runtime を優先し、開発環境では SDK の DXC を使う
Result<std::unique_ptr<DX12ShaderCompiler>> DX12ShaderCompiler::create()
{
    using compilerResult = Result<std::unique_ptr<DX12ShaderCompiler>>;
    try
    {
        auto result = std::make_unique<DX12ShaderCompiler>();
        result->m_state = std::make_unique<State>();
        auto &state = *result->m_state;
        std::wstring modulePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (length == 0 || length >= modulePath.size())
            return compilerResult::failure(compiler_error("GetModuleFileNameW", HRESULT_FROM_WIN32(GetLastError())));
        modulePath.resize(length);
        const auto directory = std::filesystem::path(modulePath).parent_path();
        auto libraryPath = directory / L"dxcompiler.dll";
        std::error_code fileError;
        if (!std::filesystem::exists(libraryPath, fileError))
            libraryPath = k_dxcLibraryPath;
        state.module = LoadLibraryExW(libraryPath.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!state.module)
            return compilerResult::failure(compiler_error("DXC.LoadLibraryExW", HRESULT_FROM_WIN32(GetLastError())));
        const auto createInstance =
            reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(state.module, "DxcCreateInstance"));
        if (!createInstance)
            return compilerResult::failure(compiler_error("DXC.GetProcAddress", HRESULT_FROM_WIN32(GetLastError())));
        HRESULT hr = createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&state.utils));
        if (SUCCEEDED(hr))
            hr = createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&state.compiler));
        if (SUCCEEDED(hr))
            hr = state.utils->CreateDefaultIncludeHandler(&state.includes);
        if (FAILED(hr))
            return compilerResult::failure(compiler_error("DXC.create", hr));
        state.shaderDirectory = directory / L"EngineResources" / L"Shader";
        if (!std::filesystem::exists(state.shaderDirectory, fileError))
            state.shaderDirectory = k_shaderSourceDirectory;
        return compilerResult::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        return compilerResult::failure({ErrorCategory::PlatformFailure, "DXC.create.allocation"});
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return compilerResult::failure({ErrorCategory::PlatformFailure, "DXC.create.path"});
    }
}

/// @brief Stage と Profile を照合し、Shader のコンパイル失敗を成功 Blob にしない
Result<Microsoft::WRL::ComPtr<IDxcBlob>> DX12ShaderCompiler::compile(const ShaderCompileDesc &a_desc)
{
    using blobResult = Result<Microsoft::WRL::ComPtr<IDxcBlob>>;
    const char *prefix = nullptr;
    switch (a_desc.stage)
    {
    case ShaderStage::Vertex:
        prefix = "vs_";
        break;
    case ShaderStage::Pixel:
        prefix = "ps_";
        break;
    case ShaderStage::Compute:
        prefix = "cs_";
        break;
    }
    if (!m_state || !prefix || !is_valid_text(a_desc.filePath) || !is_valid_text(a_desc.entryPoint) ||
        (!a_desc.targetProfile.empty() &&
         (!is_valid_text(a_desc.targetProfile) || !a_desc.targetProfile.starts_with(prefix))))
        return blobResult::failure({ErrorCategory::InvalidArgument, "DXC.compile.desc"});
    try
    {
        auto pathResult = utf8_to_utf16(a_desc.filePath);
        auto entryResult = utf8_to_utf16(a_desc.entryPoint);
        auto profileResult =
            utf8_to_utf16(a_desc.targetProfile.empty() ? std::string(prefix) + "6_0" : a_desc.targetProfile);
        if (!pathResult.has_value())
            return blobResult::failure(*pathResult.try_error());
        if (!entryResult.has_value())
            return blobResult::failure(*entryResult.try_error());
        if (!profileResult.has_value())
            return blobResult::failure(*profileResult.try_error());
        const auto path = m_state->shaderDirectory / std::filesystem::path(pathResult.take_value());
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
        HRESULT hr = m_state->utils->LoadFile(path.c_str(), nullptr, &source);
        if (FAILED(hr))
            return blobResult::failure(compiler_error("DXC.LoadFile: " + a_desc.filePath, hr));
        std::vector<std::wstring> arguments{path.wstring(),
                                            L"-E",
                                            entryResult.take_value(),
                                            L"-T",
                                            profileResult.take_value(),
                                            L"-I",
                                            path.parent_path().wstring(),
                                            L"-I",
                                            m_state->shaderDirectory.wstring(),
                                            L"-Ges"};
        for (const auto &define : a_desc.defines)
        {
            if (!is_valid_text(define))
                return blobResult::failure({ErrorCategory::InvalidArgument, "DXC.compile.define"});
            auto converted = utf8_to_utf16(define);
            if (!converted.has_value())
                return blobResult::failure(*converted.try_error());
            arguments.push_back(L"-D");
            arguments.push_back(converted.take_value());
        }
#if defined(_DEBUG) && !defined(CUE_SHIPPING)
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(L"-Od");
#else
        arguments.push_back(L"-O3");
#endif
        std::vector<LPCWSTR> pointers;
        for (const auto &argument : arguments)
            pointers.push_back(argument.c_str());
        DxcBuffer buffer{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8};
        Microsoft::WRL::ComPtr<IDxcResult> compiled;
        hr = m_state->compiler->Compile(&buffer, pointers.data(), static_cast<UINT32>(pointers.size()),
                                        m_state->includes.Get(), IID_PPV_ARGS(&compiled));
        if (FAILED(hr))
            return blobResult::failure(compiler_error("DXC.Compile", hr));
        HRESULT status = E_FAIL;
        hr = compiled->GetStatus(&status);
        if (FAILED(hr))
            return blobResult::failure(compiler_error("DXC.GetStatus", hr));
        if (FAILED(status))
        {
            Microsoft::WRL::ComPtr<IDxcBlobUtf8> diagnostics;
            std::string message = "DXC.Compile: " + a_desc.filePath;
            if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr)) && diagnostics &&
                diagnostics->GetStringLength() != 0)
                message += "\n" + std::string(diagnostics->GetStringPointer(), diagnostics->GetStringLength());
            return blobResult::failure(compiler_error(std::move(message), status));
        }
        Microsoft::WRL::ComPtr<IDxcBlob> blob;
        hr = compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
        if (FAILED(hr) || !blob || blob->GetBufferSize() == 0)
            return blobResult::failure(compiler_error("DXC.GetOutput", FAILED(hr) ? hr : E_FAIL));
        return blobResult::success(std::move(blob));
    }
    catch (const std::bad_alloc &)
    {
        return blobResult::failure({ErrorCategory::PlatformFailure, "DXC.compile.allocation"});
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return blobResult::failure({ErrorCategory::InvalidArgument, "DXC.compile.path"});
    }
}
} // namespace cue::dx12
