#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <FrameGraph/FrameGraph.h>
#include <SceneRender/RenderScene.h>

namespace cue
{
class IBackend;

/// @brief Frame ID に対応する所有 Snapshot と、Graph へ注入する MeshPass の生成を管理する
/// Backend より先に停止する。Main の publish と Render の render_frame は内部で同期する
/// create_pass / discard_pending / shutdown は CPU / GPU を静止した Owner Thread で呼ぶ
class ISceneRenderer
{
  public:
    /// @brief Graph の停止後に GPU Mesh の新規利用を止める
    virtual ~ISceneRenderer() = default;
    /// @brief Snapshot と Mesh 所有の複製を禁止する
    ISceneRenderer(const ISceneRenderer &) = delete;
    /// @brief Snapshot と Mesh 所有の複製を禁止する
    ISceneRenderer &operator=(const ISceneRenderer &) = delete;
    /// @brief Main から採用 Frame の描画値を一度だけ渡す。最大 64 未消費 Frame を保持する
    [[nodiscard]] virtual Result<void> publish_frame(std::uint64_t a_frame, SceneRenderSnapshot a_snapshot) = 0;
    /// @brief 同じ Frame の Snapshot を選択し、記録 Callback 復帰後は取消・失敗でも回収する
    /// Callback は同じ Thread で Graph を実行する。再入・並行 render_frame は禁止する
    [[nodiscard]] virtual Result<void> render_frame(std::uint64_t a_frame,
                                                    const std::function<Result<void>()> &a_record) = 0;
    /// @brief FinalColor と Depth を使用する新しい MeshPass を Graph ごとに生成する
    [[nodiscard]] virtual Result<std::unique_ptr<FrameGraphPass>> create_pass() = 0;
    /// @brief Frame 排出後の再構成で未消費 Snapshot を破棄する
    [[nodiscard]] virtual Result<void> discard_pending() = 0;
    /// @brief Graph 破棄後に Snapshot を回収して MeshHandle を退役する。繰返し呼べる
    [[nodiscard]] virtual Result<void> shutdown() = 0;

  protected:
    /// @brief Backend の Factory だけが具体 Renderer を生成する
    ISceneRenderer() = default;
};

/// @brief Backend に対応する Scene 描画実装を生成し、組み込み Cube の初回転送完了まで待つ
/// 呼出中 Backend を直列利用し、返された Renderer と全 MeshPass を Backend より先に停止する
[[nodiscard]] Result<std::unique_ptr<ISceneRenderer>> create_scene_renderer(IBackend &a_backend);
} // namespace cue
