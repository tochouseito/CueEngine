#include <Cue/Platform/Windows/WindowsFileDialog.h>

#include "UtfConversion.h"
#include "WindowsUtilities.h"
#include "WindowsWindow.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>

#include <ShObjIdl.h>
#include <Windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
/// @brief Allocation失敗をFatal境界へ渡してProcessを停止する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Windows File Dialog allocation failed");
    std::abort();
}

/// @brief Windows File Dialog失敗をHRESULT付き診断Errorへ変換する
[[nodiscard]] cue::Error make_hresult_error(const cue::AssertContext &a_context, cue::WindowsFileDialogError a_code,
                                            std::string_view a_summary, HRESULT a_result) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.Platform.Windows", static_cast<std::int64_t>(a_code));
    cue::NativeError nativeError =
        cue::NativeError::create(a_context.fatal_handler(), "HRESULT", static_cast<std::int64_t>(a_result));
    return cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary, std::move(nativeError));
}

/// @brief Windows File Dialog失敗をNative情報なしの診断Errorへ変換する
[[nodiscard]] cue::Error make_dialog_error(const cue::AssertContext &a_context, cue::WindowsFileDialogError a_code,
                                           std::string_view a_summary) noexcept
{
    return cue::windows_private::make_error(a_context, static_cast<std::int64_t>(a_code), a_summary);
}

/// @brief 下位Windows処理の診断を保持してFile Dialogの安定した失敗分類へ変換する
[[nodiscard]] cue::Error reclassify_dialog_error(const cue::AssertContext &a_context,
                                                 cue::WindowsFileDialogError a_code, std::string_view a_summary,
                                                 cue::Error &&a_cause) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.Platform.Windows", static_cast<std::int64_t>(a_code));
    return cue::Error::reclassify(a_context.fatal_handler(), std::move(code), a_summary, std::move(a_cause));
}

/// @brief COM Interface Pointerを単一OwnerとしてReleaseする
template <typename T> class ComOwner final
{
  public:
    /// @brief 空のCOM Ownerを構築する
    ComOwner() noexcept = default;
    /// @brief COM参照の複製を禁止する
    ComOwner(const ComOwner &) = delete;
    /// @brief COM参照のCopy代入を禁止する
    ComOwner &operator=(const ComOwner &) = delete;
    /// @brief 所有するCOM参照をReleaseする
    ~ComOwner() noexcept
    {
        if (m_value != nullptr)
        {
            m_value->Release();
        }
    }

    /// @brief COM生成APIへ空の出力先を渡す
    [[nodiscard]] T **put() noexcept
    {
        return &m_value;
    }

    /// @brief 所有するCOM Interfaceを非所有で返す
    [[nodiscard]] T *get() const noexcept
    {
        return m_value;
    }

  private:
    T *m_value = nullptr;
};

/// @brief COM Task Allocatorが返した文字列を単一Ownerとして解放する
class ComTaskString final
{
  public:
    /// @brief 空の文字列Ownerを構築する
    ComTaskString() noexcept = default;
    /// @brief Native文字列の複製を禁止する
    ComTaskString(const ComTaskString &) = delete;
    /// @brief Native文字列のCopy代入を禁止する
    ComTaskString &operator=(const ComTaskString &) = delete;
    /// @brief 所有するNative文字列を解放する
    ~ComTaskString() noexcept
    {
        CoTaskMemFree(m_value);
    }

    /// @brief COM APIへ空の文字列出力先を渡す
    [[nodiscard]] PWSTR *put() noexcept
    {
        return &m_value;
    }

    /// @brief 所有するUTF-16文字列を非所有で返す
    [[nodiscard]] const wchar_t *get() const noexcept
    {
        return m_value;
    }

  private:
    PWSTR m_value = nullptr;
};

