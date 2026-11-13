#include <EditorIde/ScriptCreation.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <EditorIde/ScriptSource.h>
#include <Project/Project.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
#define CHECK(...)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script creation failed %d: %s\n", __LINE__, #__VA_ARGS__);                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

constexpr std::string_view k_descriptor =
    "formatVersion=1\nprojectId=7f533c49-d416-47ea-98c1-026822b1e6ed\nname=Script "
    "Test\napplicationName=ScriptTest\nsourceRoot=Source\ncontentRoot=Content\ngameTarget=FixtureGame\n";
constexpr std::string_view k_catalog =
    "formatVersion=2\nscript.game.Existing=Existing.cpp\nregistration.game.Existing=Existing\n";

/// @brief Fixture の固定名だけを一意の Root へ結合する
cue::Path joined(const cue::Path &a_root, std::string_view a_relative)
{
    auto value = a_root.join(a_relative);
    return value.has_value() ? value.take_value() : cue::Path{};
}

/// @brief 入力と障害注入用の File を Fixture 内だけに配置する
bool write(cue::IFileSystem &a_files, const cue::Path &a_file, std::string_view a_text)
{
    return a_files.write_all(a_file, std::as_bytes(std::span(a_text.data(), a_text.size())), true).has_value();
}

/// @brief 失敗後に正本の Byte が保たれたことを比較する
std::string read(cue::IFileSystem &a_files, const cue::Path &a_file)
{
    auto bytes = a_files.read_all(a_file, 128 * 1024);
    if (!bytes.has_value())
        return {};
    const auto &data = *bytes.try_value();
    return std::string(data.empty() ? "" : reinterpret_cast<const char *>(data.data()), data.size());
}

/// @brief Result の所有期間内で存在値を取得し、問い合わせ失敗を未存在と区別する
std::optional<bool> file_exists(cue::IFileSystem &a_files, const cue::Path &a_file)
{
    auto result = a_files.exists(a_file);
    return result.has_value() ? std::optional<bool>(*result.try_value()) : std::nullopt;
}

/// @brief 部分 Write と Flush / Close の失敗を実 File の前後へ注入する
class FileProbe final : public cue::IFile
{
  public:
    /// @brief CreateNew で生成した Handle の一意所有を引き継ぐ
    FileProbe(std::unique_ptr<cue::IFile> a_file, std::size_t a_limit, bool a_flush, bool a_close)
        : m_file(std::move(a_file)), m_limit(a_limit), m_failFlush(a_flush), m_failClose(a_close)
    {
    }
    /// @brief 読み込みは所有 Handle へ渡す
    cue::Result<std::size_t> read(std::span<std::byte> a_bytes) override
    {
        return m_file->read(a_bytes);
    }
    /// @brief 指定 Byte 数を書いた後に失敗し、途中までの内容を実 File に残す
    cue::Result<std::size_t> write(std::span<const std::byte> a_bytes) override
    {
        if (m_written >= m_limit)
            return cue::Result<std::size_t>::failure({cue::ErrorCategory::PlatformFailure, "Probe.write"});
        auto value = m_file->write(a_bytes.first((std::min)(a_bytes.size(), m_limit - m_written)));
        if (value.has_value())
            m_written += *value.try_value();
        return value;
    }
    /// @brief Cursor 操作を所有 Handle へ渡す
    cue::Result<std::uint64_t> seek(std::int64_t a_offset, cue::SeekOrigin a_origin) override
    {
        return m_file->seek(a_offset, a_origin);
    }
    /// @brief Cursor 位置を所有 Handle へ渡す
    cue::Result<std::uint64_t> tell() override
    {
        return m_file->tell();
    }
    /// @brief Size を所有 Handle へ渡す
    cue::Result<std::uint64_t> size() override
    {
        return m_file->size();
    }
    /// @brief 完全 Write 後の Flush 失敗を注入する
    cue::Result<void> flush() override
    {
        return m_failFlush ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Probe.flush"})
                           : m_file->flush();
    }
    /// @brief 回収前に Close 失敗を報告し、Destructor の Handle 回収を維持する
    cue::Result<void> close() override
    {
        return m_failClose ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Probe.close"})
                           : m_file->close();
    }

  private:
    std::unique_ptr<cue::IFile> m_file;
    std::size_t m_limit;
    std::size_t m_written = 0;
    bool m_failFlush;
    bool m_failClose;
};

