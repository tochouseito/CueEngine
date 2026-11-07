# Project 未選択での Editor 起動

- 日付: 2026-11-05
- 対象: Debug / Development / Release、Windows x64
- 作業 Branch: `m11/issue146/project-loading`、既存の未コミット変更を保全

## 変更

- `CUE_GAME_PROJECT` の既定を空にし、Preset も空を指定する。以前の RotateCube 選択 Cache を解除する。
- 生成 Visual Studio Project の3構成の起動引数を空にする。
- この PC の `.vs/launch.vs.json` と `.vs/ProjectSettings.json` に残っていた Sample 起動引数を除去する。
- 未選択時は UI 状態だけを作り、Project / Scene / SceneDocument / Preview World を生成しない。
- UI は nullable Document を受け付け、File の Open Project / Exit と診断表示だけを操作可能にする。
- 描画には空の所有 Snapshot を公開する。Frame ID / Window / Renderer は継続し、後から Project を開ける。
- `CuePlayHost` は選択 Project と独立した Editor の Build 依存とし、後からの Metadata / Play を利用可能にする。

指定した Project を開く `--project` と、明示的な `-GameProject` によるゲーム Build は維持する。
通常起動で Project 作成 Tool を呼ぶことはない。
Local VS 設定の変更前コピーと SHA-256 は `out/validation/EditorLaunchSettingsBefore-*/` に保持する。
この設定は個人用で Git 管理には追加しない。

## 検証

- 規定 Debug Build: 成功、`out/validation/EditorUnselectedBuild.log`
- Development / Release の Editor Build: 成功、`EditorUnselectedDevelopmentBuild.log` / `EditorUnselectedReleaseBuild.log`
- ProjectSwitch / Ui / NoProject の対象3 Test: 成功、`EditorUnselectedTargetTests.log`
- Development / Release の引数なし継続起動・正常終了: 成功、`EditorUnselectedDevelopmentRun.log` / `EditorUnselectedReleaseRun.log`
- CMakeCache: `CUE_GAME_PROJECT:FILEPATH=`、生成 `.vcxproj` は3構成とも起動引数が空
- Local VS 設定: Sample の `--project` がないことを確認
- 全 Debug CTest: 120 / 120 成功、124.65 秒（`EditorUnselectedAllTests.log`）
- `EditorUnselectedConfiguration.json`: 既定値 / Local 引数 / Cache が空、生成3構成の空引数を記録

ProjectSwitch Test は Single / Worker の両方で、起動直後と Frame 進行後に Project / Scene /
Preview World / Scene Instance がすべて存在しないことを確認する。その後の A → B → A の切替も確認する。
UI Test は EditorApplication を作る前に nullptr Document で描画し、編集や保存を要求しないことを確認する。

Project 未選択の標準 Build では、Sample を明示的に Build した場合だけ登録する6件を外す。
従来の126件から120件となり、独立した Game CMake / Project 作成 / Editor の回帰 Test は維持する。
Sample 統合の検証が必要なら `scripts/codex_build.ps1 -GameProject Samples/RotateCube/RotateCube.cueproj` を明示する。

## 未実施・注意

実 Visual Studio の F5 / Breakpoint 操作、配布パッケージの再生成は未実施。
Visual Studio が編集前の起動設定を保持する場合は、Project を再読み込みしてから起動する。
既存の Sample Project やその成果物は削除していない。Commit / Push / Merge は実施していない。