/// @brief 現在Threadで成功したCOM初期化をScope終了時に対応解除する
class ComApartmentScope final
{
  public:
    /// @brief CoInitializeEx成功状態を所有する
    explicit ComApartmentScope(bool a_isInitialized) noexcept : m_isInitialized(a_isInitialized)
    {
    }
    /// @brief COM初期化Ownerの複製を禁止する
    ComApartmentScope(const ComApartmentScope &) = delete;
    /// @brief COM初期化OwnerのCopy代入を禁止する
    ComApartmentScope &operator=(const ComApartmentScope &) = delete;
    /// @brief この呼出しが取得したCOM初期化参照を解放する
    ~ComApartmentScope() noexcept
    {
        if (m_isInitialized)
        {
            CoUninitialize();
        }
    }

  private:
    bool m_isInitialized;
};

/// @brief 文字列に埋め込みNULがないか判定する
[[nodiscard]] bool has_no_embedded_null(std::string_view a_text) noexcept
{
    return a_text.find('\0') == std::string_view::npos;
}

/// @brief Default ExtensionがFile名要素だけで構成されるか判定する
[[nodiscard]] bool is_valid_default_extension(std::string_view a_extension) noexcept
{
    return has_no_embedded_null(a_extension) && a_extension.find_first_of("./\\*?;:") == std::string_view::npos;
}

/// @brief UI非依存RequestをNative呼出前に検証する
[[nodiscard]] cue::Result<void> validate_request(const cue::FileDialogRequest &a_request,
                                                 const cue::AssertContext &a_context) noexcept
{
    const cue::FileDialogKind kind = a_request.kind();
    if (kind != cue::FileDialogKind::OpenFile && kind != cue::FileDialogKind::SaveFile &&
        kind != cue::FileDialogKind::SelectFolder)
    {
        return cue::Result<void>::failure(
            make_dialog_error(a_context, cue::WindowsFileDialogError::InvalidRequest, "File Dialog kind is invalid"));
    }

    if (!has_no_embedded_null(a_request.initial_location_hint()) ||
        !is_valid_default_extension(a_request.default_extension()))
    {
        return cue::Result<void>::failure(make_dialog_error(a_context, cue::WindowsFileDialogError::InvalidRequest,
                                                            "File Dialog text option is invalid"));
    }

    if (kind == cue::FileDialogKind::SelectFolder &&
        (!a_request.filters().empty() || !a_request.default_extension().empty()))
    {
        return cue::Result<void>::failure(
            make_dialog_error(a_context, cue::WindowsFileDialogError::InvalidRequest,
                              "Folder Dialog cannot use file filters or a default extension"));
    }

    for (const cue::FileDialogFilter &filter : a_request.filters())
    {
        if (filter.displayName.empty() || filter.pattern.empty() || !has_no_embedded_null(filter.displayName) ||
            !has_no_embedded_null(filter.pattern))
        {
            return cue::Result<void>::failure(make_dialog_error(a_context, cue::WindowsFileDialogError::InvalidRequest,
                                                                "File Dialog filter is invalid"));
        }
    }

    return cue::Result<void>::success();
}

/// @brief COM DialogへFilterを所有UTF-16 Bufferから設定する
[[nodiscard]] cue::Result<void> configure_filters(IFileDialog &a_dialog, const cue::FileDialogRequest &a_request,
                                                  const cue::AssertContext &a_context) noexcept
{
    if (a_request.filters().empty())
    {
        return cue::Result<void>::success();
    }

    std::vector<std::wstring> names;
    std::vector<std::wstring> patterns;
    std::vector<COMDLG_FILTERSPEC> nativeFilters;
    names.reserve(a_request.filters().size());
    patterns.reserve(a_request.filters().size());
    nativeFilters.reserve(a_request.filters().size());

    for (const cue::FileDialogFilter &filter : a_request.filters())
    {
        cue::Result<std::wstring> name = cue::utf8_to_utf16(filter.displayName, a_context);
        cue::Result<std::wstring> pattern = cue::utf8_to_utf16(filter.pattern, a_context);
        if (!name)
        {
            return cue::Result<void>::failure(
                reclassify_dialog_error(a_context, cue::WindowsFileDialogError::InvalidRequest,
                                        "File Dialog filter name is not valid UTF-8", std::move(*name.try_error())));
        }
        if (!pattern)
        {
            return cue::Result<void>::failure(reclassify_dialog_error(
                a_context, cue::WindowsFileDialogError::InvalidRequest, "File Dialog filter pattern is not valid UTF-8",
                std::move(*pattern.try_error())));
        }

        names.push_back(std::move(*name.try_value()));
        patterns.push_back(std::move(*pattern.try_value()));
    }

    for (std::size_t index = 0U; index < names.size(); ++index)
    {
        nativeFilters.push_back({names[index].c_str(), patterns[index].c_str()});
    }

    HRESULT result = a_dialog.SetFileTypes(static_cast<UINT>(nativeFilters.size()), nativeFilters.data());
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_hresult_error(a_context,
                                                             cue::WindowsFileDialogError::DialogConfigurationFailed,
                                                             "File Dialog filter configuration failed", result));
    }

    result = a_dialog.SetFileTypeIndex(1U);
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_hresult_error(a_context,
                                                             cue::WindowsFileDialogError::DialogConfigurationFailed,
                                                             "File Dialog filter selection failed", result));
    }

    return cue::Result<void>::success();
}

