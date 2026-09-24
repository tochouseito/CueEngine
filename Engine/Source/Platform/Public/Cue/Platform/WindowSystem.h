#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Platform/Window.h>

#include <memory>

namespace cue
{
enum class PumpStatus
{
    Running,
    QuitRequested,
};

/// @brief Windowの生成と呼出ThreadのMessage Pumpを管理する
///
/// Hostが一意所有し、生成したWindowをすべて破棄してからSystemを破棄する
/// 全操作と破棄は生成Threadに限り、再入しない。生成失敗では再試行可能な状態を保つ
/// 一Threadで同時に扱うSystemは一つとし、公開済みMain Window終了後は再生成しない
class WindowSystem
{
public:
    /// @brief Systemの資源を解放する
    virtual ~WindowSystem() = default;

    /// @brief 単一Main Windowを生成し、成功時に所有権を呼出側へ渡す
    [[nodiscard]] virtual Result<std::unique_ptr<Window>> create_window(const WindowDescriptor& a_descriptor) = 0;

    /// @brief 待機せず呼出ThreadのMessageを処理して終了要求を返す
    [[nodiscard]] virtual Result<PumpStatus> pump_events() = 0;
};
} // namespace cue
