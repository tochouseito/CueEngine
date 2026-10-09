#include <Platform/Diagnostics.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <Logging/Logger.h>

namespace
{
/// @brief 配送中の解除待機・再入・失敗を制御する
struct SinkState final
{
    std::vector<std::string> lines;
    std::mutex mutex;
    std::condition_variable condition;
    bool isBlocked = false;
    bool hasEntered = false;
    bool shouldReenter = false;
    bool shouldFail = false;
    bool shouldThrow = false;
    unsigned flushCount = 0;
};

/// @brief Logger の所有を変えず、同期配送を任意の位置で止める
class ControlledSink final : public cue::ILogSink
{
  public:
    /// @brief Logger より長く生存する Fixture を借用する
    explicit ControlledSink(SinkState &a_state) : m_state(a_state)
    {
    }
    /// @brief 行を保持し、再入または配送失敗を再現する
    cue::Result<void> write(std::string_view a_line) override
    {
        std::unique_lock lock(m_state.mutex);
        m_state.lines.emplace_back(a_line);
        if (m_state.shouldReenter)
        {
            cue::report_log("Nested", "must use emergency", cue::LogLevel::Error);
        }
        m_state.hasEntered = true;
        m_state.condition.notify_all();
        m_state.condition.wait(lock, [&]() { return !m_state.isBlocked; });
        if (m_state.shouldThrow)
        {
            throw std::runtime_error("sink");
        }
        return m_state.shouldFail ? cue::Result<void>::failure({cue::ErrorCategory::PlatformFailure, "Test.sink"})
                                  : cue::Result<void>::success();
    }
    /// @brief Fatal 配送が Flush まで届くことを観測する
    cue::Result<void> flush() override
    {
        ++m_state.flushCount;
        return cue::Result<void>::success();
    }
    /// @brief Fixture の所有先は回収せず停止だけを成功させる
    cue::Result<void> close() override
    {
        return cue::Result<void>::success();
    }

  private:
    SinkState &m_state;
};

/// @brief 旧 API と新 API の Metadata、並列報告と登録解除の寿命を検証する
int test_routing()
{
    SinkState state;
    std::vector<std::unique_ptr<cue::ILogSink>> sinks;
    sinks.push_back(std::make_unique<ControlledSink>(state));
    auto created = cue::Logger::create(std::move(sinks));
    if (!created.has_value())
    {
        return __LINE__;
    }
    auto logger = created.take_value();
    auto attached = cue::register_diagnostic_logger(*logger);
    if (!attached.has_value())
    {
        return __LINE__;
    }
    auto registration = attached.take_value();
    if (cue::register_diagnostic_logger(*logger).has_value())
    {
        return __LINE__;
    }
    cue::report_error("Legacy", {cue::ErrorCategory::PlatformFailure, "日本語", 123}, cue::DiagnosticSeverity::Warning);
    cue::report_message("Legacy", "message", cue::DiagnosticSeverity::Error);
    cue::report_log("New", "fatal", cue::LogLevel::Fatal);
    cue::report_log(nullptr, "ignored");
    cue::report_log("ignored", nullptr);
    if (state.lines.size() != 3 || state.lines[0].find("[Warning]") == std::string::npos ||
        state.lines[0].find("Legacy: 日本語") == std::string::npos ||
        state.lines[0].find("nativeCode=123") == std::string::npos || state.flushCount != 1)
    {
        return __LINE__;
    }
    state.shouldReenter = true;
    cue::report_log("Test", "outer");
    if (state.lines.size() != 4)
    {
        return __LINE__;
    }
    state.shouldReenter = false;
    state.shouldFail = true;
    cue::report_log("Test", "failure");
    state.shouldThrow = true;
    cue::report_log("Test", "exception");
    state.shouldFail = false;
    state.shouldThrow = false;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
    {
        workers.emplace_back(
            []()
            {
                for (int index = 0; index < 50; ++index)
                {
                    cue::report_log("Worker", "record");
                }
            });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }
    if (state.lines.size() != 206)
    {
        return __LINE__;
    }
    {
        std::lock_guard lock(state.mutex);
        state.isBlocked = true;
        state.hasEntered = false;
    }
    std::thread reporter([]() { cue::report_log("Gate", "held"); });
    {
        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, std::chrono::seconds(2), [&]() { return state.hasEntered; }))
        {
            state.isBlocked = false;
            lock.unlock();
            state.condition.notify_all();
            reporter.join();
            return __LINE__;
        }
    }
    std::atomic<bool> hasStarted = false;
    std::atomic<bool> wasDetached = false;
    std::thread remover(
        [&]()
        {
            hasStarted = true;
            registration.reset();
            wasDetached = true;
        });
    while (!hasStarted)
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool waited = !wasDetached.load();
    {
        std::lock_guard lock(state.mutex);
        state.isBlocked = false;
    }
    state.condition.notify_all();
    reporter.join();
    remover.join();
    if (!waited || !wasDetached || state.lines.size() != 207)
    {
        return __LINE__;
    }
    cue::report_log("After", "no owner");
    if (state.lines.size() != 207)
    {
        return __LINE__;
    }
    auto again = cue::register_diagnostic_logger(*logger);
    if (!again.has_value())
    {
        return __LINE__;
    }
    again.take_value().reset();
    return logger->shutdown().has_value() ? 0 : __LINE__;
}
} // namespace

/// @brief 独立 Process で一つの診断登録の契約を検証する
int main()
{
    const auto result = test_routing();
    if (result)
    {
        std::fprintf(stderr, "routing: %d\n", result);
    }
    return result;
}
