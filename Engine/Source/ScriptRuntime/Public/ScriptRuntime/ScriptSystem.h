#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <ECS/ECSManager.h>
#include <Runtime/WorldHandle.h>
#include <Runtime/WorldSystem.h>
#include <ScriptData/ScriptData.h>
#include <ScriptRuntime/ScriptModule.h>

namespace cue
{
class Scene;
class World;

/// @brief Host の診断配送先。World Owner 上で同期実行し、借用文字列を呼出し後に保持しない
using scriptDiagnosticCallback = std::function<void(std::uint32_t, std::string_view)>;

/// @brief 一 Entity の Script 設定を所有する。実行 Instance は ScriptSystem が別に管理する
/// Owner Thread でだけ変更する。構造変更と同じ Callback 中には設定を変更しない
class ScriptComponent final : public ecs::IComponentTag
{
  public:
    /// @brief 空の Script 設定を作る。Instance は生成しない
    ScriptComponent() = default;
    /// @brief 検証済み候補へ置き換える。失敗時は旧設定を保つ
    [[nodiscard]] Result<void> set_scripts(std::vector<ScriptDescriptor> a_scripts);
    /// @brief 次の設定変更・Component の移動 / 削除まで設定を借用する
    [[nodiscard]] const std::vector<ScriptDescriptor> &scripts() const noexcept;

  private:
    std::vector<ScriptDescriptor> m_scripts;
};

/// @brief World Owner Thread で Script Instance を生成・更新・停止してから Game DLL を解放する
/// World の終端失敗でも shutdown が全 Instance を回収する。再入と World の使い回しを拒否する
class ScriptSystem final : public WorldSystem
{
    struct State;
    struct CreateToken final
    {
    };

  public:
    /// @brief Factory が Module の所有権と空の実行状態を渡す
    ScriptSystem(CreateToken, std::unique_ptr<State> a_state) noexcept;
    /// @brief 正常な停止は World::shutdown が行う。残った ABI Token も Module より先に破棄する
    ~ScriptSystem() override;
    /// @brief Module を一意所有する System を作る。World::add_system へ渡して使う
    [[nodiscard]] static Result<std::unique_ptr<ScriptSystem>> create(std::unique_ptr<RuntimeGameModule> a_module,
                                                                      scriptDiagnosticCallback a_report = {});
    /// @brief 全割当を検証・生成する。非 Active / 無効な Script も型の不一致を検出する
    [[nodiscard]] Result<void> initialize(World &a_world) override;
    /// @brief 追加・削除を照合し、有効な Script を登録順で更新する
    [[nodiscard]] Result<void> update(World &a_world, const WorldUpdateContext &a_context) override;
    /// @brief 開始済み Script を逆順で一度だけ停止し、失敗しても全 Token の回収を試みる
    /// Entity の削除後も呼ばれるため、stop の GameObject は既に無効な場合がある
    [[nodiscard]] Result<void> shutdown(World &a_world) override;
    /// @brief Owner の Update 前境界で設定を適用する。成功値は DLL setter も受理したことを表す
    /// Play Inspector 用。失敗後は Session を停止し、編集用 Scene へは書き戻さない
    [[nodiscard]] Result<void> set_property(World &a_world, EntityHandle a_entity, std::string_view a_instanceId,
                                            const ScriptPropertyValue &a_value);

  private:
    std::unique_ptr<State> m_state;
};

/// @brief 実体化済み Scene の Script 設定を World に追加する。Preview では呼ばず実行 World だけで使う
/// 失敗時は呼出側が Instance / World を回収し、部分状態を実行しない
[[nodiscard]] Result<void> attach_scene_scripts(const Scene &a_scene, World &a_world, SceneInstanceHandle a_instance);
} // namespace cue
