#pragma once

namespace cue::dx12
{
/// @brief Debug 構成で Backend 解放後の DXGI Live Object を報告する
class DX12ResourceLeakChecker final
{
public:
    /// @brief 診断機能が利用できる場合に Process 内の Live Object を一度報告する
    ///
    /// Backend が所有する Device と Pool の解放後に呼ぶ。Debug Layer が利用できない環境では報告を省略する
    static void report_live_objects();
};
} // namespace cue::dx12
