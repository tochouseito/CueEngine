#include <WindowsHost/WindowsHost.h>

#include <cstdio>
#include <string>

#include <EditorHost/EditorHost.h>
#include <Platform/Diagnostics.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief OS File を維持したまま Close だけを失敗させる
class CloseFaultFile final : public cue::IFile
{
  public:
    /// @brief 下位 File を所有し、Fixture の失敗設定を借用する
    CloseFaultFile(std::unique_ptr<cue::IFile> a_file, bool &a_hasFailure)
        : m_file(std::move(a_file)), m_hasFailure(a_hasFailure)
    {
    }
    /// @brief 下位の同期読込みへ委譲する
    cue::Result<std::size_t> read(std::span<std::byte> a_bytes) override
    {
        return m_file->read(a_bytes);
    }
    /// @brief 下位の同期書込みへ委譲する
    cue::Result<std::size_t> write(std::span<const std::byte> a_bytes) override
    {
        return m_file->write(a_bytes);
    }
    /// @brief 下位の位置移動へ委譲する
    cue::Result<std::uint64_t> seek(std::int64_t a_offset, cue::SeekOrigin a_origin) override
    {
        return m_file->seek(a_offset, a_origin);
    }
    /// @brief 下位の位置を返す
    cue::Result<std::uint64_t> tell() override
    {
        return m_file->tell();
    }
    /// @brief 下位の Size を返す
    cue::Result<std::uint64_t> size() override
    {
        return m_file->size();
    }
    /// @brief 書込み内容は常に Flush できる
    cue::Result<void> flush() override
    {
        return m_file->flush();
    }
    /// @brief Host の再試行まで Native Handle を保全する
    cue::Result<void> close() override
    {
        return m_hasFailure ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.close"})
                            : m_file->close();
    }

  private:
    std::unique_ptr<cue::IFile> m_file;
    bool &m_hasFailure;
};

/// @brief Host のログ File だけに Close 故障を接続する
class CloseFaultFiles final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief 実 FileSystem と故障設定を Host より長く借用する
    CloseFaultFiles(cue::IFileSystem &a_files, bool &a_hasFailure)
        : FileSystemProbe(a_files), m_hasFailure(a_hasFailure)
    {
    }
    /// @brief OS の File を開き、ログ File の場合だけ故障 Adapter で所有する
    cue::Result<std::unique_ptr<cue::IFile>> open(const cue::Path &a_path, const cue::FileOpenDesc &a_desc) override
    {
        auto opened = files.open(a_path, a_desc);
        if (!opened.has_value() || a_path.extension() != ".log")
        {
            return opened;
        }
        return cue::Result<std::unique_ptr<cue::IFile>>::success(
            std::make_unique<CloseFaultFile>(opened.take_value(), m_hasFailure));
    }

  private:
    bool &m_hasFailure;
};

/// @brief 所有 Logger が File に保存した行を Fixture の FileSystem から読む
std::string read_text(cue::IFileSystem &a_files, const cue::Path &a_path)
{
    auto bytes = a_files.read_all(a_path);
    return bytes.has_value()
               ? std::string(reinterpret_cast<const char *>(bytes.try_value()->data()), bytes.try_value()->size())
               : std::string{};
}

/// @brief Window 初期化の失敗が記録され、Rollback 後に File Handle と借用が回収されることを検証する
int test_rollback(cue::tests::TemporaryFiles &a_temporary)
{
    cue::WindowsHost *owner = nullptr;
    cue::Path logFile;
    bool hasLogger = false;
    cue::WindowsHostConfig config;
    config.window = {"Logger Rollback", {160, 120}};
    config.storage.dataRootOverride = a_temporary.root;
    config.callbacks.initializeWindow = [&](cue::Window &) -> cue::Result<void>
    {
        hasLogger = owner->logger() != nullptr && owner->log_file_path() != nullptr;
        if (hasLogger)
        {
            logFile = *owner->log_file_path();
        }
        cue::report_message("Legacy", "旧診断", cue::DiagnosticSeverity::Warning);
        cue::report_log_error("Test", {cue::ErrorCategory::PlatformFailure, "native", 42}, cue::LogLevel::Error);
        return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.initialize.failed"});
    };
    cue::WindowsHost host(std::move(config));
    owner = &host;
    auto result = host.initialize();
    if (result.has_value() || result.try_error()->operation != "Test.initialize.failed" || !hasLogger ||
        host.logger() || host.log_file_path() || !host.shutdown().has_value())
    {
        return __LINE__;
    }
    const auto text = read_text(*a_temporary.files, logFile);
    if (text.find("Legacy: 旧診断") == std::string::npos || text.find("nativeCode=42") == std::string::npos ||
        text.find("Test.initialize.failed") == std::string::npos ||
        text.find("WindowsHost: stopped") == std::string::npos)
    {
        return __LINE__;
    }
    auto file = a_temporary.files->open(logFile, {cue::FileAccess::Write});
    return file.has_value() && file.try_value()->get()->close().has_value() ? 0 : __LINE__;
}