/// @brief Link / 書込み / 公開 / 回収の失敗を実 FileSystem に注入する
class CreationProbe final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief Fixture が所有する FileSystem を借用する
    explicit CreationProbe(cue::IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief 存在しない出力や既存の親も Link として拒否できるようにする
    cue::Result<std::optional<cue::FileInfo>> stat(const cue::Path &a_path) override
    {
        if (!linkedFilename.empty() && a_path.filename() == linkedFilename)
            return cue::Result<std::optional<cue::FileInfo>>::success(cue::FileInfo{cue::FileType::Link});
        return FileSystemProbe::stat(a_path);
    }
    /// @brief 新規 Source Handle だけを失敗注入用 Wrapper に置き換える
    cue::Result<std::unique_ptr<cue::IFile>> open(const cue::Path &a_path,
                                                  const cue::FileOpenDesc &a_desc = {}) override
    {
        auto value = FileSystemProbe::open(a_path, a_desc);
        if (!value.has_value() || a_path.filename() != probeFilename || a_desc.creation != cue::FileCreation::CreateNew)
            return value;
        std::unique_ptr<cue::IFile> probe =
            std::make_unique<FileProbe>(value.take_value(), writeLimit, failFlush, failClose);
        return cue::Result<std::unique_ptr<cue::IFile>>::success(std::move(probe));
    }
    /// @brief 外部編集が入った Source を Cleanup が削除しないことを検証する
    cue::Result<cue::FileSaveOutcome> replace_file(const cue::Path &a_path, std::span<const std::byte> a_bytes) override
    {
        if (!changedSource.is_empty())
            (void)write(files, changedSource, "// user changed this file\n");
        return FileSystemProbe::replace_file(a_path, a_bytes);
    }
    /// @brief 単一 File の回収に失敗した際の診断を確認する
    cue::Result<bool> remove(const cue::Path &a_path) override
    {
        if (failRemove)
            return cue::Result<bool>::failure({cue::ErrorCategory::PlatformFailure, "Probe.remove"});
        return FileSystemProbe::remove(a_path);
    }
    std::string linkedFilename;
    std::string probeFilename;
    std::size_t writeLimit = (std::numeric_limits<std::size_t>::max)();
    bool failFlush = false;
    bool failClose = false;
    bool failRemove = false;
    cue::Path changedSource;
};

