#pragma once

#include <Foundation/Result.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>
#include <Platform/Waiter.h>
#include <Platform/WindowSystem.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace cue
{
/// @brief Windows 専用の Message 値を呼出中だけ借用する
struct WindowsMessage final
{
    void* window = nullptr;
    std::uint32_t message = 0;
    std::uintptr_t wParam = 0;
    std::intptr_t lParam = 0;
};

/// @brief 通常 Message の標準処理を省略するかと返却値を指定する
struct WindowsMessageResult final
{
    bool isHandled = false;
    std::intptr_t result = 0;
};

using windowsMessageHandler = std::function<WindowsMessageResult(const WindowsMessage&)>;

/// @brief Window の生存中だけ有効な Handler 登録の非所有 Token
struct WindowsMessageHandlerToken final
{
    const Window* window = nullptr;
    std::uint64_t generation = 0;
};

/// @brief 単一の外部 Handler を登録し、解除まで Callback を Window が保持する
///
/// Window の生成 Thread から呼ぶ。Message 処理中の登録・解除と二重登録を拒否する
/// Callback の借用先は解除まで生存させる。例外は Pump の Error へ変換する
/// Close、Size、Destroy 等の必須処理は isHandled にかかわらず実行する
[[nodiscard]] Result<WindowsMessageHandlerToken> register_windows_message_handler(
    Window& a_window, windowsMessageHandler a_handler);

/// @brief 自分の Token の Handler を解除する。解除済みの同じ Token は成功する
///
/// Window の生成 Thread から呼び、Window 破棄後は Token を使用しない
[[nodiscard]] Result<void> unregister_windows_message_handler(Window& a_window,
                                                             WindowsMessageHandlerToken a_token);

/// @brief Windows用の時間・Thread実装を一括所有する
struct WindowsThreadServices final
{
    std::unique_ptr<Clock> clock;
    std::unique_ptr<Waiter> waiter;
    std::unique_ptr<ThreadFactory> threadFactory;
};

/// @brief Windowsの時計、待機点、Worker Factoryを生成する
///
/// Hostが一意所有し、Workerを停止・joinしてから破棄する。生成失敗では部分生成物を公開しない
[[nodiscard]] Result<WindowsThreadServices> create_windows_thread_services();

/// @brief Windows用WindowSystemを生成して呼出側へ一意所有権を渡す
///
/// 返却したSystemとWindowは呼出Threadで操作・破棄し、Windowを先に破棄する
/// 生成失敗では部分的なSystemを公開しない
[[nodiscard]] Result<std::unique_ptr<WindowSystem>> create_windows_window_system();

/// @brief Windows WindowのNative HandleをWindow生存中だけ借用する
///
/// Window生成Threadから呼び、返したPointerはWindow破棄後に使用しない
/// Win32型はWindows実装内で解釈し、共通Platform契約へ公開しない
[[nodiscard]] Result<void*> borrow_windows_window_handle(Window& a_window);
} // namespace cue
