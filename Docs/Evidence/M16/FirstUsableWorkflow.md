# M16 First Usable Engine Workflow Coverage

## User-facing Procedure

新規利用者向けの正本手順は`Docs/Testing/M16-first-usable-engine-workflow.md`とする。手順内のWindow名、Button名、
Shortcut、生成PathはM16実装の文字列と一致させている。

## Blank Game Template

Project Hubの`Blank 3D`は、Project Descriptor schema version 2、Default Scene Reference、
`Assets/Source/Scenes/Default.cuescene`、Project CMake Preset、`Source/Game/GameModule.cpp`を一つのStagingから
Atomic公開する。Default Sceneは空のObject列を持つCanonical Sceneであり、最小Game ModuleはSchema、Component、
Runtime Systemを追加登録しない。外部Assetまたは第三者Codeを必要としない。

## Automated Mapping

| Manual workflow | Automated coverage |
| --- | --- |
| Blank Project／Default Scene | `Cue.Project.Generator`、`Cue.Project.Generator.Windows`、`Cue.ProjectHub.Service` |
| Project HubからEditor Open／再Open | `Cue.ProjectHub.Windows.EditorProcess`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Hierarchy／Inspector／Undo／Redo | `Cue.EditorCore.DocumentState`、`Cue.Editor.ImGui.HierarchyInspector` |
| Save／Reload／Recovery | `Cue.EditorCore.DocumentState`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Files／Trash／Root境界 | `Cue.EditorCore.FilesWorkspace`、`Cue.ProjectFiles.Create`、`Cue.ProjectFiles.Recovery`、`Cue.ProjectFiles.TrashRecord`、`Cue.ProjectFiles.FileDialogRevalidation` |
| Play／Stop／失敗後再試行 | `Cue.EditorCore.PlaySession`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| 3構成Game Build／失敗／Retry | `Cue.Build.Plan`、`Cue.Build.CMakeRunner.Process`、`Cue.Build.Windows.ArtifactPublisher`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Package Staging／失敗／Rollback | `Cue.Package.Publisher`、`Cue.Package.Workflow` |
| Standalone起動／Relocation | `Cue.RuntimeHost.Package.Process`、`Cue.Editor.Workflow.ProcessRoundTrip` |

## Manual Status

2026-09-11にユーザーが`Docs/Testing/M16-first-usable-engine-workflow.md`の実Window End-to-End手順を実行し、
「問題なし」と確認した。Blank 3D作成からScene編集・保存・再Open、Files操作、Play／Stop、3構成の
Build & Package、Standalone Run／Stop、失敗後Retry、実行中終了確認までを一続きの制作LoopとしてPassした。

Screenshotまたは動画は保存していない。M12、M13、M14の既存手動確認は、Project Hub、Editor Scene、Files、
Play／Stopまでの個別操作根拠として維持する。
