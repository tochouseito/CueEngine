#include <Platform/Windows/WindowsFileSystem.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>

#include <WindowsHost/StoragePaths.h>
#include <WindowsHost/WindowsHost.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
using namespace cue;

/// @brief Scripted File の転送・破棄と失敗を呼出後も観測する
struct MemoryState final
{
    std::vector<std::byte> data = std::vector<std::byte>(7, std::byte{42});
    std::uint64_t reportedSize = 7;
    bool failFlush = false;
    bool failClose = false;
    bool returnZeroWrite = false;
    unsigned destructions = 0;
};
/// @brief OS の成功条件に依存せず部分転送と後処理の失敗を再現する
class MemoryFile final : public IFile
{
  public:
    /// @brief Test State を破棄まで借用する
    explicit MemoryFile(MemoryState &a_state) : m_state(a_state)
    {
    }
    /// @brief Helper 失敗後も一意所有が回収されたことを記録する
    ~MemoryFile() override
    {
        ++m_state.destructions;
    }
    /// @brief 1 回で最大 2 Byte だけ転送する
    Result<std::size_t> read(std::span<std::byte> a_destination) override
    {
        const auto count = std::min({a_destination.size(), std::size_t(2), m_state.data.size() - m_offset});
        std::copy_n(m_state.data.begin() + m_offset, count, a_destination.begin());
        m_offset += count;
        return Result<std::size_t>::success(count);
    }
    /// @brief 1 回で最大 2 Byte 書込み、進捗なしも再現する
    Result<std::size_t> write(std::span<const std::byte> a_source) override
    {
        const auto count = m_state.returnZeroWrite ? 0 : std::min(a_source.size(), std::size_t(2));
        m_state.data.insert(m_state.data.end(), a_source.begin(), a_source.begin() + count);
        return Result<std::size_t>::success(count);
    }
    /// @brief この Fixture では Cursor 移動を使用しない
    Result<std::uint64_t> seek(std::int64_t, SeekOrigin) override
    {
        return Result<std::uint64_t>::failure({ErrorCategory::InvalidState, "Memory.seek"});
    }
    /// @brief Test Cursor を返す
    Result<std::uint64_t> tell() override
    {
        return Result<std::uint64_t>::success(m_offset);
    }
    /// @brief 実 Buffer と異なる Size も報告できる
    Result<std::uint64_t> size() override
    {
        return Result<std::uint64_t>::success(m_state.reportedSize);
    }
    /// @brief Flush 失敗を注入する
    Result<void> flush() override
    {
        return m_state.failFlush ? Result<void>::failure({ErrorCategory::PlatformFailure, "Memory.flush"})
                                 : Result<void>::success();
    }
    /// @brief Close 失敗を注入する
    Result<void> close() override
    {
        return m_state.failClose ? Result<void>::failure({ErrorCategory::PlatformFailure, "Memory.close"})
                                 : Result<void>::success();
    }

  private:
    MemoryState &m_state;
    std::size_t m_offset = 0;
};
/// @brief Portable Helper を検証する File Factory
struct MemoryFiles final : public tests::FileSystemProbe
{
  public:
    /// @brief File 操作以外は Test の Windows 実装へ委譲する
    explicit MemoryFiles(IFileSystem &a_files) : FileSystemProbe(a_files)
    {
    }
    /// @brief 呼出ごとに独立した Cursor を返す
    Result<std::unique_ptr<IFile>> open(const Path &, const FileOpenDesc & = {}) override
    {
        return Result<std::unique_ptr<IFile>>::success(std::make_unique<MemoryFile>(state));
    }
    MemoryState state;
};

/// @brief Helper の部分転送、Size 変化、上限、Flush / Close 失敗と回収を検証する
int test_helpers(IFileSystem &a_files, const Path &a_path)
{
    MemoryFiles memory(a_files);
    auto complete = memory.read_all(a_path);
    if (!complete.has_value() || complete.try_value()->size() != 7 || memory.state.destructions != 1 ||
        memory.read_all(a_path, 6).has_value())
    {
        return __LINE__;
    }
    memory.state.data.resize(5);
    if (memory.read_all(a_path).has_value())
    {
        return __LINE__;
    }
    memory.state.reportedSize = (std::numeric_limits<std::uint64_t>::max)();
    if (memory.read_all(a_path, (std::numeric_limits<std::size_t>::max)()).has_value())
    {
        return __LINE__;
    }
    memory.state.reportedSize = 7;
    memory.state.data.resize(9);
    if (memory.read_all(a_path).has_value())
    {
        return __LINE__;
    }
    memory.state.data.resize(7);
    memory.state.failClose = true;
    if (memory.read_all(a_path).has_value())
    {
        return __LINE__;
    }
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    memory.state = {};
    memory.state.data.clear();
    if (!memory.write_all(a_path, bytes).has_value() ||
        memory.state.data != std::vector<std::byte>(bytes.begin(), bytes.end()))
    {
        return __LINE__;
    }
    memory.state.failFlush = true;
    if (memory.write_all(a_path, bytes).has_value())
    {
        return __LINE__;
    }
    memory.state.failFlush = false;
    memory.state.failClose = true;
    if (memory.write_all(a_path, bytes).has_value())
    {
        return __LINE__;
    }
    memory.state.failClose = false;
    memory.state.returnZeroWrite = true;
    if (memory.write_all(a_path, bytes).has_value())
    {
        return __LINE__;
    }
    return 0;
}