/// @brief 正常作成・型対応・衝突と失敗後の正本保持を実際の FileSystem 上で確認する
int test_creation(cue::tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    const auto root = a_fixture.root;
    const auto source = joined(root, "Source");
    const auto map = joined(source, "ScriptSources.cueide");
    const auto cmake = joined(source, "CMakeLists.txt");
    const auto existing = joined(source, "Existing.cpp");
    const auto projectFile = joined(root, "Game.cueproj");
    CHECK(files.create_directories(source).has_value());
    CHECK(files.create_directories(joined(root, "Content")).has_value());
    CHECK(write(files, projectFile, k_descriptor));
    CHECK(write(files, existing, "// existing source\n"));
    CHECK(write(files, cmake, "cue_add_game_scripts(FixtureGame)\n"));
    CHECK(write(files, map, k_catalog));
    auto loaded = cue::Project::load(files, projectFile);
    CHECK(loaded.has_value());
    const auto project = loaded.take_value();
    CreationProbe probe(files);
    for (const auto *name : {"", "../Outside", "_Hidden", "Bad__Name", "class", "co_await", "CON", "lpt1", "9Wrong",
                             "Bad-Name", "Bad.cpp", "日本語", "A\nB", "Bad;Command"})
    {
        CHECK(!cue::is_valid_script_name(name));
        CHECK(!cue::create_script_source(probe, project, name).has_value());
        CHECK(read(files, map) == k_catalog);
    }
    CHECK(!cue::create_script_source(probe, project, std::string(65, 'A')).has_value());
    CHECK(cue::is_valid_script_name("RotateCube2"));
    auto created = cue::create_script_source(probe, project, "RotateCube");
    CHECK(created.has_value() && created.try_value()->utf8() == joined(source, "RotateCube.cpp").utf8());
    const auto header = read(files, joined(source, "RotateCube.h"));
    const auto generated = read(files, *created.try_value());
    CHECK(header.find("class RotateCube final : public ::cue::Behaviour") != std::string::npos);
    CHECK(header.find("update() override;") != std::string::npos);
    CHECK(header.find("CUE_DEFINE_SCRIPT") == std::string::npos);
    CHECK(generated.starts_with("#include \"RotateCube.h\"\n"));
    CHECK(generated.find("RotateCube::update()") != std::string::npos);
    CHECK(generated.find("CUE_DEFINE_SCRIPT(RotateCube,") != std::string::npos);
    CHECK(generated.find("CUE_DEFINE_SCRIPT_MODULE") == std::string::npos);
    auto resolved = cue::resolve_script_source(files, project, "game.RotateCube");
    CHECK(resolved.has_value() && resolved.try_value()->utf8() == created.try_value()->utf8());
    CHECK(cue::create_script_source(probe, project, "MoveCamera").has_value());
    // Engine Namespace と同じ Class 名でも、生成先の Namespace と完全修飾 API を混同しない
    auto namespaceName = cue::create_script_source(probe, project, "cue");
    CHECK(namespaceName.has_value());
    const auto namespaceSource = read(files, *namespaceName.try_value());
    CHECK(namespaceSource.find("namespace game\n") != std::string::npos);
    CHECK(read(files, joined(source, "cue.h")).find("class cue final : public ::cue::Behaviour") != std::string::npos);
    CHECK(namespaceSource.find("::cue::make_script_registration<::game::cue>") != std::string::npos);
    auto before = read(files, map);
    CHECK(!cue::create_script_source(probe, project, "rotatecube").has_value());
    CHECK(write(files, joined(source, "Manual.CPP"), "// user source\n"));
    CHECK(!cue::create_script_source(probe, project, "Manual").has_value());
    CHECK(file_exists(files, joined(source, "Manual.h")) == false);
    const auto manualHeader = joined(source, "ExistingHeader.H");
    CHECK(write(files, manualHeader, "// user header\n"));
    CHECK(!cue::create_script_source(probe, project, "ExistingHeader").has_value());
    CHECK(read(files, manualHeader) == "// user header\n");
    CHECK(file_exists(files, joined(source, "ExistingHeader.cpp")) == false);
    CHECK(read(files, map) == before);

    // 二つの Editor が同時に対応表を上書きせず、OS の非共有 Handle で直列化する
    const auto lockPath = joined(root, "Intermediate/Editor/ScriptCreation.lock");
    auto locked =
        files.open(lockPath, {cue::FileAccess::ReadWrite, cue::FileCreation::OpenExisting, false, false, false});
    CHECK(locked.has_value());
    CHECK(!cue::create_script_source(probe, project, "Locked").has_value());
    CHECK((*locked.try_value())->close().has_value());
    locked.try_value()->reset();
    CHECK(file_exists(files, joined(source, "Locked.cpp")) == false);

    probe.failSave = true;
    CHECK(!cue::create_script_source(probe, project, "SaveFailed").has_value());
    CHECK(file_exists(files, joined(source, "SaveFailed.cpp")) == false && read(files, map) == before);
    CHECK(file_exists(files, joined(source, "SaveFailed.h")) == false);
    probe.failSave = false;
    probe.failedFilename = "OpenFailed.cpp";
    CHECK(!cue::create_script_source(probe, project, "OpenFailed").has_value());
    CHECK(file_exists(files, joined(source, "OpenFailed.cpp")) == false);
    CHECK(file_exists(files, joined(source, "OpenFailed.h")) == false);
    probe.failedFilename.clear();

    for (const auto *name : {"WriteFailed", "FlushFailed", "CloseFailed"})
    {
        probe.probeFilename = std::string(name) + ".cpp";
        probe.writeLimit = std::string_view(name) == "WriteFailed" ? 117 : (std::numeric_limits<std::size_t>::max)();
        probe.failFlush = std::string_view(name) == "FlushFailed";
        probe.failClose = std::string_view(name) == "CloseFailed";
        CHECK(!cue::create_script_source(probe, project, name).has_value());
        CHECK(file_exists(files, joined(source, probe.probeFilename)) == false && read(files, map) == before);
        CHECK(file_exists(files, joined(source, std::string(name) + ".h")) == false);
    }
    probe.probeFilename.clear();
    probe.failFlush = false;
    probe.failClose = false;

    // Source 作成の間に外部 Editor が対応表を変更した場合は、変更を残して新規 Source だけを戻す
    bool changed = false;
    probe.onOpen = [&](const cue::Path &a_path)
    {
        if (!changed && a_path.filename() == "CatalogRace.cpp")
        {
            changed = true;
            (void)write(files, map, before + "# external edit\n");
        }
    };
    CHECK(!cue::create_script_source(probe, project, "CatalogRace").has_value());
    CHECK(changed && read(files, map) == before + "# external edit\n");
    CHECK(file_exists(files, joined(source, "CatalogRace.cpp")) == false);
    probe.onOpen = {};
    CHECK(write(files, map, before));

    probe.failSave = true;
    probe.changedSource = joined(source, "UserChanged.cpp");
    auto userChanged = cue::create_script_source(probe, project, "UserChanged");
    CHECK(!userChanged.has_value() && userChanged.try_error()->operation.find("cleanup:") != std::string::npos);
    CHECK(read(files, probe.changedSource) == "// user changed this file\n" && read(files, map) == before);
    CHECK(file_exists(files, joined(source, "UserChanged.h")) == false);
    probe.changedSource = {};
    probe.failRemove = true;
    auto cleanupFailed = cue::create_script_source(probe, project, "CleanupFailed");
    CHECK(!cleanupFailed.has_value() &&
          cleanupFailed.try_error()->operation.find("cleanup:Probe.remove") != std::string::npos);
    CHECK(file_exists(files, joined(source, "CleanupFailed.cpp")) == true);
    probe.failSave = false;
    probe.failRemove = false;
    for (const auto *filename :
         {"ScriptSources.cueide", "CMakeLists.txt", "Source", "Intermediate", "ScriptCreation.lock", "LinkTarget.cpp"})
    {
        probe.linkedFilename = filename;
        CHECK(!cue::create_script_source(probe, project, "LinkTarget").has_value());
        CHECK(read(files, map) == before);
    }
    probe.linkedFilename.clear();

    for (const auto &bad :
         {std::string("formatVersion=1\nscript.game.Existing=Existing.cpp\n"),
          std::string("formatVersion=2\nscript.game.Existing=Existing.cpp\n"),
          std::string(k_catalog) + "unknown=value\n", std::string(k_catalog) + "registration.game.Other=Existing\n",
          std::string("formatVersion=2\nscript.game.Existing=../Outside.cpp\nregistration.game.Existing=Existing\n"),
          std::string("formatVersion=2\nscript.game.Existing=Existing.cpp\nregistration.game.Existing=Bad;Command\n"),
          std::string(65537, 'x')})
    {
        CHECK(write(files, map, bad));
        CHECK(!cue::create_script_source(probe, project, "Rejected").has_value());
        CHECK(read(files, map) == bad && file_exists(files, joined(source, "Rejected.cpp")) == false);
    }
    CHECK(write(files, map, before));
    // v2 の型 ID は Runtime の保存契約と同じ 128 Byte を上限にする
    for (const auto length : {128U, 129U})
    {
        const std::string typeId(length, 'A');
        const auto bounded =
            "formatVersion=2\nscript." + typeId + "=Existing.cpp\nregistration." + typeId + "=Existing\n";
        CHECK(write(files, map, bounded));
        CHECK(cue::resolve_script_source(probe, project, typeId).has_value() == (length == 128));
        if (length == 129)
        {
            CHECK(!cue::create_script_source(probe, project, "TooLongId").has_value());
            CHECK(read(files, map) == bounded);
        }
    }
    CHECK(write(files, map, before));
    CHECK(write(files, cmake, "cue_add_game_module(FixtureGame SOURCES Existing.cpp)\n"));
    CHECK(!cue::create_script_source(probe, project, "Legacy").has_value());
    CHECK(read(files, map) == before && read(files, existing) == "// existing source\n");
    CHECK(read(files, projectFile) == k_descriptor);
    return 0;
}
} // namespace

/// @brief 一意の Fixture 以外の Source / Scene へ書き込まず、作成と Rollback を確認する
int main()
{
    cue::tests::TemporaryFiles fixture;
    CHECK(fixture.isCreated && fixture.files);
    return test_creation(fixture);
}
