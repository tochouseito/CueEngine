#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

#include <Runtime/WorldHandle.h>
#include <SceneRender/RenderScene.h>
#include <ScriptRuntime/ScriptSystem.h>

namespace cue
{
class Scene;
class World;

/// @brief Play / 製品に共通する実行 World と Script を一意所有する。Window / Renderer / IPC は持たない
/// 生成・更新・抽出・停止・破棄は生成 Thread で直列化する。実行失敗後は停止以外の実行を拒否する
class GameSession final
{
    struct CreateToken final
    {
    };

  public:
    /// @brief Factory が検証した実行 World を所有する
    GameSession(CreateToken, std::unique_ptr<World> a_world, SceneInstanceHandle a_instance,
                ScriptSystem *a_scripts) noexcept;
    /// @brief Owner Thread で未停止の Script と World を回収する
    ~GameSession();
    GameSession(const GameSession &) = delete;
    GameSession &operator=(const GameSession &) = delete;
    /// @brief Scene を実体化し、Module を所有する ScriptSystem の初期化まで完成させる
    /// 途中失敗では部分 Session を公開しない。Module と World は呼出 Thread で回収する
    [[nodiscard]] static Result<std::unique_ptr<GameSession>> create(const Scene &a_scene,
                                                                     std::unique_ptr<RuntimeGameModule> a_module,
                                                                     scriptDiagnosticCallback a_report = {});
    /// @brief 外部の時間入力で Script と Transform を更新する。FPS 制御は Host が担当する
    [[nodiscard]] Result<void> update(std::chrono::nanoseconds a_deltaTime);
    /// @brief 描画側へ渡せる独立した値を抽出する。World / Component の借用を含めない
    [[nodiscard]] Result<SceneRenderSnapshot> snapshot();
    /// @brief 実行 Scene 内の Object / Script を指定し、次の更新前境界で設定を反映する
    /// 未存在 Object は拒否し、Setter の失敗では以後の実行を止める
    [[nodiscard]] Result<void> set_property(std::string_view a_objectId, std::string_view a_instanceId,
                                            const ScriptPropertyValue &a_value);
    /// @brief Script を停止して World を回収する。失敗があっても完了し、繰返し呼べる
    [[nodiscard]] Result<void> shutdown();
    /// @brief 正常に完了した Simulation 更新回数を返す。FPS や描画 Frame 数とは異なる
    [[nodiscard]] std::uint64_t completed_ticks() const noexcept;
    /// @brief Owner Thread の実行中だけ実行 World を借用する。構造変更は World の契約に従う
    [[nodiscard]] World *world() noexcept;
    /// @brief この Session の SceneInstance を返す。World 停止後は生存しない
    [[nodiscard]] SceneInstanceHandle instance() const noexcept;

  private:
    /// @brief Thread・再入・停止・終端失敗を検証する
    [[nodiscard]] Result<void> check_running() const;
    std::thread::id m_owner;
    std::unique_ptr<World> m_world;
    SceneInstanceHandle m_instance;
    ScriptSystem *m_scripts = nullptr;
    std::optional<Error> m_failure;
    std::uint64_t m_completedTicks = 0;
    bool m_isBusy = false;
    bool m_isStopped = false;
};
} // namespace cue
