# ImGuiPass と Test Window（M05-06）

Issue: [#82](https://github.com/tochouseito/CueEngine/issues/82)

## 今回の表示仕様

2026-10-06 のユーザー指示に従い、既定 Editor はタイトル `Test`、本文 `ImGui::Text("TEST")` の Window を一つ表示する。初回位置は 32 / 32、初回寸法は 240 / 120。以後は ImGui の移動と Layout 保存を利用する。`EditorHostConfig::buildUi` が指定された場合はその Callback を使う

元 Issue の Demo Window / FinalColor Image 表示は今回の UI 指定に含まれない。FinalColor の物理枠を UI 専用 Heap の SRV として登録する機能は未実装であり、今回の完了範囲に含めない

## Pass と所有

`EditorHost/Public/EditorHost/ImGuiPass.h` に Editor 所有の `IImGuiRenderer` と ImGuiPass を定義する。Header は FrameGraph の抽象契約だけに依存し、ImGui / DX12 / Editor 型を Include しない。PSO、Root、Vertex / Index Buffer、Font Texture は #81 の公式 DX12 Backend が所有する

Pass は注入された Adapter を一意所有する。EditorHost 内部の EditorImGuiRenderer が ImGuiManager の安定した所有先を借用し、記録時に `record_draw_data(FrameGraphContext&)` へ委譲する。Host は Move 不可であり、Graph と Adapter の破棄後に Manager を停止・破棄する

EditorHost は `displayPass` 未指定時に ImGuiPass を作る。明示された Pass は保持する。WindowsHost と Renderer は UI の具体型を生成しない。Standalone の未指定時は従来どおり PresentToSwapChainPass を使う

## Resource と Frame

Graph は FinalColorTexture の Clear、追加 Scene Pass、ImGuiPass の順に構築する。FinalColorTexture の必須生成は維持する。今回の文字 UI は FinalColor を参照しないため、ImGuiPass は BackBuffer の Write / RenderTarget のみを宣言する

setup で BackBuffer Handle と Texture 宣言の Clear 色を取得する。実行時は BackBuffer Clear → RTV 設定 → Adapter 記録を行う。終了時の Present 遷移は Graph の終了 Barrier、提出後の Present は WindowsHost が担当する

UI は採用 Frame の MainThread 段階で構築し、Update より先に CPU Draw Data を確定する。#83 の Snapshot を固定 RenderThread（単一 Thread 構成では Owner）で記録する。WindowsHostCallbacks::main → Runtime → FrameController の汎用 Main Callback を利用し、Renderer 側に ImGuiPass の具体型を公開しない。Font / UI Texture の生成と更新は #83 の Main 公開時に検証・予約と公式 Upload を行う。今回の UI 構築段階は Graph の物理 Texture や Native Descriptor を借用しない

最小化中は WindowsHost が Frame 投入と Present を停止する。Client Size の変更では投入済み Frame と GPU の完了後に Graph とサイズ依存資源を再生成し、描画を再開する。ImGuiPass は displayPassFactory から新しく生成し、同じ ImGuiManager を借用する。詳細は [Presentation Resize](PresentationResize.md) を参照する。Worker への Draw Data 転送は #83 で接続済み

## 検証

- `Cue.Editor.ImGuiPass`: BackBuffer 宣言、Clear / RTV 設定後の記録、Adapter の所有と破棄、記録失敗伝播、Adapter 欠落の Build 拒否
- `Cue.Editor.ImGuiDX12Backend`: 本番 ImGuiPass から公式描画の Red Pixel を Readback、通常 Renderer に戻した Blue Pixel、容量不足回復、動的 Texture、GPU 資源解放
- `Cue.EditorHost.Lifecycle`: 既定 Test UI の頂点 / Index 生成、Frame の GPU 提出、停止、既存の明示表示 Pass と失敗回収
- `Cue.EditorHost.Process`: 実 Executable の起動、Window 表示、Close 後の終了 Code 0

2026-10-06: `scripts/codex_build.ps1` の最終 Debug Build 成功。全 CTest は 36 / 36 成功。既定 UI の 3 Frame 提出・停止と、本番 ImGuiPass 経由の実画素、Adapter の失敗伝播・解放を確認した。Main → Update → Render の順序、Main の構築 Thread 実行、満杯時の UI 準備省略、Main 失敗 / 例外の停止伝播と Host / Runtime の再入拒否も確認した。独立した読み取り Review でも追加の具体的な不具合は見つからなかった

Development / Release、実画面の目視と手動操作、FinalColor Image、Worker、Multi-Viewport は今回の検証に含めない。目視確認は computer-use のアプリ承認が時間切れになったため未実施

#83 の描画 Snapshot 転送と #84 の構成別検証を追加した。現在の結果と未完了項目は [ImGui Completion Gate](ImGuiCompletionGate.md) を参照する。上記の検証記録は各 Issue 実装時点の履歴とする
