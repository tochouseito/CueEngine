# M13 Editor Files Manual Workflow Record

## Status

Pending User verification.

CodexのWindows app controlが利用できないため、2026-09-07時点では実Window確認を実行していない。
Headless ImGui Testと実Process Testは成功しているが、本記録では手動Workflow成功の代用として扱わない。

## Procedure

- `Docs/Testing/M13-files-workflow-manual-test.md`
- `Docs/Testing/M13-native-file-dialog-manual-test.md`

## Result Checklist

| Check | Result | Note |
|---|---|---|
| Project作成とEditor Open | Pending | |
| Folder／File作成、Rename、Move、Copy、検索 | Pending | |
| F5／DeleteのFiles focus限定 | Pending | |
| Delete PreviewとEscape Cancel | Pending | |
| Project-local TrashへのDeleteとRestore | Pending | |
| Restore競合時の既存Data／Recovery Data保持 | Pending | |
| Root外Move拒否と日本語Error | Pending | |
| Editor外変更の再列挙とSelection解除 | Pending | |
| Editor再起動後のRecovery CatalogとRestore | Pending | |
| Native Dialogのopen／save／folderとCancel | Pending | |
| Test ProjectのCleanup | Pending | |

## Automated Supporting Evidence

- `Cue.Editor.ImGui.Files`: Pass
- `Cue.EditorCore.FilesWorkspace`: Pass
- `Cue.Editor.Workflow.ProcessRoundTrip`: Pass
- `Cue.ProjectFiles.Create`: Pass
- `Cue.ProjectFiles.Recovery`: Pass
- `Cue.ProjectFiles.FileDialogRevalidation`: Pass
- `Cue.IO.WindowsWorkspaceWatcher`: 3構成で各20回連続Pass

## Record Update

User確認後、実行日、環境、各Result、発見事項、Test ProjectのCleanup状況を記録する。
一項目でも失敗した場合はGateをPassにせず、最小のBug Issueへ分離する。