/// @brief COM Dialogへ共通Optionと要求固有Optionを設定する
[[nodiscard]] cue::Result<void> configure_options(IFileDialog &a_dialog, const cue::FileDialogRequest &a_request,
                                                  const cue::AssertContext &a_context) noexcept
{
    FILEOPENDIALOGOPTIONS options = {};
    HRESULT result = a_dialog.GetOptions(&options);
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_hresult_error(a_context,
                                                             cue::WindowsFileDialogError::DialogConfigurationFailed,
                                                             "File Dialog option query failed", result));
    }

    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
    if (a_request.kind() == cue::FileDialogKind::OpenFile)
    {
        options |= FOS_FILEMUSTEXIST;
    }
    else if (a_request.kind() == cue::FileDialogKind::SaveFile)
    {
        options |= FOS_OVERWRITEPROMPT;
    }
    else
    {
        options |= FOS_PICKFOLDERS;
    }

    result = a_dialog.SetOptions(options);
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_hresult_error(a_context,
                                                             cue::WindowsFileDialogError::DialogConfigurationFailed,
                                                             "File Dialog option configuration failed", result));
    }

    return cue::Result<void>::success();
}

/// @brief COM DialogへDefault Extensionを設定する
[[nodiscard]] cue::Result<void> configure_default_extension(IFileDialog &a_dialog,
                                                            const cue::FileDialogRequest &a_request,
                                                            const cue::AssertContext &a_context) noexcept
{
    if (a_request.default_extension().empty())
    {
        return cue::Result<void>::success();
    }

    cue::Result<std::wstring> extension = cue::utf8_to_utf16(a_request.default_extension(), a_context);
    if (!extension)
    {
        return cue::Result<void>::failure(reclassify_dialog_error(
            a_context, cue::WindowsFileDialogError::InvalidRequest, "File Dialog default extension is not valid UTF-8",
            std::move(*extension.try_error())));
    }

    const HRESULT result = a_dialog.SetDefaultExtension(extension.try_value()->c_str());
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_hresult_error(a_context, cue::WindowsFileDialogError::DialogConfigurationFailed,
                               "File Dialog default extension configuration failed", result));
    }

    return cue::Result<void>::success();
}

/// @brief COM Dialogへ初期Location Hintを設定する
[[nodiscard]] cue::Result<void> configure_initial_location(IFileDialog &a_dialog,
                                                           const cue::FileDialogRequest &a_request,
                                                           const cue::AssertContext &a_context) noexcept
{
    if (a_request.initial_location_hint().empty())
    {
        return cue::Result<void>::success();
    }

    cue::Result<std::wstring> location = cue::utf8_to_utf16(a_request.initial_location_hint(), a_context);
    if (!location)
    {
        return cue::Result<void>::failure(reclassify_dialog_error(
            a_context, cue::WindowsFileDialogError::InvalidRequest, "File Dialog initial location is not valid UTF-8",
            std::move(*location.try_error())));
    }

    ComOwner<IShellItem> folder;
    HRESULT result = SHCreateItemFromParsingName(location.try_value()->c_str(), nullptr, IID_PPV_ARGS(folder.put()));
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_hresult_error(a_context,
                                                             cue::WindowsFileDialogError::DialogConfigurationFailed,
                                                             "File Dialog initial location resolution failed", result));
    }

    result = a_dialog.SetDefaultFolder(folder.get());
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_hresult_error(a_context, cue::WindowsFileDialogError::DialogConfigurationFailed,
                               "File Dialog initial location configuration failed", result));
    }

    return cue::Result<void>::success();
}

