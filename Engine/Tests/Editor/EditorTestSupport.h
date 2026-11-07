#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <Editor/EditorApplication.h>
#include <Runtime/Components/TransformComponent.h>
#include <Runtime/World.h>

namespace cue::tests
{
/// @brief 成功を前提とする Fixture 値を一時 Result から所有値へ移す
template <typename T> T editor_value(Result<T> a_result)
{
    return a_result.take_value();
}

/// @brief Native API に依存せず、保存と読込みの契約を検証する Memory File
class EditorMemoryFile final : public IFile
{
  public:
    /// @brief Fixture 内容を独立して所有する
    explicit EditorMemoryFile(std::string a_text) : m_text(std::move(a_text))
    {
    }
    /// @brief EOF まで複写する
    Result<std::size_t> read(std::span<std::byte> a_bytes) override
    {
        if (m_isClosed)
            return Result<std::size_t>::failure({ErrorCategory::InvalidState, "Test.read.closed"});
        const auto count = std::min(a_bytes.size(), m_text.size() - m_cursor);
        std::memcpy(a_bytes.data(), m_text.data() + m_cursor, count);
        m_cursor += count;
        return Result<std::size_t>::success(count);
    }
    /// @brief Fixture の File は読取り専用とする
    Result<std::size_t> write(std::span<const std::byte>) override
    {
        return Result<std::size_t>::failure({ErrorCategory::InvalidState, "Test.write"});
    }
    /// @brief Fixture の利用経路にない Seek を拒否する
    Result<std::uint64_t> seek(std::int64_t, SeekOrigin) override
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "Test.seek"});
    }
    /// @brief 現在の読込み位置を返す
    Result<std::uint64_t> tell() override
    {
        return Result<std::uint64_t>::success(m_cursor);
    }
    /// @brief 独立して所有した内容の Size を返す
    Result<std::uint64_t> size() override
    {
        return Result<std::uint64_t>::success(m_text.size());
    }
    /// @brief 外部出力はない
    Result<void> flush() override
    {
        return Result<void>::success();
    }
    /// @brief Handle を閉じる
    Result<void> close() override
    {
        m_isClosed = true;
        return Result<void>::success();
    }

  private:
    std::string m_text;
    std::size_t m_cursor = 0;
    bool m_isClosed = false;
};

/// @brief Path 境界・Link・公開失敗・耐久性未確認を注入できる抽象 FileSystem
class EditorMemoryFiles final : public IFileSystem
{
  public:
    struct Entry final
    {
        FileType type = FileType::Regular;
        std::string text;
    };
    std::map<std::string, Entry> entries{{"/", {FileType::Directory, {}}},
                                         {"/Content", {FileType::Directory, {}}},
                                         {"/Content/Scenes", {FileType::Directory, {}}},
                                         {"/ContentOther", {FileType::Directory, {}}}};
    bool failSave = false;
    bool failRead = false;
    bool isDurabilityConfirmed = true;
    unsigned saveCount = 0;
    std::function<void()> onSave;
    /// @brief 相対 Path は Fixture Root に解決する
    Result<Path> absolute_path(const Path &a_path) override
    {
        return a_path.is_absolute() ? Result<Path>::success(a_path) : Path::create('/' + a_path.utf8());
    }
    /// @brief 固定した Fixture Root を返す
    Result<Path> executable_directory() override
    {
        return Path::create("/");
    }
    /// @brief 固定した Fixture Root を返す
    Result<Path> local_data_directory() override
    {
        return Path::create("/");
    }
    /// @brief 登録した要素の種類と Size を返す
    Result<std::optional<FileInfo>> stat(const Path &a_path) override
    {
        const auto entry = entries.find(a_path.utf8());
        return Result<std::optional<FileInfo>>::success(
            entry == entries.end() ? std::nullopt
                                   : std::optional<FileInfo>{{entry->second.type, entry->second.text.size(), 0}});
    }
    /// @brief Test に不要な Directory 変更を拒否する
    Result<void> create_directories(const Path &) override
    {
        return unsupported();
    }
    /// @brief Test に不要な一覧を空で返す
    Result<std::vector<Path>> list_directory(const Path &) override
    {
        return Result<std::vector<Path>>::success({});
    }
    /// @brief Test に不要な削除を拒否する
    Result<bool> remove(const Path &) override
    {
        return Result<bool>::failure({ErrorCategory::InvalidState, "Test.remove"});
    }
    /// @brief Test に不要な移動を拒否する
    Result<void> rename(const Path &, const Path &) override
    {
        return unsupported();
    }
    /// @brief Test に不要な複写を拒否する
    Result<void> copy_file(const Path &, const Path &, bool) override
    {
        return unsupported();
    }
    /// @brief 保存した所有内容を File へ複写する
    Result<std::unique_ptr<IFile>> open(const Path &a_path, const FileOpenDesc &) override
    {
        auto entry = entries.find(a_path.utf8());
        if (failRead || entry == entries.end() || entry->second.type != FileType::Regular)
            return Result<std::unique_ptr<IFile>>::failure({ErrorCategory::PlatformFailure, "Test.open"});
        return Result<std::unique_ptr<IFile>>::success(std::make_unique<EditorMemoryFile>(entry->second.text));
    }
    /// @brief 失敗前は内容を保持し、成功時だけ完全な内容を公開する
    Result<FileSaveOutcome> replace_file(const Path &a_path, std::span<const std::byte> a_bytes) override
    {
        ++saveCount;
        if (onSave)
            onSave();
        if (failSave)
            return Result<FileSaveOutcome>::failure({ErrorCategory::PlatformFailure, "Test.replace"});
        std::string text(reinterpret_cast<const char *>(a_bytes.data()), a_bytes.size());
        entries[a_path.utf8()] = {FileType::Regular, std::move(text)};
        return Result<FileSaveOutcome>::success({isDurabilityConfirmed});
    }

  private:
    /// @brief Fixture が提供しない操作を失敗として返す
    static Result<void> unsupported()
    {
        return Result<void>::failure({ErrorCategory::InvalidState, "Test.unsupported"});
    }
};

/// @brief Test 専用の再現可能な UUID を発行する
inline EditorApplicationConfig editor_config(unsigned &a_ids, bool *a_failWorld = nullptr)
{
    EditorApplicationConfig config;
    config.contentRoot = editor_value(Path::create("/Content"));
    config.generateId = [&a_ids]()
    {
        char id[37]{};
        std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", ++a_ids);
        return Result<std::string>::success(id);
    };
    config.createWorld = [a_failWorld]()
    {
        return a_failWorld && *a_failWorld
                   ? Result<std::unique_ptr<World>>::failure({ErrorCategory::PlatformFailure, "Test.world_factory"})
                   : World::create();
    };
    return config;
}

/// @brief Document 内の指定 Object を Test の既知 ID で検索する
inline const SceneObjectDescriptor &editor_object(const EditorApplication &a_app, std::string_view a_id)
{
    const auto &objects = a_app.document().scene().descriptor().objects;
    return *std::find_if(objects.begin(), objects.end(),
                         [&](const auto &a_object) { return a_object.objectId == a_id; });
}
} // namespace cue::tests