/// @brief 実 Editor の通常ログと借用 Logger を同じ File に保存する
int test_editor(cue::tests::TemporaryFiles &a_temporary)
{
    cue::EditorHostConfig config;
    config.window.clientSize = {160, 120};
    config.storage.dataRootOverride = a_temporary.root;
    config.imgui.settingsFile.clear();
    config.frame.useWorkerThreads = false;
    config.frame.maxFps = 0;
    cue::EditorHost host(std::move(config));
    if (!host.initialize().has_value() || !host.logger() || !host.log_file_path())
    {
        return __LINE__;
    }
    const auto logFile = *host.log_file_path();
    if (!host.logger()->log(cue::LogLevel::Info, "Editor", "borrowed logger").has_value())
    {
        return __LINE__;
    }
    for (int frame = 0; frame < 3; ++frame)
    {
        auto step = host.step();
        if (!step.has_value() || !*step.try_value())
        {
            return __LINE__;
        }
    }
    if (!host.shutdown().has_value() || host.logger() || host.log_file_path())
    {
        return __LINE__;
    }
    const auto text = read_text(*a_temporary.files, logFile);
    return text.find("WindowsHost: initialized") != std::string::npos &&
                   text.find("Editor: borrowed logger") != std::string::npos &&
                   text.find("WindowsHost: stopped") != std::string::npos
               ? 0
               : __LINE__;
}

/// @brief Open 失敗の伝播、File 無効設定と Close 失敗後の再試行を検証する
int test_failures(cue::tests::TemporaryFiles &a_temporary)
{
    for (const bool isFileEnabled : {true, false})
    {
        auto files = std::make_unique<cue::tests::FileSystemProbe>(*a_temporary.files);
        files->failRead = true;
        bool hasReachedWindow = false;
        cue::WindowsHostConfig config;
        config.window = {"Logger Open Failure", {160, 120}};
        config.storage.dataRootOverride = a_temporary.root;
        config.fileSystem = std::move(files);
        config.logging.isFileEnabled = isFileEnabled;
        config.logging.debugOutput = cue::DebugLogOutput::Disabled;
        config.callbacks.initializeWindow = [&](cue::Window &)
        {
            hasReachedWindow = true;
            return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.window"});
        };
        cue::WindowsHost host(std::move(config));
        auto initialized = host.initialize();
        if (initialized.has_value() || hasReachedWindow == isFileEnabled ||
            initialized.try_error()->operation != (isFileEnabled ? "Probe.open" : "Test.window") ||
            !host.shutdown().has_value())
        {
            return __LINE__;
        }
    }
    bool hasCloseFailure = true;
    cue::WindowsHostConfig config;
    config.window = {"Logger Close Failure", {160, 120}};
    config.storage.dataRootOverride = a_temporary.root;
    config.fileSystem = std::make_unique<CloseFaultFiles>(*a_temporary.files, hasCloseFailure);
    config.callbacks.initializeWindow = [](cue::Window &)
    { return cue::Result<void>::failure({cue::ErrorCategory::InvalidState, "Test.primary"}); };
    cue::WindowsHost host(std::move(config));
    auto initialized = host.initialize();
    if (initialized.has_value() || initialized.try_error()->operation != "Test.primary" || !host.logger())
    {
        return __LINE__;
    }
    auto stopped = host.shutdown();
    if (stopped.has_value() || stopped.try_error()->operation != "Test.close")
    {
        return __LINE__;
    }
    hasCloseFailure = false;
    return host.shutdown().has_value() && !host.logger() && !host.log_file_path() ? 0 : __LINE__;
}
} // namespace

/// @brief Host が所有する File ログの起動・Rollback・Editor 接続と終了を検証する
int main()
{
    cue::tests::TemporaryFiles temporary;
    if (!temporary.isCreated)
    {
        return __LINE__;
    }
    if (const auto result = test_rollback(temporary))
    {
        std::fprintf(stderr, "rollback: %d\n", result);
        return result;
    }
    if (const auto result = test_editor(temporary))
    {
        std::fprintf(stderr, "editor: %d\n", result);
        return result;
    }
    if (const auto result = test_failures(temporary))
    {
        std::fprintf(stderr, "failures: %d\n", result);
        return result;
    }
    return 0;
}
