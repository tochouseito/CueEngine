#include "EditorWindowsServices.h"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Shell の宣言より先に Win32 の基本型を読み込む
#include <shobjidl.h>
#include <wrl/client.h>

#include <EditorUi/EditorLanguage.h>
#include <Foundation/Uuid.h>
#include <Foundation/Windows/UtfConversion.h>
#include <Platform/FileSystem.h>
#include <Platform/Windows/WindowsPlatform.h>

namespace cue
{
namespace
{
/// @brief Dialog 呼出の成功した COM 初期化だけを対応する解放と組にする
class DialogApartment final
{
  public:
    /// @brief UI Dialog 用の STA をこの Thread に初期化する
    DialogApartment() noexcept : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }

    /// @brief S_OK と S_FALSE の初期化だけを解放し、既存の別 Apartment を変更しない
    ~DialogApartment()
    {
        if (SUCCEEDED(m_result))
        {
            CoUninitialize();
        }
    }

    DialogApartment(const DialogApartment &) = delete;
    DialogApartment &operator=(const DialogApartment &) = delete;

    /// @brief COM 初期化の成否を返す
    [[nodiscard]] HRESULT result() const noexcept
    {
        return m_result;
    }

  private:
    HRESULT m_result;
};

/// @brief Shell が確保した Path を COM の Allocator へ返す
struct ShellPathDeleter final
{
    /// @brief GetDisplayName の返却領域を解放する
    void operator()(wchar_t *a_path) const noexcept
    {
        CoTaskMemFree(a_path);
    }
};

/// @brief Dialog の開始 Folder を既存 Scene の親、または Content Root に合わせる
[[nodiscard]] Result<void> set_initial_location(IFileDialog &a_dialog, bool a_isSave, const Path *a_initialFile,
                                                const Path *a_contentRoot)
{
    // Scene の場所が決まっていれば、その Folder と File 名を次回選択の開始位置にする
    const bool hasInitialFile = a_initialFile && !a_initialFile->is_empty();
    const Path folder = hasInitialFile ? a_initialFile->parent() : (a_contentRoot ? *a_contentRoot : Path{});
    if (!folder.is_empty())
    {
        auto folderText = utf8_to_utf16(folder.utf8());
        if (!folderText.has_value())
        {
            return Result<void>::failure(*folderText.try_error());
        }
        // Shell の Parsing Name は共通 Path の '/' を受け付けないため、Native 区切りへ戻す
        auto &nativeFolder = *folderText.try_value();
        std::replace(nativeFolder.begin(), nativeFolder.end(), L'/', L'\\');
        Microsoft::WRL::ComPtr<IShellItem> folderItem;
        auto result = SHCreateItemFromParsingName(folderText.try_value()->c_str(), nullptr, IID_PPV_ARGS(&folderItem));
        if (FAILED(result))
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.SceneDialog.initial_folder", result});
        }
        result = a_dialog.SetFolder(folderItem.Get());
        if (FAILED(result))
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "IFileDialog.SetFolder", result});
        }
    }
    // 新規保存だけ仮の名前を付け、既存 Scene ならその名前を維持する
    if (hasInitialFile || a_isSave)
    {
        auto filename = utf8_to_utf16(hasInitialFile ? a_initialFile->filename() : "Untitled.cuescene");
        if (!filename.has_value())
        {
            return Result<void>::failure(*filename.try_error());
        }
        const auto result = a_dialog.SetFileName(filename.try_value()->c_str());
        if (FAILED(result))
        {
            return Result<void>::failure({ErrorCategory::PlatformFailure, "IFileDialog.SetFileName", result});
        }
    }
    return Result<void>::success();
}
} // namespace

Result<std::string> create_editor_object_id()
{
    // CoCreateGuid は COM Interface を使わず、呼出側の Apartment 初期化を必要としない
    GUID identifier{};
    const auto result = CoCreateGuid(&identifier);
    if (FAILED(result))
    {
        return Result<std::string>::failure({ErrorCategory::PlatformFailure, "CoCreateGuid", result});
    }
    std::array<wchar_t, 39> formatted{};
    if (StringFromGUID2(identifier, formatted.data(), static_cast<int>(formatted.size())) != 39)
    {
        return Result<std::string>::failure({ErrorCategory::PlatformFailure, "StringFromGUID2"});
    }
    // 外側の波括弧を除き、Scene 契約に合わせて英字を小文字へ正規化する
    std::string text;
    text.reserve(36);
    for (std::size_t index = 1; index <= 36; ++index)
    {
        auto character = formatted[index];
        if (character >= L'A' && character <= L'F')
        {
            character += L'a' - L'A';
        }
        text.push_back(static_cast<char>(character));
    }
    if (!is_valid_uuid_text(text))
    {
        return Result<std::string>::failure({ErrorCategory::PlatformFailure, "Editor.ObjectId.uuid"});
    }
    return Result<std::string>::success(std::move(text));
}

