# M13 Editor Files Manual Workflow Record

## Status

Pass.

2026-09-08にUserがLocal Windows x64のDebug Toolを実Windowで操作し、手順全体に問題がないことを報告した。
CodexのWindows app controlは利用できなかったため、本記録はUser報告を正本とし、画面Captureまたは操作録画は保存していない。

## Procedure

- `Docs/Testing/M13-files-workflow-manual-test.md`
- `Docs/Testing/M13-native-file-dialog-manual-test.md`

## Result Checklist

| Check | Result | Note |
|---|---|---|
| Project作成とEditor Open | Pass | 問題なし |
| Folder／File作成、Rename、Move、Copy、検索 | Pass | 問題なし |
| F5／DeleteのFiles focus限定 | Pass | 問題なし |
| Delete PreviewとEscape Cancel | Pass | 問題なし |
| Project-local TrashへのDeleteとRestore | Pass | 問題なし |
| Restore競合時の既存Data／Recovery Data保持 | Pass | 問題なし |
| Root外Move拒否と日本語Error | Pass | 問題なし |
| Editor外変更の再列挙とSelection解除 | Pass | 問題なし |
| Editor再起動後のRecovery CatalogとRestore | Pass | 問題なし |
| Native Dialogのopen／save／folderとCancel | Pass | 問題なし |
| Test ProjectのCleanup | Pass | 問題なし |

## Automated Supporting Evidence

- `Cue.Editor.ImGui.Files`: Pass
- `Cue.EditorCore.FilesWorkspace`: Pass
- `Cue.Editor.Workflow.ProcessRoundTrip`: Pass
- `Cue.ProjectFiles.Create`: Pass
- `Cue.ProjectFiles.Recovery`: Pass
- `Cue.ProjectFiles.FileDialogRevalidation`: Pass
- `Cue.IO.WindowsWorkspaceWatcher`: 3構成で各20回連続Pass

## Result

発見事項はなく、M13の手動Files UI WorkflowとNative Dialog GateをPassとする。
