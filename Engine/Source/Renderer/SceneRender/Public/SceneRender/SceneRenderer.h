#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <FrameGraph/FrameGraph.h>
#include <RenderData/RenderScene.h>

namespace cue
{
class IBackend;

/// @brief Texture Loader が所有する、行 Padding を持たない一枚の RGBA8 Pixel データ
struct SceneTextureRgba8 final
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool isSrgb = true;
    std::vector<std::byte> pixels;
};

/// @brief Asset ID から GPU Material へ解決する描画入力。共有 Texture は同じ所有参照を渡す
struct SceneMaterialDesc final
{
    std::string assetId;
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    std::shared_ptr<const SceneTextureRgba8> texture;
};

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
    /// @brief Owner Thread で描画定義の世代を置き換える。空なら既定 Material だけに戻す
    /// 転送完了まで待ち、失敗時と公開済み Frame は旧世代を保持する。shutdown との並行呼出しは禁止
    [[nodiscard]] virtual Result<void> set_materials(std::span<const SceneMaterialDesc> a_materials) = 0;
    /// @brief Main から採用 Frame の描画値を一度だけ渡す。最大 64 未消費 Frame を保持する
    [[nodiscard]] virtual Result<void> publish_frame(std::uint64_t a_frame, SceneRenderSnapshot a_snapshot) = 0;
    /// @brief 同じ Frame の Snapshot を選択し、記録 Callback 復帰後は取消・失敗でも回収する
    /// Callback は同じ Thread で Graph を実行する。再入・並行 render_frame は禁止する
    [[nodiscard]] virtual Result<void> render_frame(std::uint64_t a_frame,
                                                    const std::function<Result<void>()> &a_record) = 0;
    /// @brief FinalColor へ描画する Pass 群を未構築 Graph に登録する。失敗した Graph は破棄する
    [[nodiscard]] virtual Result<void> add_passes(FrameGraph &a_graph) = 0;
    /// @brief Frame 排出後の再構成で未消費 Snapshot を破棄する
    [[nodiscard]] virtual Result<void> discard_pending() = 0;
    /// @brief Graph 破棄後に Snapshot を回収して MeshHandle を退役する。繰返し呼べる
    [[nodiscard]] virtual Result<void> shutdown() = 0;

  protected:
    /// @brief Backend の Factory だけが具体 Renderer を生成する
    ISceneRenderer() = default;
};

/// @brief Backend に対応する Scene 描画実装を生成し、組み込み Cube と既定 Material の転送完了まで待つ
/// 呼出中 Backend を直列利用し、返された Renderer と全 MeshPass を Backend より先に停止する
[[nodiscard]] Result<std::unique_ptr<ISceneRenderer>> create_scene_renderer(IBackend &a_backend);
} // namespace cue
