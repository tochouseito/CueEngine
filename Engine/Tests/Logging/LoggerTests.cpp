#include <Logging/FileLogSink.h>
#include <Logging/Logger.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#include <Platform/Windows/WindowsDebugLogSink.h>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"

namespace
{
/// @brief Sink を破棄した後も配送結果を保持する
struct SinkState final
{
    std::vector<std::string> lines;
    bool failWrite = false;
    bool failFlush = false;
    bool throwWrite = false;
    unsigned closeCount = 0;
    std::function<void()> onWrite;
};

/// @brief 配送失敗と再入を再現し、別 Sink への配送継続を観測する
class MemorySink final : public cue::ILogSink
{
  public:
    /// @brief 外部 Fixture の State を借用する
    explicit MemorySink(SinkState &a_state) : m_state(a_state)
    {
    }
    /// @brief 借用行を複製してから指定した失敗を返す
    cue::Result<void> write(std::string_view a_line) override
    {
        m_state.lines.emplace_back(a_line);
        if (m_state.onWrite)
        {
            m_state.onWrite();
        }
        if (m_state.throwWrite)
        {
            throw std::runtime_error("sink");
        }
        return m_state.failWrite ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.write"})
                                 : cue::Result<void>::success();
    }
    /// @brief Flush の失敗を注入する
    cue::Result<void> flush() override
    {
        return m_state.failFlush ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.flush"})
                                 : cue::Result<void>::success();
    }
    /// @brief 停止が全 Sink に届くことを数える
    cue::Result<void> close() override
    {
        ++m_state.closeCount;
        return cue::Result<void>::success();
    }

  private:
    SinkState &m_state;
};

/// @brief File の転送と失敗後の回収を観測する
struct FileState final
{
    std::string text;
    bool returnZero = false;
    bool failWrite = false;
    bool failFlush = false;
    bool failClose = false;
    unsigned closeCount = 0;
};

/// @brief 二 Byte の部分書込み、進捗なし、Flush / Close 失敗を再現する
class ScriptedFile final : public cue::IFile
{
  public:
    /// @brief 転送結果を File より長く保持する State を借用する
    explicit ScriptedFile(FileState &a_state) : m_state(a_state)
    {
    }
    /// @brief ログ Sink は読込みを行わない
    cue::Result<std::size_t> read(std::span<std::byte>) override
    {
        return cue::Result<std::size_t>::success(0);
    }
    /// @brief 一度に最大二 Byte だけ追記する
    cue::Result<std::size_t> write(std::span<const std::byte> a_bytes) override
    {
        if (m_state.failWrite)
        {
            return cue::Result<std::size_t>::failure({cue::ErrorCategory::PlatformFailure, "Test.file.write"});
        }
        const auto count = m_state.returnZero ? 0 : std::min(a_bytes.size(), std::size_t(2));
        m_state.text.append(reinterpret_cast<const char *>(a_bytes.data()), count);
        return cue::Result<std::size_t>::success(count);
    }
    /// @brief 末尾への移動を成功させる
    cue::Result<std::uint64_t> seek(std::int64_t, cue::SeekOrigin) override
    {
        return size();
    }
    /// @brief 追記した位置を返す
    cue::Result<std::uint64_t> tell() override
    {
        return size();
    }
    /// @brief 保持する文字列の Size を返す
    cue::Result<std::uint64_t> size() override
    {
        return cue::Result<std::uint64_t>::success(m_state.text.size());
    }
    /// @brief OS Flush の失敗を再現する
    cue::Result<void> flush() override
    {
        return m_state.failFlush ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.file.flush"})
                                 : cue::Result<void>::success();
    }
    /// @brief 再試行可能な Close の失敗を再現する
    cue::Result<void> close() override
    {
        ++m_state.closeCount;
        return m_state.failClose ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.file.close"})
                                 : cue::Result<void>::success();
    }

  private:
    FileState &m_state;
};

/// @brief Native Directory 操作は維持し、開いた File だけ制御可能な実装へ置換する
class ScriptedFiles final : public cue::tests::FileSystemProbe
{
  public:
    /// @brief 実 FileSystem と Script の寿命を Fixture に委ねる
    ScriptedFiles(cue::IFileSystem &a_files, FileState &a_state) : FileSystemProbe(a_files), m_state(a_state)
    {
    }
    /// @brief File の一意所有を公開する
    cue::Result<std::unique_ptr<cue::IFile>> open(const cue::Path &, const cue::FileOpenDesc &) override
    {
        return cue::Result<std::unique_ptr<cue::IFile>>::success(std::make_unique<ScriptedFile>(m_state));
    }

