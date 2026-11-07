#pragma once

#include <chrono>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <Foundation/Windows/UtfConversion.h>
#include <Platform/Windows/WindowsFileSystem.h>

namespace cue::tests
{
/// @brief 一意に新規作成した Test Root と FileSystem を所有する
struct TemporaryFiles final
{
  public:
    /// @brief Native の新規 Directory 作成に成功した場合だけ Cleanup を許可する
    TemporaryFiles()
    {
        auto result = create_windows_file_system();
        if (!result.has_value())
        {
            return;
        }
        files = result.take_value();
        std::wstring temporary(32768, L'\0');
        const auto length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (!length || length >= temporary.size())
        {
            return;
        }
        auto text = utf16_to_utf8(std::wstring_view(temporary).substr(0, length));
        if (!text.has_value())
        {
            return;
        }
        auto base = Path::create(*text.try_value());
        if (!base.has_value())
        {
            return;
        }
        auto joined =
            base.try_value()->join("cue-io-" + std::to_string(GetCurrentProcessId()) + "-" +
                                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!joined.has_value())
        {
            return;
        }
        root = joined.take_value();
        auto native = utf8_to_utf16(root.utf8());
        isCreated = native.has_value() && CreateDirectoryW(native.try_value()->c_str(), nullptr);
    }
    /// @brief 作成した Root 内だけを回収する。Reparse Point の先には移動しない
    ~TemporaryFiles()
    {
        if (isCreated)
        {
            try
            {
                cleanup(root);
            }
            catch (...)
            {
                OutputDebugStringA("TemporaryFiles cleanup failed\n");
            }
        }
    }
    std::unique_ptr<IFileSystem> files;
    Path root;
    bool isCreated = false;

  private:
    /// @brief Fixture の Root 所属を確認してから単一要素単位で回収する
    void cleanup(const Path &a_path)
    {
        if (a_path.utf8() != root.utf8() && !a_path.utf8().starts_with(root.utf8() + '/'))
        {
            return;
        }
        auto info = files->stat(a_path);
        if (!info.has_value() || !*info.try_value())
        {
            return;
        }
        if ((**info.try_value()).type == FileType::Directory)
        {
            auto children = files->list_directory(a_path);
            if (children.has_value())
            {
                for (const auto &child : *children.try_value())
                {
                    cleanup(child);
                }
            }
        }
        (void)files->remove(a_path);
    }
};
} // namespace cue::tests