Result<std::optional<Path>> choose_scene_file(Window &a_window, bool a_isSave, const Path *a_initialFile,
                                              const Path *a_contentRoot, EditorLanguage a_language)
{
    auto handle = borrow_windows_window_handle(a_window);
    if (!handle.has_value())
    {
        return Result<std::optional<Path>>::failure(*handle.try_error());
    }
    // Dialog と Shell Item が解放された後に、この呼出分の COM 初期化を解除する
    DialogApartment apartment;
    if (FAILED(apartment.result()))
    {
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.SceneDialog.CoInitializeEx", apartment.result()});
    }
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    auto result = CoCreateInstance(a_isSave ? __uuidof(FileSaveDialog) : __uuidof(FileOpenDialog), nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result))
    {
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.SceneDialog.CoCreateInstance", result});
    }
    // Filesystem 上の Scene のみ選択し、Current Directory や Recent 一覧への副作用を抑える
    FILEOPENDIALOGOPTIONS options{};
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result))
    {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
        options |= a_isSave ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
        result = dialog->SetOptions(options);
    }
    if (FAILED(result))
    {
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.SceneDialog.options", result});
    }
    // 共通 Dialog の操作文言は Windows が表示し、CueEngine 側で指定する文言だけを翻訳する
    const auto *title = a_isSave ? editor_text(a_language, L"Save Scene", L"シーンを保存")
                                 : editor_text(a_language, L"Open Scene", L"シーンを開く");
    const COMDLG_FILTERSPEC filter[] = {
        {editor_text(a_language, L"CueEngine Scene (*.cuescene)", L"CueEngine シーン (*.cuescene)"), L"*.cuescene"}};
    result = dialog->SetTitle(title);
    if (SUCCEEDED(result))
        result = dialog->SetFileTypes(1, filter);
    if (SUCCEEDED(result))
    {
        result = dialog->SetDefaultExtension(L"cuescene");
    }
    if (FAILED(result))
    {
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.SceneDialog.file_type", result});
    }
    auto location = set_initial_location(*dialog.Get(), a_isSave, a_initialFile, a_contentRoot);
    if (!location.has_value())
    {
        return Result<std::optional<Path>>::failure(*location.try_error());
    }
    // ユーザーのキャンセルは失敗扱いにせず、呼出元が作業をそのまま継続できるようにする
    result = dialog->Show(static_cast<HWND>(handle.take_value()));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        return Result<std::optional<Path>>::success(std::nullopt);
    }
    if (FAILED(result))
    {
        return Result<std::optional<Path>>::failure({ErrorCategory::PlatformFailure, "IFileDialog.Show", result});
    }
    Microsoft::WRL::ComPtr<IShellItem> selected;
    result = dialog->GetResult(&selected);
    if (FAILED(result))
    {
        return Result<std::optional<Path>>::failure({ErrorCategory::PlatformFailure, "IFileDialog.GetResult", result});
    }
    PWSTR rawPath = nullptr;
    result = selected->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    std::unique_ptr<wchar_t, ShellPathDeleter> ownedPath(rawPath);
    if (FAILED(result) || !ownedPath)
    {
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "IShellItem.GetDisplayName", result});
    }
    // Shell の UTF-16 Path を Foundation の UTF-8 Path に戻し、OS 依存をここで閉じる
    auto text = utf16_to_utf8(ownedPath.get());
    if (!text.has_value())
    {
        return Result<std::optional<Path>>::failure(*text.try_error());
    }
    auto path = Path::create(text.take_value());
    if (!path.has_value())
    {
        return Result<std::optional<Path>>::failure(*path.try_error());
    }
    return Result<std::optional<Path>>::success(path.take_value());
}

Result<std::optional<Path>> choose_project_folder(Window &a_window, const Path *a_initialFolder,
                                                  EditorLanguage a_language)
{
    auto handle = borrow_windows_window_handle(a_window);
    if (!handle.has_value())
        return Result<std::optional<Path>>::failure(*handle.try_error());
    // Scene Dialog と同じ STA / Window Owner を使い、キャンセルで現在の Project を変更しない
    DialogApartment apartment;
    if (FAILED(apartment.result()))
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.ProjectDialog.CoInitializeEx", apartment.result()});
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    auto result = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result))
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.ProjectDialog.CoCreateInstance", result});
    FILEOPENDIALOGOPTIONS options{};
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result))
        result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                    FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    if (SUCCEEDED(result))
        result = dialog->SetTitle(
            editor_text(a_language, L"Open CueEngine Project Folder", L"CueEngine プロジェクトフォルダーを開く"));
    if (FAILED(result))
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.ProjectDialog.options", result});
    auto location = set_initial_location(*dialog.Get(), false, nullptr, a_initialFolder);
    if (!location.has_value())
        return Result<std::optional<Path>>::failure(*location.try_error());
    result = dialog->Show(static_cast<HWND>(handle.take_value()));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return Result<std::optional<Path>>::success(std::nullopt);
    if (FAILED(result))
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.ProjectDialog.Show", result});
    Microsoft::WRL::ComPtr<IShellItem> selected;
    result = dialog->GetResult(&selected);
    if (FAILED(result))
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.ProjectDialog.GetResult", result});
    PWSTR rawPath = nullptr;
    result = selected->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    std::unique_ptr<wchar_t, ShellPathDeleter> ownedPath(rawPath);
    if (FAILED(result) || !ownedPath)
        return Result<std::optional<Path>>::failure(
            {ErrorCategory::PlatformFailure, "Editor.ProjectDialog.GetDisplayName", result});
    auto text = utf16_to_utf8(ownedPath.get());
    if (!text.has_value())
        return Result<std::optional<Path>>::failure(*text.try_error());
    auto path = Path::create(text.take_value());
    if (!path.has_value())
        return Result<std::optional<Path>>::failure(*path.try_error());
    return Result<std::optional<Path>>::success(path.take_value());
}