/// @brief Native選択PathをAbsolute UTF-16 Pathへ正規化する
[[nodiscard]] cue::Result<std::wstring> normalize_selected_path(std::wstring_view a_path,
                                                                const cue::AssertContext &a_context) noexcept
{
    if (a_path.empty() || a_path.find(L'\0') != std::wstring_view::npos)
    {
        return cue::Result<std::wstring>::failure(
            make_dialog_error(a_context, cue::WindowsFileDialogError::PathNormalizationFailed,
                              "File Dialog selected path is empty or invalid"));
    }

    const std::wstring source(a_path);
    const DWORD required = GetFullPathNameW(source.c_str(), 0U, nullptr, nullptr);
    if (required == 0U)
    {
        return cue::Result<std::wstring>::failure(cue::windows_private::make_native_error(
            a_context, static_cast<std::int64_t>(cue::WindowsFileDialogError::PathNormalizationFailed),
            "File Dialog selected path normalization failed", GetLastError()));
    }

    std::vector<wchar_t> buffer(required);
    const DWORD written = GetFullPathNameW(source.c_str(), required, buffer.data(), nullptr);
    if (written == 0U || written >= required)
    {
        const DWORD nativeCode = written == 0U ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return cue::Result<std::wstring>::failure(cue::windows_private::make_native_error(
            a_context, static_cast<std::int64_t>(cue::WindowsFileDialogError::PathNormalizationFailed),
            "File Dialog selected path normalization failed", nativeCode));
    }

    return cue::Result<std::wstring>::success(std::wstring(buffer.data(), written));
}
} // namespace

namespace cue
{
/// @brief Platform実装だけにFile Dialog Owner Tokenの生成と読取りを許可する
class FileDialogOwnerAccess final
{
  public:
    /// @brief 検証済みNative Owner値、Generation、ThreadからOpaque Tokenを生成する
    [[nodiscard]] static FileDialogOwnerToken create(std::uintptr_t a_nativeValue, std::uint64_t a_ownerGeneration,
                                                     std::uint32_t a_ownerThreadId) noexcept
    {
        return FileDialogOwnerToken(a_nativeValue, a_ownerGeneration, a_ownerThreadId);
    }

    /// @brief Opaque Tokenが保持するNative Owner値をPlatform実装へ返す
    [[nodiscard]] static std::uintptr_t native_value(const FileDialogOwnerToken &a_owner) noexcept
    {
        return a_owner.m_nativeValue;
    }

    /// @brief Opaque Tokenが保持するOwner Thread IDをPlatform実装へ返す
    [[nodiscard]] static std::uint32_t owner_thread_id(const FileDialogOwnerToken &a_owner) noexcept
    {
        return a_owner.m_ownerThreadId;
    }

    /// @brief HWNDとWindow Objectの再利用を区別するGenerationを返す
    [[nodiscard]] static std::uint64_t owner_generation(const FileDialogOwnerToken &a_owner) noexcept
    {
        return a_owner.m_ownerGeneration;
    }
};

/// @brief Windows COM File DialogをUI非依存契約へ適合させる
class WindowsFileDialogService final : public FileDialogService
{
  public:
    /// @brief 非所有診断Contextを保持するWindows Dialog Serviceを構築する
    explicit WindowsFileDialogService(const AssertContext &a_assertContext) noexcept : m_assertContext(&a_assertContext)
    {
    }

    /// @brief Owner Thread上でOpen、Save、Folder Dialogを同期表示する
    [[nodiscard]] Result<FileDialogResult> show(const FileDialogRequest &a_request) noexcept override
    {
        try
        {
            return show_internal(a_request);
        }
        catch (const std::bad_alloc &)
        {
            terminate_allocation(*m_assertContext);
        }
    }

