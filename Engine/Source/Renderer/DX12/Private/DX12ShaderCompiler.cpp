#include "DX12ShaderCompiler.h"

#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <wrl/implements.h>

#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Windows/WindowsFileSystem.h>

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

/// @brief DXC の Include 候補を注入された FileSystem から取得する
class FileIncludeHandler final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IDxcIncludeHandler>
{
  public:
    /// @brief 前回 Compile の File 診断を破棄する
    void clear_error() noexcept
    {
        m_lastError.reset();
    }
    /// @brief 同期 Compile の間だけ保持する File 診断を借用する
    const std::optional<Error> &last_error() const noexcept
    {
        return m_lastError;
    }
    /// @brief Compiler State の FileSystem と Utils を同期 Compile 中だけ借用する
    void initialize(IFileSystem &a_files, IDxcUtils &a_utils) noexcept
    {
        m_files = &a_files;
        m_utils = &a_utils;
    }
    /// @brief Include 候補の内容を所有 Blob へ複製し、例外を COM 境界から出さない
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR a_filename, IDxcBlob **a_source) override
    {
        if (!a_filename || !a_source)
        {
            return E_INVALIDARG;
        }
        *a_source = nullptr;
        try
        {
            auto text = utf16_to_utf8(a_filename);
            if (!text.has_value())
            {
                m_lastError = *text.try_error();
                return E_INVALIDARG;
            }
            auto path = Path::create(*text.try_value());
            if (!path.has_value())
            {
                m_lastError = *path.try_error();
                return E_INVALIDARG;
            }
            auto bytes = m_files->read_all(*path.try_value());
            if (!bytes.has_value())
            {
                m_lastError = *bytes.try_error();
                const auto code = m_lastError->nativeCode;
                return m_lastError->category == ErrorCategory::PlatformFailure && code > 0 && code <= MAXDWORD
                           ? HRESULT_FROM_WIN32(static_cast<DWORD>(code))
                           : E_FAIL;
            }
            Microsoft::WRL::ComPtr<IDxcBlobEncoding> blob;
            const auto &data = *bytes.try_value();
            const char emptySource = '\0';
            const auto hr = m_utils->CreateBlob(data.empty() ? static_cast<const void *>(&emptySource) : data.data(),
                                                static_cast<UINT32>(data.size()), DXC_CP_UTF8, &blob);
            if (SUCCEEDED(hr))
            {
                *a_source = blob.Detach();
                // 成功候補が見つかった後は Search 中の未存在を最終 Compile Error に混ぜない
                m_lastError.reset();
            }
            return hr;
        }
        catch (const std::bad_alloc &)
        {
            return E_OUTOFMEMORY;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

  private:
    std::optional<Error> m_lastError;
    IFileSystem *m_files = nullptr;
    IDxcUtils *m_utils = nullptr;
};
} // namespace

struct DX12ShaderCompiler::State final
{
    HMODULE module = nullptr;
    std::unique_ptr<IFileSystem> ownedFiles;
    IFileSystem *files = nullptr;
    Path shaderDirectory;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    Microsoft::WRL::ComPtr<FileIncludeHandler> includes;

    /// @brief DXC の COM Object を残したまま DLL を解放しない
    ~State()
    {
        includes.Reset();
        compiler.Reset();
        utils.Reset();
        if (module)
        {
            FreeLibrary(module);
        }
    }
};

/// @brief Factory が成功するまで空の状態を保持する
DX12ShaderCompiler::DX12ShaderCompiler() noexcept = default;
/// @brief Blob の Owner が先に参照を返却する契約で Compiler を解放する
DX12ShaderCompiler::~DX12ShaderCompiler() = default;