/// @brief Native File の転送、置換失敗時の旧内容保全と再試行を検証する
int test_native(tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    auto directory = a_fixture.root.join("資料/設定");
    if (!directory.has_value() || !files.create_directories(*directory.try_value()).has_value() ||
        !files.create_directories(*directory.try_value()).has_value())
    {
        return __LINE__;
    }
    auto targetResult = directory.try_value()->join("保存.bin");
    if (!targetResult.has_value())
    {
        return __LINE__;
    }
    const auto target = targetResult.take_value();
    const std::array first{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array second{std::byte{5}, std::byte{6}, std::byte{7}};
    auto published = files.replace_file(target, first);
    if (!published.has_value() || published.try_value()->isDurabilityConfirmed ||
        !files.replace_file(target, second).has_value())
    {
        return __LINE__;
    }
    auto bytes = files.read_all(target);
    if (!bytes.has_value() || *bytes.try_value() != std::vector<std::byte>(second.begin(), second.end()) ||
        files.read_all(target, 2).has_value())
    {
        return __LINE__;
    }
    auto info = files.stat(target);
    if (!info.has_value() || !*info.try_value() || (**info.try_value()).size != 3 ||
        (**info.try_value()).type != FileType::Regular)
    {
        return __LINE__;
    }
    auto opened = files.open(target, {FileAccess::ReadWrite, FileCreation::OpenExisting});
    if (!opened.has_value())
    {
        return __LINE__;
    }
    auto file = opened.take_value();
    std::array<std::byte, 8> buffer{};
    auto read = file->read(buffer);
    auto eof = file->read(buffer);
    auto begin = file->seek(-2, SeekOrigin::End);
    auto position = file->tell();
    if (!read.has_value() || *read.try_value() != 3 || !eof.has_value() || *eof.try_value() != 0 ||
        !begin.has_value() || *begin.try_value() != 1 || !position.has_value() || *position.try_value() != 1 ||
        file->seek(-10, SeekOrigin::Begin).has_value() || file->seek(0, static_cast<SeekOrigin>(99)).has_value())
    {
        return __LINE__;
    }
    // Delete Share がない Handle は公開を阻止する。旧内容と一時 File の回収を確認する
    auto blocked = files.replace_file(target, first);
    if (blocked.has_value() || !file->close().has_value() || !file->close().has_value() ||
        file->read(buffer).has_value() || file->size().has_value() || file->flush().has_value())
    {
        return __LINE__;
    }
    auto retained = files.read_all(target);
    auto entries = files.list_directory(*directory.try_value());
    if (!retained.has_value() || *retained.try_value() != std::vector<std::byte>(second.begin(), second.end()) ||
        !entries.has_value() || entries.try_value()->size() != 1 || !files.replace_file(target, first).has_value())
    {
        return __LINE__;
    }
    auto copy = directory.try_value()->join("copy.bin");
    auto moved = directory.try_value()->join("moved.bin");
    if (!copy.has_value() || !moved.has_value() || !files.copy_file(target, *copy.try_value(), false).has_value() ||
        files.copy_file(target, *copy.try_value(), false).has_value() ||
        !files.copy_file(target, *copy.try_value(), true).has_value() ||
        files.rename(target, *copy.try_value()).has_value() ||
        !files.rename(*copy.try_value(), *moved.try_value()).has_value() ||
        files.remove(*directory.try_value()).has_value() || !files.remove(*moved.try_value()).has_value())
    {
        return __LINE__;
    }
    auto missing = files.stat(*moved.try_value());
    auto removed = files.remove(*moved.try_value());
    if (!missing.has_value() || missing.try_value()->has_value() || !removed.has_value() || *removed.try_value())
    {
        return __LINE__;
    }
    for (const auto name : {"NUL.txt", "CON", "x:y", "tail.", "tail ", "bad?", "COM1"})
    {
        auto invalid = directory.try_value()->join(name);
        if (invalid.has_value() &&
            files.open(*invalid.try_value(), {FileAccess::Write, FileCreation::CreateAlways}).has_value())
        {
            return __LINE__;
        }
    }
    if (files.open(*directory.try_value()).has_value() ||
        files.open(target, {FileAccess::Read, FileCreation::CreateAlways}).has_value() ||
        files.open(target, {FileAccess::Write, FileCreation::CreateNew}).has_value() || files.stat(Path{}).has_value())
    {
        return __LINE__;
    }
    auto longPath = a_fixture.root.join(std::string(100, 'a') + '/' + std::string(100, 'b') + '/' +
                                        std::string(100, 'c') + "/long.bin");
    if (!longPath.has_value() || !files.write_all(*longPath.try_value(), first, true).has_value() ||
        !files.read_all(*longPath.try_value()).has_value())
    {
        return __LINE__;
    }
    bool succeeded = false;
    std::thread worker([&]() { succeeded = files.read_all(target).has_value(); });
    worker.join();
    if (!succeeded)
    {
        return __LINE__;
    }
    return test_helpers(files, target);
}

/// @brief 保存先 Mode、Override、取得・作成失敗と CWD 非依存を確認する
int test_storage(tests::TemporaryFiles &a_fixture)
{
    auto &files = *a_fixture.files;
    StoragePathsConfig config;
    config.mode = StorageMode::Development;
    config.repositoryRoot = a_fixture.root;
    auto development = resolve_storage_paths(files, config);
    if (!development.has_value() || development.try_value()->logs.utf8() != a_fixture.root.utf8() + "/out/logs")
    {
        return __LINE__;
    }
    config.mode = StorageMode::Product;
    config.companyName = "TestCompany";
    config.applicationName = "TestApplication";
    auto local = files.local_data_directory();
    auto product = resolve_storage_paths(files, config);
    if (!local.has_value() || !product.has_value() ||
        product.try_value()->dataRoot.utf8() != local.try_value()->utf8() + "/TestCompany/TestApplication")
    {
        return __LINE__;
    }
    config.mode = StorageMode::Portable;
    auto executable = files.executable_directory();
    auto portable = resolve_storage_paths(files, config);
    if (!executable.has_value() || !portable.has_value() ||
        portable.try_value()->dataRoot.utf8() != executable.try_value()->utf8())
    {
        return __LINE__;
    }
    std::wstring previous(32768, L'\0');
    const auto length = GetCurrentDirectoryW(static_cast<DWORD>(previous.size()), previous.data());
    if (!length || length >= previous.size())
    {
        return __LINE__;
    }
    previous.resize(length);
    auto native = utf8_to_utf16(a_fixture.root.utf8());
    if (!native.has_value() || !SetCurrentDirectoryW(native.try_value()->c_str()))
    {
        return __LINE__;
    }
    auto differentCwd = resolve_storage_paths(files, config);
    const auto restored = SetCurrentDirectoryW(previous.c_str());
    if (!restored || !differentCwd.has_value() ||
        differentCwd.try_value()->logs.utf8() != portable.try_value()->logs.utf8())
    {
        return __LINE__;
    }
    tests::FileSystemProbe probe(files);
    probe.failDirectory = true;
    if (resolve_storage_paths(probe, config).has_value())
    {
        return __LINE__;
    }
    config.dataRootOverride = a_fixture.root;
    auto overridden = resolve_storage_paths(probe, config);
    if (!overridden.has_value() ||
        overridden.try_value()->shaderCache.utf8() != a_fixture.root.utf8() + "/cache/shaders")
    {
        return __LINE__;
    }
    probe.failCreate = true;
    if (create_storage_directories(probe, *overridden.try_value()).has_value())
    {
        return __LINE__;
    }
    probe.failCreate = false;
    if (!create_storage_directories(probe, *overridden.try_value()).has_value())
    {
        return __LINE__;
    }
    auto relative = Path::create("relative");
    config.logsOverride = relative.take_value();
    if (resolve_storage_paths(files, config).has_value())
    {
        return __LINE__;
    }
    config.logsOverride = {};
    for (const auto name : {"../escape", "CON.txt", "COM¹", "App.x\n"})
    {
        config.applicationName = name;
        if (resolve_storage_paths(files, config).has_value())
        {
            return __LINE__;
        }
    }
    config.applicationName = "TestApplication";
    config.shaderCacheOverride = a_fixture.root;
    auto custom = resolve_storage_paths(files, config);
    if (!custom.has_value() || custom.try_value()->shaderCache.utf8() != a_fixture.root.utf8())
    {
        return __LINE__;
    }
    WindowsHostConfig hostConfig;
    hostConfig.storage.dataRootOverride = a_fixture.root;
    auto failedFiles = std::make_unique<tests::FileSystemProbe>(files);
    failedFiles->failCreate = true;
    hostConfig.fileSystem = std::move(failedFiles);
    bool wasWindowInitialized = false;
    hostConfig.callbacks.initializeWindow = [&](Window &)
    {
        wasWindowInitialized = true;
        return Result<void>::success();
    };
    WindowsHost host(std::move(hostConfig));
    auto initialization = host.initialize();
    if (initialization.has_value() || initialization.try_error()->operation != "Probe.create_directories" ||
        wasWindowInitialized || host.file_system() || !host.shutdown().has_value())
    {
        return __LINE__;
    }
    return 0;
}
} // namespace
/// @brief File の正常・失敗・再試行と Host 保存先を検証する
int main()
{
    cue::tests::TemporaryFiles fixture;
    const auto native = fixture.isCreated ? test_native(fixture) : __LINE__;
    const auto result = native ? native : test_storage(fixture);
    if (result)
    {
        std::fprintf(stderr, "FileSystemTests line %d\n", result);
    }
    return result;
}