  private:
    /// @brief 検証済みRequestをWindows COM Dialogへ変換して結果を返す
    [[nodiscard]] Result<FileDialogResult> show_internal(const FileDialogRequest &a_request)
    {
        Result<void> validated = validate_request(a_request, *m_assertContext);
        if (!validated)
        {
            return Result<FileDialogResult>::failure(std::move(*validated.try_error()));
        }

        const FileDialogOwnerToken &owner = a_request.owner();
        const std::uintptr_t nativeOwnerValue = FileDialogOwnerAccess::native_value(owner);
        const std::uint64_t ownerGeneration = FileDialogOwnerAccess::owner_generation(owner);
        const std::uint32_t ownerThreadId = FileDialogOwnerAccess::owner_thread_id(owner);
        if (nativeOwnerValue == 0U || ownerGeneration == 0U || ownerThreadId == 0U)
        {
            return Result<FileDialogResult>::failure(make_dialog_error(
                *m_assertContext, WindowsFileDialogError::OwnerUnavailable, "File Dialog Owner Token is unavailable"));
        }
        if (GetCurrentThreadId() != ownerThreadId)
        {
            return Result<FileDialogResult>::failure(
                make_dialog_error(*m_assertContext, WindowsFileDialogError::OwnerThreadViolation,
                                  "File Dialog must run on its Owner Window thread"));
        }

        HWND ownerWindow = reinterpret_cast<HWND>(nativeOwnerValue);
        DWORD nativeOwnerThread = GetWindowThreadProcessId(ownerWindow, nullptr);
        const std::uint64_t liveOwnerGeneration = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(GetPropW(ownerWindow, k_windowsDialogOwnerGenerationProperty)));
        if (IsWindow(ownerWindow) == FALSE || nativeOwnerThread == 0U || nativeOwnerThread != ownerThreadId ||
            liveOwnerGeneration != ownerGeneration)
        {
            return Result<FileDialogResult>::failure(
                make_dialog_error(*m_assertContext, WindowsFileDialogError::OwnerUnavailable,
                                  "File Dialog Owner Window is no longer available"));
        }

        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(initialized))
        {
            return Result<FileDialogResult>::failure(
                make_hresult_error(*m_assertContext, WindowsFileDialogError::ComInitializationFailed,
                                   "File Dialog COM initialization failed", initialized));
        }
        ComApartmentScope apartment(true);

