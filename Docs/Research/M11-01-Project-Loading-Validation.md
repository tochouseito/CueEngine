# M11-01 Project 読み込みの実装・検証

- Date: 2026-10-28
- Issue: [#146](https://github.com/tochouseito/CueEngine/issues/146)
- Branch: `m11/issue146/project-loading`
- Base: `9492422768c3b0350a3033aac07aa920b184450e`（origin/develop）
- 契約: [ADR-0009](../Decisions/0009-project-loading.md)

## 実装

- `Cue.Project` に、`.cueproj` v1 の読み込み・検証・Project Root と Source / Content の解決を追加した。
- `Samples/RotateCube/RotateCube.cueproj` と Source / Content の配置を追加した。
- `IFileSystem::absolute_path` と Windows 実装を追加し、起動引数の Path を一度だけ絶対化した。
- Windows Command Line の分割と UTF-8 変換後に、共通 Parser が `--project <path>` を検証する。
- Editor の起動前に Project を読み込み、EditorHost が不変の所有値として保持する。Window タイトルと既定 UI に Project 名を表示する。
- CMake に RotateCube を開く固定の Visual Studio 起動引数を設定した。Debug / Development / Release の生成 Project に反映されることを確認した。
- Path 内にあった UTF-8 検証を Foundation の `is_valid_utf8_text` へ抽出した。既存 Path の判定を保ち、表示名を Path 構文から分離し、設定の Comment 行も検証する。
- 起動失敗は診断 Dialog と終了 Code 1 で扱い、Editor の Window / Renderer / Worker の初期化前に拒否する。

## Build / Test

Windows x64 / Visual Studio 2026 / Debug で以下を実行した。

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1
ctest --preset windows-vs2026-debug --output-on-failure
git diff --check
```

- 最終 Debug Build: 成功。
- Debug CTest: **64 / 64 成功**、全体 69.78 秒。
- `git diff --check`: 成功。
- 独立した読み取り専用レビューで表示名と File 全体の UTF-8 検証を修正し、再確認時の追加指摘なし。

| 対象 | 確認した内容 |
| --- | --- |
| `Cue.Project.PublicHeaders` | 公開設定が Windows / Editor / Renderer の Header を要求しない |
| `Cue.Project.Loading` | UTF-8 / 空白 Path、移動後の Project ID、値の独立性、相対指定、CWD 不変、必須項目、重複、Version、NUL、不正 UTF-8、Root 外参照、Directory 欠損、64 KiB 境界、I/O 失敗 |
| `Cue.Editor.ProjectArguments` | 引数の所有化、絶対 / 相対指定、Unicode、未指定・重複・未知引数・不正 Path の拒否 |
| `Cue.EditorHost.Process` | Sample 指定で実 Editor が起動し、Project 名を表示、6 秒以上の継続と正常 Close |
| `Cue.EditorHost.ResizeMinimizeRestore` | Sample を開いた Editor の Resize・最小化・復帰・正常 Close |
| `Cue.EditorHost.ProjectRelative` | Samples を CWD とする相対引数からの起動・継続・正常 Close |
| `Cue.EditorHost.ProjectUnicode` | 日本語・空白を含む実 File Path を Windows Command Line 経由で指定し、起動・継続・正常 Close |
| `Cue.EditorHost.ProjectMissingArgument` | 引数なしの診断 Dialog と終了 Code 1 |
| `Cue.EditorHost.ProjectMissingFile` | File 欠損の診断 Dialog と終了 Code 1 |
| `Cue.EditorHost.ProjectUnsupportedVersion` | 未対応 Version の診断 Dialog と終了 Code 1 |
| 既存 Test | Path、FileSystem、Host、FrameController、Logging、Profiler、DX12 / FrameGraph、ImGui を含む全体回帰 |

通常の Tool 実行では、変更していない既存 Source にも PDB 管理の C1902 が発生した。
同じ Build Script を権限のある実行環境で再実行すると解消した。Compiler / PDB 部品の Version 混在は確認されず、
実行環境の PDB 起動・通信制約と整合する結果だった。Source の回避変更や既存 PDB の削除はしていない。
追加 Test の C++ 規格指定と Result の参照修飾の Compile Error は修正し、最終 Build / 全 Test は修正版で確認した。

生 Log は Git 管理外の `out/validation/Issue146/DebugBuildFinal.log` と `DebugCTest.log` に保存した。

## 未実施・残る制約

- Development / Release の Build・実行は未実施。生成された固定起動引数だけを確認した。
- Project Root の物理的な Symlink / Junction 閉じ込めは保証しない。現在は信頼する開発 Project の読み取りだけを扱う。
- Windows 以外の FileSystem 実装はまだない。Project 公開契約には具体的な Platform 型を含めていない。
- Hub、Scene / ECS / Script、Cube 描画、アプリ生成は後続 Issue の範囲。
- UI の目視による Layout 確認は未実施。実 Process の Project 名は Window タイトルを検査し、既定 ImGui UI の描画は既存 Test と併せて確認した。
- Commit / Push / Merge、Issue の Close は行っていない。
