#pragma once

namespace cue
{
/// @brief Renderer Backend が所有する GPU Device の共通契約
///
/// 呼出側が所有し、利用者より長く生存させる。呼出側で生成と破棄を同期する
/// Backend 固有の Device や Adapter は派生クラスで取得する
class IRenderDevice
{
public:
    /// @brief 派生 Device を基底 Pointer から安全に破棄する
    virtual ~IRenderDevice() = default;

    IRenderDevice(const IRenderDevice&) = delete;
    IRenderDevice& operator=(const IRenderDevice&) = delete;

    /// @brief 選択した Adapter が Software 実装か返す
    [[nodiscard]] virtual bool is_software_adapter() const noexcept = 0;

protected:
    /// @brief Backend 固有の生成経路からだけ基底契約を構築する
    IRenderDevice() = default;
};
} // namespace cue
