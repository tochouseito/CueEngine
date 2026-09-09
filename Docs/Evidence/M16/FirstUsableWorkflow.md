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
| Hierarchy／Inspector／Undo／Redo | `Cue.Editor.ImGui`、`Cue.EditorCore.Commands`、`Cue.EditorCore.Controller` |
| Save／Reload／Recovery | `Cue.EditorCore.Persistence`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Files／Trash／Root境界 | `Cue.EditorCore.FilesWorkspace`、`Cue.ProjectFiles.*`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Play／Stop／失敗後再試行 | `Cue.EditorCore.PlaySession`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| 3構成Game Build／失敗／Retry | `Cue.Build.*`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Package Staging／失敗／Rollback | `Cue.Package.Publisher`、`Cue.Package.Workflow` |
| Standalone起動／Relocation | `Cue.RuntimeHost.Package.Process`、`Cue.Editor.Workflow.ProcessRoundTrip` |

## Manual Status

M16の実WindowEnd-to-End確認結果はCompletion Gate #237でこの文書へ追記する。M12、M13、M14の既存手動確認は、
Project Hub、Editor Scene、Files、Play／Stopまでの個別操作根拠として維持する。