/// @brief 配置済み Runtime を優先し、開発環境では SDK の DXC を使う
Result<std::unique_ptr<DX12ShaderCompiler>> DX12ShaderCompiler::create(IFileSystem *a_files)
{
    using compilerResult = Result<std::unique_ptr<DX12ShaderCompiler>>;
    try
    {
        auto result = std::make_unique<DX12ShaderCompiler>();
        result->m_state = std::make_unique<State>();
        auto &state = *result->m_state;
        if (!a_files)
        {
            auto files = create_windows_file_system();
            if (!files.has_value())
            {
                return compilerResult::failure(*files.try_error());
            }
            state.ownedFiles = files.take_value();
            a_files = state.ownedFiles.get();
        }
        state.files = a_files;
        auto directory = a_files->executable_directory();
        if (!directory.has_value())
        {
            return compilerResult::failure(*directory.try_error());
        }
        auto library = directory.try_value()->join("dxcompiler.dll");
        if (!library.has_value())
        {
            return compilerResult::failure(*library.try_error());
        }
        auto exists = a_files->exists(*library.try_value());
        if (!exists.has_value())
        {
            return compilerResult::failure(*exists.try_error());
        }
        auto libraryPath = utf8_to_utf16(library.try_value()->utf8());
        if (!libraryPath.has_value())
        {
            return compilerResult::failure(*libraryPath.try_error());
        }
        if (!*exists.try_value())
        {
            libraryPath = Result<std::wstring>::success(k_dxcLibraryPath);
        }
        state.module = LoadLibraryExW(libraryPath.try_value()->c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!state.module)
        {
            return compilerResult::failure(compiler_error("DXC.LoadLibraryExW", HRESULT_FROM_WIN32(GetLastError())));
        }
        const auto createInstance =
            reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(state.module, "DxcCreateInstance"));
        if (!createInstance)
        {
            return compilerResult::failure(compiler_error("DXC.GetProcAddress", HRESULT_FROM_WIN32(GetLastError())));
        }
        HRESULT hr = createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&state.utils));
        if (SUCCEEDED(hr))
        {
            hr = createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&state.compiler));
        }
        if (FAILED(hr))
        {
            return compilerResult::failure(compiler_error("DXC.create", hr));
        }
        state.includes = Microsoft::WRL::Make<FileIncludeHandler>();
        if (!state.includes)
        {
            return compilerResult::failure(compiler_error("DXC.IncludeHandler", E_OUTOFMEMORY));
        }
        state.includes->initialize(*state.files, *state.utils.Get());
        auto shaderDirectory = directory.try_value()->join("EngineResources/Shader");
        if (!shaderDirectory.has_value())
        {
            return compilerResult::failure(*shaderDirectory.try_error());
        }
        auto shaderExists = a_files->exists(*shaderDirectory.try_value());
        if (!shaderExists.has_value())
        {
            return compilerResult::failure(*shaderExists.try_error());
        }
        if (!*shaderExists.try_value())
        {
            auto sourceDirectory = utf16_to_utf8(k_shaderSourceDirectory);
            if (!sourceDirectory.has_value())
            {
                return compilerResult::failure(*sourceDirectory.try_error());
            }
            shaderDirectory = Path::create(*sourceDirectory.try_value());
            if (!shaderDirectory.has_value())
            {
                return compilerResult::failure(*shaderDirectory.try_error());
            }
        }
        state.shaderDirectory = shaderDirectory.take_value();
        return compilerResult::success(std::move(result));
    }
    catch (const std::bad_alloc &)
    {
        return compilerResult::failure({ErrorCategory::PlatformFailure, "DXC.create.allocation"});
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
    {
        return blobResult::failure({ErrorCategory::InvalidArgument, "DXC.compile.desc"});
    }
    try
    {
        auto pathResult = utf8_to_utf16(a_desc.filePath);
        auto entryResult = utf8_to_utf16(a_desc.entryPoint);
        auto profileResult =
            utf8_to_utf16(a_desc.targetProfile.empty() ? std::string(prefix) + "6_0" : a_desc.targetProfile);
        if (!pathResult.has_value())
        {
            return blobResult::failure(*pathResult.try_error());
        }
        if (!entryResult.has_value())
        {
            return blobResult::failure(*entryResult.try_error());
        }
        if (!profileResult.has_value())
        {
            return blobResult::failure(*profileResult.try_error());
        }
        auto path = m_state->shaderDirectory.join(a_desc.filePath);
        if (!path.has_value())
        {
            return blobResult::failure(*path.try_error());
        }
        auto source = m_state->files->read_all(*path.try_value());
        if (!source.has_value())
        {
            return blobResult::failure(*source.try_error());
        }
        auto fullPath = utf8_to_utf16(path.try_value()->utf8());
        auto parentPath = utf8_to_utf16(path.try_value()->parent().utf8());
        auto shaderPath = utf8_to_utf16(m_state->shaderDirectory.utf8());
        if (!fullPath.has_value() || !parentPath.has_value() || !shaderPath.has_value())
        {
            return blobResult::failure(!fullPath.has_value()     ? *fullPath.try_error()
                                       : !parentPath.has_value() ? *parentPath.try_error()
                                                                 : *shaderPath.try_error());
        }
        std::vector<std::wstring> arguments{fullPath.take_value(),      L"-E",  entryResult.take_value(), L"-T",
                                            profileResult.take_value(), L"-I",  parentPath.take_value(),  L"-I",
                                            shaderPath.take_value(),    L"-Ges"};
        for (const auto &define : a_desc.defines)
        {
            if (!is_valid_text(define))
            {
                return blobResult::failure({ErrorCategory::InvalidArgument, "DXC.compile.define"});
            }
            auto converted = utf8_to_utf16(define);
            if (!converted.has_value())
            {
                return blobResult::failure(*converted.try_error());
            }
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
        {
            pointers.push_back(argument.c_str());
        }
        const auto &data = *source.try_value();
        const char emptySource = '\0';
        DxcBuffer buffer{data.empty() ? static_cast<const void *>(&emptySource) : data.data(), data.size(),
                         DXC_CP_UTF8};
        m_state->includes->clear_error();
        Microsoft::WRL::ComPtr<IDxcResult> compiled;
        HRESULT hr = m_state->compiler->Compile(&buffer, pointers.data(), static_cast<UINT32>(pointers.size()),
                                                m_state->includes.Get(), IID_PPV_ARGS(&compiled));
        if (FAILED(hr))
        {
            return blobResult::failure(compiler_error("DXC.Compile", hr));
        }
        HRESULT status = E_FAIL;
        hr = compiled->GetStatus(&status);
        if (FAILED(hr))
        {
            return blobResult::failure(compiler_error("DXC.GetStatus", hr));
        }
        if (FAILED(status))
        {
            Microsoft::WRL::ComPtr<IDxcBlobUtf8> diagnostics;
            std::string message = "DXC.Compile: " + a_desc.filePath;
            if (m_state->includes->last_error())
            {
                message += "\nInclude File I/O: " + m_state->includes->last_error()->operation + " (" +
                           std::to_string(m_state->includes->last_error()->nativeCode) + ")";
            }
            if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr)) && diagnostics &&
                diagnostics->GetStringLength() != 0)
            {
                message += "\n" + std::string(diagnostics->GetStringPointer(), diagnostics->GetStringLength());
            }
            return blobResult::failure(compiler_error(std::move(message), status));
        }
        Microsoft::WRL::ComPtr<IDxcBlob> blob;
        hr = compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
        if (FAILED(hr) || !blob || blob->GetBufferSize() == 0)
        {
            return blobResult::failure(compiler_error("DXC.GetOutput", FAILED(hr) ? hr : E_FAIL));
        }
        return blobResult::success(std::move(blob));
    }
    catch (const std::bad_alloc &)
    {
        return blobResult::failure({ErrorCategory::PlatformFailure, "DXC.compile.allocation"});
    }
}
} // namespace cue::dx12