Result<Path> find_project_file(IFileSystem &a_files, const Path &a_folder)
{
    try
    {
        if (a_folder.is_empty())
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.project_folder.empty"});
        auto folder = a_files.absolute_path(a_folder);
        if (!folder.has_value())
            return Result<Path>::failure(*folder.try_error());
        // Folder と親の Link を拒否し、選択表示と実際の読込先が別配置になることを防ぐ
        for (Path current = *folder.try_value();;)
        {
            auto info = a_files.stat(current);
            if (!info.has_value())
                return Result<Path>::failure(*info.try_error());
            if (!*info.try_value() || (**info.try_value()).type != FileType::Directory)
                return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.project_folder.not_directory"});
            const auto parent = current.parent();
            if (parent.is_empty() || parent.utf8() == current.utf8())
                break;
            current = parent;
        }
        auto entries = a_files.list_directory(*folder.try_value());
        if (!entries.has_value())
            return Result<Path>::failure(*entries.try_error());
        std::optional<Path> selected;
        for (const auto &entry : *entries.try_value())
        {
            // 拡張子以外は Project Parser で検証する。Folder 列挙順で選択結果を変えない
            auto extension = entry.extension();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](char a_character)
                           {
                               return static_cast<char>(
                                   a_character >= 'A' && a_character <= 'Z' ? a_character + ('a' - 'A') : a_character);
                           });
            if (extension != ".cueproj")
                continue;
            auto info = a_files.stat(entry);
            if (!info.has_value())
                return Result<Path>::failure(*info.try_error());
            if (!*info.try_value() || (**info.try_value()).type != FileType::Regular)
                return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.project_folder.invalid_file"});
            if (selected)
                return Result<Path>::failure(
                    {ErrorCategory::InvalidArgument, "Editor.project_folder.multiple_projects"});
            selected = entry;
        }
        if (!selected)
            return Result<Path>::failure({ErrorCategory::InvalidArgument, "Editor.project_folder.no_project"});
        return Result<Path>::success(std::move(*selected));
    }
    catch (const std::bad_alloc &)
    {
        return Result<Path>::failure({ErrorCategory::PlatformFailure, "Editor.project_folder.allocation"});
    }
}

Result<void> set_editor_project_title(Window &a_window, std::string_view a_projectName)
{
    try
    {
        if (a_projectName.find('\0') != std::string_view::npos)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "Editor.window_title.name"});
        auto handle = borrow_windows_window_handle(a_window);
        if (!handle.has_value())
            return Result<void>::failure(*handle.try_error());
        const auto title = a_projectName.empty() ? std::string("CueEngine Editor")
                                                 : "CueEngine Editor - " + std::string(a_projectName);
        auto native = utf8_to_utf16(title);
        if (!native.has_value())
            return Result<void>::failure(*native.try_error());
        // Project 切替を Window 生成と分離し、Debugger が接続している Process / Window を維持する
        if (!SetWindowTextW(static_cast<HWND>(handle.take_value()), native.try_value()->c_str()))
            return Result<void>::failure(
                {ErrorCategory::PlatformFailure, "Editor.SetWindowTextW.title", GetLastError()});
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.window_title.allocation"});
    }
}

Result<void> request_editor_close(Window &a_window)
{
    auto handle = borrow_windows_window_handle(a_window);
    if (!handle.has_value())
    {
        return Result<void>::failure(*handle.try_error());
    }
    // Menu と Title Bar の終了を同じ Close Event 経路へ揃える
    if (!PostMessageW(static_cast<HWND>(handle.take_value()), WM_CLOSE, 0, 0))
    {
        return Result<void>::failure({ErrorCategory::PlatformFailure, "Editor.PostMessageW.close", GetLastError()});
    }
    return Result<void>::success();
}
} // namespace cue
