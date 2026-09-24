#pragma once

#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Runtime/FrameController.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace cue
{
/// @brief Window、Platform Service、Frame Controllerの所有とMainThread進行をまとめる
///
/// 構築Threadだけで初期化、Message処理、破棄を行う。WorkerはWindowに触れない
class WindowHost final
{
public:
    /// @brief 初期化で使用する引数を保持する
    explicit WindowHost(std::wstring_view a_options);

    /// @brief Workerを回収してからWindowとSystemを破棄する
    ~WindowHost();

    WindowHost(const WindowHost&) = delete;
    WindowHost& operator=(const WindowHost&) = delete;

    /// @brief WindowとServiceを生成し、Callback登録後にControllerを開始する
    [[nodiscard]] Result<void> initialize();

    /// @brief Window Messageを処理し、終了要求があればfalseを返す
    [[nodiscard]] Result<bool> pump_events();

    /// @brief 初期化後からshutdownまで有効なControllerへの非所有参照を返す
    [[nodiscard]] FrameController& frame_controller() noexcept;

    /// @brief Workerを停止・joinしてからWindowとSystemを破棄する
    [[nodiscard]] Result<void> shutdown();

private:
    /// @brief Frame進行の診断値をDebuggerへ出す
    void report_progress(const FrameProgress& a_progress);

    /// @brief Test用Frame数と単一Thread指定を読み取る
    [[nodiscard]] bool parse_options(std::wstring_view a_options);

    std::wstring m_options;
    std::unique_ptr<WindowSystem> m_system;
    std::unique_ptr<Window> m_window;
    WindowsThreadServices m_services;
    std::unique_ptr<FrameController> m_controller;
    std::uint64_t m_testFrameLimit = 0;
    std::uint64_t m_nextReportFrame = 60;
    bool m_useWorkerThreads = true;
    bool m_isStarted = false;
    bool m_isDestroyed = false;
    bool m_isCloseRequested = false;
};
} // namespace cue
