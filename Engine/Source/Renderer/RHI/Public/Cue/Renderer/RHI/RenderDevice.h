#pragma once

namespace cue
{
/// @brief Renderer Backend が所有する GPU Device の共通契約
/// @details Device は Backend と同じ Thread で生成し、利用者より長く生存させる
class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;

    /// @brief Hardware Device を利用しているかを診断する
    [[nodiscard]] virtual bool is_software_adapter() const noexcept = 0;
};
} // namespace cue
