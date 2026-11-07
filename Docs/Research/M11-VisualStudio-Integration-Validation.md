# M11 Visual Studio 連携 / Script Build / Debug Play の検証

- 日付: 2026-11-04
- 対象: `m11/issue146/project-loading` の既存作業を保全した追加実装
- 契約: [ADR-0020](../Decisions/0020-editor-ide-and-debug-play.md)
- この記録時点では Commit / Push / Merge は実施していない

## 実装範囲

`Script > Open Visual Studio`、Inspector の `Edit Source`、`Script > Build Scripts`、
`Debug Play`、`Cancel Script Operation` を追加した。
Editor 専用の抽象 IDE Service / Windows Provider / C# DTE Bridge と Controller に責務を分けた。
UI が返す Action を Host の Owner Thread で消費し、非同期 Service を poll する。

ProjectBuild は Player / IDE Configure / ScriptModule Build を区別する。
IDE と Script Build は `Intermediate/IDE/<Engine識別値>/Windows/x64` を共有し、
DLL と PDB は実行中 Editor の構成に合わせる。製品用 Tree と Run の契約は独立して維持する。
Source Map は `Source/ScriptSources.cueide`、RotateCube の Type ID は `sample.RotateCube`。
Debug Play はゲーム DLL 読込前に子を止め、検証した所有 PID へ Native Debugger を接続してから開始する。

## 自動検証の記録

標準 `pwsh -NoProfile -File scripts/codex_build.ps1` による Debug Build は成功した。
記録: `out/validation/VisualStudioIntegrationBuild.log`。
全変更後の標準 Debug 再 Build も成功し、`VisualStudioIntegrationFinalBuild.log` に記録した。
Framework 4.8 の Bridge もこの Build で生成し、NuGet Restore は行っていない。

全 Debug CTest の最初の結果は **117 / 118 成功、119.85 秒**。
記録: `out/validation/VisualStudioIntegrationTests.log`。
`Cue.Script.Play` が `Script Play test failed at line 231` で失敗した。
原因は新規 Test Fixture が意図した Types 応答の後に別の不正 Header まで送っていたことだった。
当該ケースの Fixture を修正し、`Cue.Script.Play` は単独で成功した（4.64 秒）。
製品 Code の回避変更は加えていない。記録: `VisualStudioScriptPlayFixtureTests.log`。

修正後に全 Debug CTest を再実行し、**118 / 118 成功、116.60 秒**。
最終記録: `out/validation/VisualStudioIntegrationFinalTests.log`。

| 成功した Test | 確認内容 |
| --- | --- |
| Cue.EditorIde.ScriptSource | Source Map の Version / Type ID / 相対 Path、無効入力と Source 外参照の拒否 |
| Cue.EditorIde.WindowsService | 実 Process の Fixture による非同期応答、失敗、取消、期限と対象検証 |
| Cue.EditorIde.BridgeInvalidArguments | 実 C# Bridge が不正引数を成功扱いしないこと |
| Cue.Project.GameBuildService | IDE Configure のみ、共通 Tree、DLL / PDB / Manifest、構成・Hash 不一致、曖昧 Solution、排他、目的別 Run 制限 |
| Cue.Editor.ScriptController | IDE / Source 表示の順序、Build 成否と Reload / 起動、取消・再試行・二重要求、Owner Thread、所有外 Build / 通常 Play の保全 |
| Cue.Editor.ScriptUi | Inspector の Edit Source Action と無効時の抑止、従来の Script Property 編集の保全 |
| Cue.Script.DebugPlay | DLL 読込前 Gate、Debugger 接続確認、Continue、停止、通常 Play との境界 |

Fixture Test は実 Visual Studio の DTE 操作や Symbol Load を代替しない。

## 実 CMake の Script Build

Editor と同じ `IGameBuildService` を任意の `--engine-smoke` モードから呼び出した。
TEMP の Sample コピーに対し、Windows / x64 / Debug / ScriptModule の
Configure → `Cue.GameProjectBuild` → DLL / Static / Manifest / PDB の検証が成功した。

- Tree: `C:/Users/sinse/AppData/Local/Temp/cue-ui-cad09dd3/Intermediate/IDE/8aa15dc2f5782682/Windows/x64`
- 出力 Root: `C:/Users/sinse/AppData/Local/Temp/cue-ui-cad09dd3/Build/Windows/x64/Debug`
- Log: `out/validation/VisualStudioScriptBuild.log`
- 終了状態: `Succeeded`

TEMP 配下の MSBuild 中間 Directory に対する MSB8029 Warning は残ったが、Build は成功した。
通常の Project では自身の Intermediate 配下を使う。

## 実 Visual Studio 操作

専用 Solution を実 Visual Studio で開き、Source 表示と Native Breakpoint 到達を確認した。
`out/validation/VisualStudioBreakpoint.log` に次の結果を記録した。

- 選択 IDE PID: `39068`、対象 Play PID: `32896`
- Windows IDE Service による Native Debugger 接続を確認
- `RotateCubeBehaviour::update` の Source Breakpoint に停止
- Continue 後、3 Tick の Play を正常終了し、検証 Process も Exit Code `0`

この検証は TEMP の専用 Project と同じ Build の DLL / PDB を使い、Source を変更せず行った。
UI 入力を代行せず、製品と同じ IDE Service と PlayClient、および DTE で確認した。
初回 `out/validation/VisualStudioOpenSource.log` は `InvalidCastException` により失敗した。
同じ Binary の再試行と新しい VS Instance での初回起動は成功し、例外は再現していない。
発生位置は未確定のため、推測による catch 追加などは行っていない。
成功記録は `VisualStudioOpenSourceSuccess.log`（PID `39068`）と
`VisualStudioColdOpenSource.log`（別名 Solution / PID `2508`）。
検証用 VS は終了せず残している。

追加の手動確認が必要な項目:

1. Editor の各メニューから、専用 Solution / Source 表示 / Debug Play を連続実行する。
2. 別 Solution の既存 Instance を開いた状態での実操作。
3. 停止中の Editor の画面応答、および VS 終了後の UI からの再試行。

## 残る制約と未実施

- Visual Studio から直接 Build すると Editor の Project Lock を通らない。Play と Editor の Build を終えてから実行する。
- PDB は DLL と同じ Link で出力するが、Service は PDB GUID と DLL CodeView の一致を独立検証しない。
- Framework 4.8 Reference Assemblies / VS の未導入環境、他 PC、他 IDE、他 OS は未検証。
- 初回 COM エラーの原因は未特定。現在は失敗を表示して再要求できるが、自動回復を保証しない。
- 実手動制作経路の完走と、Release / Development の実 VS Breakpoint は未検証。
- 今回は性能計測を目的とせず、Frame 性能の改善を主張しない。
