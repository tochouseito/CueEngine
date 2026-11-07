# M11 手順 4: Camera / Cube 描画 / Scene View の実装と検証

- 日付: 2026-11-02
- Branch: `m11/issue146/project-loading`
- 指示: 手順 4 全体、Cube を Editor に表示するところまで実装する
- 契約: `Docs/Decisions/0014-camera-and-initial-scene-rendering.md`

## 実装

- Math: `Projection.h` の RH / -Z / 行ベクトル / Depth 0～1 の透視投影と View 計算
- Runtime: `Components/CameraComponent.h` と `MeshRendererComponent.h`
- Scene / SceneRuntime: Camera / Mesh の検証・Serialize・読込・World 実体化
- SceneRender: World から所有値を抽出し、Frame ID で Main と Render 間を受け渡す
- DX12: D32Float、DSV、Depth PSO、分離頂点入力、24 Byte の Command Signature
- MeshPass: 統合 Page の VB / IB と CPU 生成 Indirect 引数、描画枠別の行列転送
- Editor: 現在の GPU 枠の FinalColor を UI Heap に接続する Scene View
- Sample: Cube の組み込み Mesh ID、三面を確認できる初期回転、Camera を追加

Bounds / GPU カリングは追加していない。Scene / Runtime Component に GPU Handle を保存しない。
Shader は `Engine/Shader/Hlsl/StaticMesh.hlsl` に置いた。

## 自動検証

既定の `pwsh -NoProfile -File scripts/codex_build.ps1` による Debug Build が成功した。
最終 Log に Compiler Error / Warning はない。
全 Debug CTest は **87 / 87 成功、70.84 秒**。
その後、既存の性能設定統合 Test に Cube / Camera Scene を追加し、対象の再 Build と再 Test が成功した（6.63 秒）。
製品 Code の追加変更はなく、変更した Test だけを再検証した。
`git diff --check` も成功した。

主な追加・拡張 Test:

| Test | 確認内容 |
| --- | --- |
| Cue.Math.Projection | float / double、Near→0・Far→1、FOV / Aspect、View の位置・回転、不正値 |
| Cue.Runtime.Camera | 設定検証、旧値保全、World での Component 利用 |
| Cue.Scene.RenderComponents | Camera / Mesh の保存・再読込・検証・World 実体化 |
| Cue.Renderer.SceneExtraction | Snapshot 独立性、World 破棄後の値、Camera 欠落・重複、Dirty、Owner Thread |
| Cue.Renderer.DX12.Depth | WARP で Committed / Placed Depth、DSV、GPU 枠別の資源と不正入力 |
| Cue.Renderer.DX12.SceneRenderer | WARP で実画素と Depth を読み戻し、Indirect / Cull / Resize を確認 |
| Cue.Editor.ImGuiPass | FinalColor の ShaderRead 宣言 |
| Cue.Editor.ImGuiDX12Backend | 公開前の取消、既存の UI Renderer / Descriptor 寿命 |
| Cue.EditorHost.PerformanceSettings | Cube 表示中の Buffering / Worker / Waitable 設定の切替 |
| Cue.EditorHost.ResizeMinimizeRestore | Sample を起動した Editor の Resize / Minimize / Restore / Close |

WARP の描画 Test は次を確認した。

1. Cube 中心の青い面色と、最も近い表面の Depth（Camera 距離 2.5）
2. Cube を画面外へ移した Frame と、Mesh がない Frame の背景
3. 手前の赤面 → 奥の青面の順に提出しても、Depth Test により手前が残ること
4. 二番目の Indirect Record が別の Transform を参照すること
5. 96×48 への Resize 後も Cube が描画され、Aspect 比を反映すること
6. Debug Layer の Error / Corruption がないこと、Backend 停止前の Native 資源解放

## 実際の Editor 表示

Debug の `CueEditorHost.exe` を RotateCube の `.cueproj` 指定で起動した。
computer-use で対象 Window を選び、前面にして実画面を確認した。
Scene View に赤・緑・青の三面を持つ Cube が表示された。
確認後は通常の Close 操作で終了し、Window が消えたことを確認した。
この確認は実際の Editor 表示であり、手順 5 の編集操作や製品 Package の実行確認ではない。

## 記録

- `out/validation/SceneRenderingBuildVerified.log`: 最終の既定 Debug Build
- `out/validation/SceneRenderingFullCTest.log`: 全 87 Test
- `out/validation/SceneRenderingCoreCTest.log`: Camera / Depth の個別確認
- `out/validation/SceneRenderingSnapshotCTest.log`: Scene / Snapshot の個別確認
- `out/validation/SceneRenderingGpuCTest.log`: WARP 実画素 / Depth
- `out/validation/SceneRenderingReconfigurationBuild.log` / `SceneRenderingReconfigurationCTest.log`: Cube 表示中の設定切替

初回 Build では Depth Clear の設定が Buffer 生成へ誤挿入されていたためコンパイルで検出し、
Texture 生成側だけへ残すように修正した。最終 Build は成功している。

## 未実施・制約

- Development / Release / Windows 以外の Build、GPU 性能測定は未実施
- OOM / Device Lost の実機故障注入は未実施
- 現在は Perspective Camera 一つと組み込み Cube、固定面色のみ対応する
- Scene View Panel の寸法は表示拡縮に使い、描画解像度は Window の寸法を使う
- Preview World は Main 所有。Play / Script 更新の Thread 分担は今後決める
- Script による自動回転、Cube 追加・Transform 編集・保存 UI、Editor からのアプリ生成は後続
- 既存の未 Commit 変更を保持した。Commit / Push / Merge / Branch 作成 / GitHub 更新は行っていない