  private:
    FileState &m_state;
};

/// @brief フィルタ・整形・並列配送・再入拒否・失敗伝播を検証する
int test_logger()
{
    SinkState first, second;
    std::vector<std::unique_ptr<cue::ILogSink>> sinks;
    sinks.push_back(std::make_unique<MemorySink>(first));
    sinks.push_back(std::make_unique<MemorySink>(second));
    auto created = cue::Logger::create(std::move(sinks));
    if (!created.has_value())
    {
        return __LINE__;
    }
    auto logger = created.take_value();
    if (!logger->log(cue::LogLevel::Debug, "Test", "filtered").has_value() || !first.lines.empty())
    {
        return __LINE__;
    }
    cue::LogRecord record{cue::LogLevel::Warning, "日本語", "line\nnext\r"};
    record.time = std::chrono::system_clock::time_point{};
    record.nativeCode = -7;
    if (!logger->write(record).has_value() || first.lines != second.lines ||
        first.lines[0].find("1970-01-01T00:00:00.000Z [Warning]") != 0 ||
        first.lines[0].find("日本語: line\\nnext\\r [nativeCode=-7]") == std::string::npos ||
        std::count(first.lines[0].begin(), first.lines[0].end(), '\n') != 1)
    {
        return __LINE__;
    }
    std::atomic<bool> hasFailure = false;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker)
    {
        threads.emplace_back(
            [&]()
            {
                for (int index = 0; index < 100; ++index)
                {
                    if (!logger->log(cue::LogLevel::Info, "Worker", "record").has_value())
                    {
                        hasFailure = true;
                    }
                }
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    if (hasFailure || first.lines.size() != 401 || first.lines != second.lines)
    {
        return __LINE__;
    }
    bool wasRejected = false;
    first.onWrite = [&]() { wasRejected = !logger->log(cue::LogLevel::Error, "Test", "recursive").has_value(); };
    if (!logger->log(cue::LogLevel::Info, "Test", "outer").has_value() || !wasRejected)
    {
        return __LINE__;
    }
    first.onWrite = {};
    first.failWrite = true;
    if (logger->log_error(cue::LogLevel::Error, "Test", {cue::ErrorCategory::Fatal, "failure", 42}).has_value() ||
        first.lines != second.lines || second.lines.back().find("nativeCode=42") == std::string::npos)
    {
        return __LINE__;
    }
    first.throwWrite = true;
    if (logger->log(cue::LogLevel::Info, "Test", "exception").has_value() || first.lines != second.lines)
    {
        return __LINE__;
    }
    first.failFlush = true;
    if (logger->flush().has_value() || logger->shutdown().has_value() || first.closeCount != 1 ||
        second.closeCount != 1 || logger->log(cue::LogLevel::Fatal, "Test", "after close").has_value())
    {
        return __LINE__;
    }
    return logger->shutdown().has_value() ? 0 : __LINE__;
}

/// @brief 実 File の UTF-8、既存内容保全、追記と部分転送の失敗契約を確認する
int test_file_sink()
{
    cue::tests::TemporaryFiles temporary;
    if (!temporary.isCreated)
    {
        return __LINE__;
    }
    auto joined = temporary.root.join("日本語/ログ.txt");
    if (!joined.has_value())
    {
        return __LINE__;
    }
    auto path = joined.take_value();
    auto created = cue::FileLogSink::create(*temporary.files, path);
    if (!created.has_value())
    {
        return __LINE__;
    }
    auto sink = created.take_value();
    if (!sink->write("日本語\n").has_value() || !sink->close().has_value() ||
        cue::FileLogSink::create(*temporary.files, path).has_value())
    {
        return __LINE__;
    }
    auto append = cue::FileLogSink::create(*temporary.files, path, true);
    if (!append.has_value() || !append.try_value()->get()->write("append\n").has_value() ||
        !append.try_value()->get()->close().has_value())
    {
        return __LINE__;
    }
    auto text = temporary.files->read_all(path);
    const std::string expected = "日本語\nappend\n";
    if (!text.has_value() ||
        std::string(reinterpret_cast<const char *>(text.try_value()->data()), text.try_value()->size()) != expected)
    {
        return __LINE__;
    }
    FileState state;
    ScriptedFiles files(*temporary.files, state);
    auto scriptedResult = cue::FileLogSink::create(files, path);
    if (!scriptedResult.has_value())
    {
        return __LINE__;
    }
    auto scripted = scriptedResult.take_value();
    if (!scripted->write("1234567\n").has_value() || state.text != "1234567\n")
    {
        return __LINE__;
    }
    state.returnZero = true;
    if (scripted->write("bad").has_value())
    {
        return __LINE__;
    }
    state.returnZero = false;
    if (scripted->write("no retry").has_value() || state.text != "1234567\n")
    {
        return __LINE__;
    }
    state.failFlush = true;
    state.failClose = true;
    if (scripted->close().has_value() || state.closeCount != 1)
    {
        return __LINE__;
    }
    state.failFlush = false;
    state.failClose = false;
    if (!scripted->close().has_value() || state.closeCount != 2 || !scripted->close().has_value())
    {
        return __LINE__;
    }
    return 0;
}
} // namespace

/// @brief 抽象 Logger と Native 出力先を同じ配送契約で検証する
int main()
{
    if (const auto result = test_logger())
    {
        std::fprintf(stderr, "logger: %d\n", result);
        return result;
    }
    if (const auto result = test_file_sink())
    {
        std::fprintf(stderr, "file sink: %d\n", result);
        return result;
    }
    auto debug = cue::create_windows_debug_log_sink();
    if (!debug->write("日本語\n").has_value() || debug->write(std::string_view("\xff", 1)).has_value() ||
        !debug->close().has_value() || debug->write("closed").has_value())
    {
        return __LINE__;
    }
    auto disabled = cue::create_windows_debug_log_sink(false);
    return disabled->write("disabled").has_value() ? 0 : __LINE__;
}