        ComOwner<IFileDialog> dialog;
        HRESULT result =
            a_request.kind() == FileDialogKind::SaveFile
                ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))
                : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()));
        if (FAILED(result))
        {
            return Result<FileDialogResult>::failure(make_hresult_error(*m_assertContext,
                                                                        WindowsFileDialogError::DialogCreationFailed,
                                                                        "Windows File Dialog creation failed", result));
        }

        Result<void> options = configure_options(*dialog.get(), a_request, *m_assertContext);
        if (!options)
        {
            return Result<FileDialogResult>::failure(std::move(*options.try_error()));
        }
        Result<void> filters = configure_filters(*dialog.get(), a_request, *m_assertContext);
        if (!filters)
        {
            return Result<FileDialogResult>::failure(std::move(*filters.try_error()));
        }
        Result<void> extension = configure_default_extension(*dialog.get(), a_request, *m_assertContext);
        if (!extension)
        {
            return Result<FileDialogResult>::failure(std::move(*extension.try_error()));
        }
        Result<void> location = configure_initial_location(*dialog.get(), a_request, *m_assertContext);
        if (!location)
        {
            return Result<FileDialogResult>::failure(std::move(*location.try_error()));
        }

        result = dialog.get()->Show(ownerWindow);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return Result<FileDialogResult>::success(FileDialogResult::cancelled());
        }
        if (FAILED(result))
        {
            return Result<FileDialogResult>::failure(make_hresult_error(*m_assertContext,
                                                                        WindowsFileDialogError::DialogDisplayFailed,
                                                                        "Windows File Dialog display failed", result));
        }

        ComOwner<IShellItem> selectedItem;
        result = dialog.get()->GetResult(selectedItem.put());
        if (FAILED(result))
        {
            return Result<FileDialogResult>::failure(
                make_hresult_error(*m_assertContext, WindowsFileDialogError::SelectionFailed,
                                   "Windows File Dialog selection query failed", result));
        }

        ComTaskString selectedPath;
        result = selectedItem.get()->GetDisplayName(SIGDN_FILESYSPATH, selectedPath.put());
        if (FAILED(result) || selectedPath.get() == nullptr)
        {
            const HRESULT failure = FAILED(result) ? result : E_FAIL;
            return Result<FileDialogResult>::failure(
                make_hresult_error(*m_assertContext, WindowsFileDialogError::SelectionFailed,
                                   "Windows File Dialog did not return a filesystem path", failure));
        }

        Result<std::wstring> normalized = normalize_selected_path(selectedPath.get(), *m_assertContext);
        if (!normalized)
        {
            return Result<FileDialogResult>::failure(std::move(*normalized.try_error()));
        }
        Result<std::string> utf8Path = utf16_to_utf8(*normalized.try_value(), *m_assertContext);
        if (!utf8Path)
        {
            return Result<FileDialogResult>::failure(reclassify_dialog_error(
                *m_assertContext, WindowsFileDialogError::PathNormalizationFailed,
                "File Dialog selected path could not be represented as UTF-8", std::move(*utf8Path.try_error())));
        }

        return Result<FileDialogResult>::success(FileDialogResult::selected(std::move(*utf8Path.try_value())));
    }

    const AssertContext *m_assertContext;
};

/// @brief Windows Windowから短命なNative Dialog Owner Capabilityを発行する
Result<FileDialogOwnerToken> create_windows_file_dialog_owner(Window &a_window,
                                                              const AssertContext &a_assertContext) noexcept
{
    auto *windowsWindow = dynamic_cast<WindowsWindow *>(&a_window);
    if (windowsWindow == nullptr || windowsWindow->m_system == nullptr)
    {
        return Result<FileDialogOwnerToken>::failure(
            make_dialog_error(a_assertContext, WindowsFileDialogError::OwnerUnavailable,
                              "File Dialog Owner must be a Windows Window with a live Window System"));
    }

    const DWORD currentThread = GetCurrentThreadId();
    const DWORD ownerThread = windowsWindow->m_system->thread_id();
    if (ownerThread != currentThread)
    {
        return Result<FileDialogOwnerToken>::failure(
            make_dialog_error(a_assertContext, WindowsFileDialogError::OwnerThreadViolation,
                              "File Dialog Owner Token must be created on the Window thread"));
    }

    HWND nativeWindow = windowsWindow->m_window;
    if (windowsWindow->m_state == WindowState::Destroyed || nativeWindow == nullptr || IsWindow(nativeWindow) == FALSE)
    {
        return Result<FileDialogOwnerToken>::failure(make_dialog_error(
            a_assertContext, WindowsFileDialogError::OwnerUnavailable, "File Dialog Owner Window is unavailable"));
    }
    const DWORD nativeOwnerThread = GetWindowThreadProcessId(nativeWindow, nullptr);
    if (nativeOwnerThread == 0U || nativeOwnerThread != ownerThread)
    {
        return Result<FileDialogOwnerToken>::failure(make_dialog_error(
            a_assertContext, WindowsFileDialogError::OwnerUnavailable, "File Dialog Owner Window is unavailable"));
    }

    return Result<FileDialogOwnerToken>::success(FileDialogOwnerAccess::create(
        reinterpret_cast<std::uintptr_t>(nativeWindow), windowsWindow->dialog_owner_generation(), ownerThread));
}

/// @brief Project HubとEditorで共有するWindows Native File Dialog Serviceを生成する
std::unique_ptr<FileDialogService> create_windows_file_dialog_service(const AssertContext &a_assertContext) noexcept
{
    try
    {
        return std::make_unique<WindowsFileDialogService>(a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
}
} // namespace cue
