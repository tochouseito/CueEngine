#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/WindowEvent.h>

#include <string>

namespace cue
{
/// @brief 作成時のTitleとClient Areaの要求Sizeを所有する
struct WindowDescriptor final
{
    std::string title;
    WindowSize clientSize;
};

/// @brief Windowの生成から破棄までをHostへ公開する状態
enum class WindowState
{
    Created,
    Visible,
    CloseRequested,
    Destroyed,
};

/// @brief 一つのWindowとそのEvent Queueを一意に所有する公開契約
///
/// 呼出側がWindowを所有し、WindowSystemより先に破棄する
/// 全操作と破棄は生成Threadで行い、再入しない。失敗時は各操作前の状態を維持する
class Window
{
public:
    /// @brief Native Windowを所有契約に従って解放する
    virtual ~Window() = default;

    /// @brief Windowを表示し、失敗時は表示前の状態を維持する
    [[nodiscard]] virtual Result<void> show() = 0;

    /// @brief Windowを明示的に破棄する。二度目以降は成功とする
    [[nodiscard]] virtual Result<void> destroy() = 0;

    /// @brief 現在のLifecycle状態を返す
    [[nodiscard]] virtual WindowState state() const noexcept = 0;

    /// @brief Client AreaのSizeを返す
    [[nodiscard]] virtual WindowSize client_size() const noexcept = 0;

    /// @brief FIFOの次のEventを取り出し、空なら出力値を変更しない
    [[nodiscard]] virtual bool try_pop_event(WindowEvent& a_event) noexcept = 0;
};
} // namespace cue
