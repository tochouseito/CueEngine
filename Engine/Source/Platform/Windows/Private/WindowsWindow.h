#pragma once

#include <Cue/Platform/WindowSystem.h>

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace cue
{
inline constexpr wchar_t k_windowsDialogOwnerGenerationProperty[] = L"CueEngine.DialogOwnerGeneration";

class AssertContext;
class FileDialogOwnerToken;
class WindowsMessageSink;
class WindowsWindow;

/// @brief Win32 の Window Class、Main Window、Message Queue を Platform 契約へ適合させる実装
///
/// Win32 Window は作成 Thread に所属するため、生成時の Thread ID を保持して全操作の所属を検証する
/// M02 の単一 Main Window 制約と Window Class 参照寿命もこの System が管理する
class WindowsWindowSystem final : public WindowSystem
{
  public:
    /// @brief WindowsWindowSystem を必要な依存と初期状態から構築する
    WindowsWindowSystem(const AssertContext &a_assertContext, HINSTANCE a_instance) noexcept;
    /// @brief WindowsWindowSystem が保持する Resource を所有権規則に従って破棄する
    ~WindowsWindowSystem() noexcept override;

    /// @brief Win32 Window で使用する Window を生成し、呼び出し元へ返す
    [[nodiscard]] Result<std::unique_ptr<Window>> create_window(const WindowDescriptor &a_descriptor) noexcept override;
    /// @brief Win32 Window の Events を規定された順序と失敗規則で処理する
    [[nodiscard]] Result<PumpStatus> pump_events() noexcept override;

    /// @brief Win32 Window が保持する Assert Context を呼び出し元へ返す
    [[nodiscard]] const AssertContext &assert_context() const noexcept;
    /// @brief Win32 Window が保持する Instance を呼び出し元へ返す
    [[nodiscard]] HINSTANCE instance() const noexcept;
    /// @brief Win32 Window が保持する Thread ID を呼び出し元へ返す
    [[nodiscard]] DWORD thread_id() const noexcept;
    /// @brief Win32 Window の Window を整合性を保って更新する
    void publish_window(WindowsWindow &a_window) noexcept;
    /// @brief Win32 Window の Window を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> release_window(WindowsWindow &a_window) noexcept;

  private:
    /// @brief Win32 Window 生成に必要な Class を Process へ登録し、共有参照を確立する
    [[nodiscard]] Result<void> register_window_class() noexcept;
    /// @brief Win32 Window の Window Class を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> unregister_window_class() noexcept;

    // Window と WindowSystem の全寿命を通して、契約違反と Native Error を同じ経路へ報告する
    const AssertContext *m_assertContext;
    // Window Class と HWND を同じ実行 Module へ所属させるため保持する
    HINSTANCE m_instance;
    // Win32 Window 操作を生成 Thread へ限定し、Thread Queue と HWND の所属を一致させる
    DWORD m_threadId;
    // M02 の単一 Main Window 制約を検証する非所有参照
    WindowsWindow *m_window = nullptr;
};

/// @brief HWND の寿命と Win32 Message から変換した WindowEvent Queue を所有する実装
///
/// Native Callback 中に通常の Runtime Event Callback を呼び戻さず、Message を値 Event へ変換する
/// 診断と回復不能 Error の経路では Logger または FatalHandler を同期呼び出す場合がある
class WindowsWindow final : public Window
{
  public:
    /// @brief WindowsWindow を必要な依存と初期状態から構築する
    explicit WindowsWindow(WindowsWindowSystem &a_system) noexcept;
    /// @brief WindowsWindow が保持する Resource を所有権規則に従って破棄する
    ~WindowsWindow() noexcept override;

    /// @brief 生成済み Window を表示可能な状態へ移し、Native 表示結果を返す
    [[nodiscard]] Result<void> show() noexcept override;
    /// @brief Win32 Window を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> destroy() noexcept override;
    /// @brief Win32 Window が保持する State を呼び出し元へ返す
    [[nodiscard]] WindowState state() const noexcept override;
    /// @brief Win32 Window が保持する Client Size を呼び出し元へ返す
    [[nodiscard]] WindowSize client_size() const noexcept override;
    /// @brief Win32 Window の Pop Event へ安全に Access できる場合だけ参照を返す
    [[nodiscard]] bool try_pop_event(WindowEvent &a_event) noexcept override;
    /// @brief Platform 非依存 Handle 表現から Win32 Window Handle 値を復元する
    [[nodiscard]] const void *native_view_value() const noexcept;
    /// @brief Native Dialog Owner Tokenと照合するWindow Object非依存Generationを返す
    [[nodiscard]] std::uint64_t dialog_owner_generation() const noexcept;

    /// @brief 共有 Win32 Window Class の参照 Count を増やし、利用可能な登録状態を確保する
    void acquire_class_reference() noexcept;
    /// @brief Win32 Window で使用する Native を生成し、呼び出し元へ返す
    [[nodiscard]] Result<void> create_native(std::wstring_view a_title, int a_width, int a_height) noexcept;
    /// @brief Win32 Window を整合性を保って更新する
    void publish() noexcept;

    /// @brief Win32 Message を対象 Window Instance へ配送し、既定処理との境界を管理する
    [[nodiscard]] static LRESULT CALLBACK window_procedure(HWND a_window, UINT a_message, WPARAM a_wParam,
                                                           LPARAM a_lParam) noexcept;

  private:
    friend Result<FileDialogOwnerToken> create_windows_file_dialog_owner(Window &a_window,
                                                                         const AssertContext &a_assertContext) noexcept;
    friend Result<void> attach_windows_message_sink(Window &a_window, WindowsMessageSink &a_sink,
                                                    const AssertContext &a_assertContext) noexcept;
    friend Result<void> detach_windows_message_sink(Window &a_window, WindowsMessageSink &a_sink,
                                                    const AssertContext &a_assertContext) noexcept;

    /// @brief Owner Thread上でMessage Sinkを一意に関連付ける
    [[nodiscard]] Result<void> attach_message_sink(WindowsMessageSink &a_sink) noexcept;
    /// @brief Owner Thread上で同一Message Sinkの関連付けを解除する
    [[nodiscard]] Result<void> detach_message_sink(WindowsMessageSink &a_sink) noexcept;
    /// @brief Win32 Window の Message を規定された順序と失敗規則で処理する
    [[nodiscard]] LRESULT process_message(UINT a_message, WPARAM a_wParam, LPARAM a_lParam) noexcept;
    /// @brief Win32 Window の Event を整合性を保って更新する
    void push_event(WindowEvent a_event) noexcept;
    /// @brief Win32 Window の System Reference を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> release_system_reference() noexcept;
    /// @brief Win32 Window の Thread が期待する契約を満たすか検証する
    void verify_thread() const noexcept;

    // System は本 Window より長く生存し、Thread 検証と診断 Context を提供する
    WindowsWindowSystem *m_system;
    // 読み取り済み範囲を Index で保持し、Event 取得ごとの先頭要素削除を避ける
    std::vector<WindowEvent> m_events;
    std::size_t m_eventReadIndex = 0;
    // HWND は WM_NCDESTROY まで有効であり、WindowsWindow 自身だけが破棄を管理する
    HWND m_window = nullptr;
    // HWND値とObject Addressが再利用されてもTokenを別Windowへ転用させない単調Generation
    std::uint64_t m_dialogOwnerGeneration = 0U;
    WindowSize m_clientSize = {};
    WindowState m_state = WindowState::Destroyed;
    // Native 生成失敗時にも Window Class の参照を一度だけ解放するため個別に追跡する
    bool m_hasClassReference = false;
    // 生成中の Win32 Message を Runtime Event として公開しないため、生成完了後に切り替える
    bool m_isPublished = false;
    // OS が同じ閉じる Message を繰り返しても未処理要求を一件に保つ
    bool m_isClosePending = false;
    // SIZE_RESTORED を通常 Resize と最小化復帰へ分類するため直前状態を保持する
    bool m_isMinimized = false;
    // Tool UI SinkはWindowより短命であり、attachからdetachまでだけ非所有で参照する
    WindowsMessageSink *m_messageSink = nullptr;
    // Callback中の関連付け変更をProgramming Contract違反として検出する
    bool m_isDispatchingMessageSink = false;
};
} // namespace cue
