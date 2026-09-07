# M13 Editor Files Manual Workflow

## Purpose

Project作成からEditor再起動後の復元まで、M13で追加したFiles UI、Project Root境界、外部変更再同期、
Project-local Trashを実Windowで確認する。Asset Import／Cook、Asset Database、Source Control統合は対象外とする。

## Preconditions

1. `Debug|x64`の全TargetをBuild済みであること。
2. `out/build/windows-vs2026/bin/Debug/CueProjectHubTool.exe`と同じDirectoryに
   `CueEditorTool.exe`が存在すること。
3. Test専用の空DirectoryをProject作成先として使用すること。
4. Project DescriptorのSource Asset Rootが`Assets/Source`であること。
5. Test Projectの外側に、境界拒否確認用の空Fileを一つ用意すること。

## Files UI Workflow

1. Project HubからTest Projectを作成し、Editorで開く。
2. Files Windowで`Workflow` Folderを作成し、その中へ空File `Original.txt`を作成する。
3. `Original.txt`を`Renamed.txt`へRenameする。
4. `Destination` Folderを作成し、`Renamed.txt`を移動する。
5. 移動したFileを`Copied.txt`としてCopyし、一覧と検索の双方から見つけられることを確認する。
6. Files WindowにFocusがある状態で`F5`を押し、選択と展開状態を壊さず再取得できることを確認する。
7. Files Window以外にFocusがある状態では、`F5`と`Delete`がFiles操作として発火しないことを確認する。

## Recoverable Delete and Restore

1. `Copied.txt`を選択して`Delete`を押す。
2. 確認DialogにProject相対Path、対象件数、概算Logical Size、復元可能であることが表示されることを確認する。
3. `Escape`でCancelし、元FileとRecovery一覧が変化しないことを確認する。
4. 再度`Delete`を押して確定し、FileがSource Asset一覧から消え、Recovery一覧へ現れることを確認する。
5. Recovery一覧からRestoreし、元Pathへ内容を保持して戻ることを確認する。
6. 復元した`Copied.txt`をもう一度Recoverable Deleteし、Recovery一覧へ戻ることを確認する。
7. 元と同じPathへ別内容の`Copied.txt`を作成する。
8. Recovery一覧から元の`Copied.txt`をRestoreし、既存FileとRecovery Dataの双方を保持して日本語Errorになることを確認する。
9. 競合用に作成したFileを`CopiedConflict.txt`へRenameし、元のRecovery Entryを残したまま再起動確認へ進める状態にする。

## Root Boundary and External Change

1. Files UIから`../Outside.txt`またはTest Project外のPathを移動先として指定する。
2. 操作が日本語Errorで拒否され、Source FileとProject外Dataの双方が変化しないことを確認する。
3. ExplorerなどEditor外のProcessから`Assets/Source`配下へFileを作成、Rename、削除する。
4. Files表示が通知後の再列挙で追従し、消失したEntryのSelectionが安全に解除されることを確認する。
5. 監視対象外のProject外変更がFiles一覧へ現れないことを確認する。

## Restart Recovery

1. Recoverable Deleteを一件残したままEditorを通常終了する。
2. Project Hubから同じProjectを再度開く。
3. Recovery一覧に削除済みEntryが残り、既存Source Assetも保持されていることを確認する。
4. EntryをRestoreし、元Pathと内容が復元されることを確認する。
5. EditorとProject Hubを通常終了し、Test Projectと境界確認用Fileを削除する。

## Native Dialog

`Docs/Testing/M13-native-file-dialog-manual-test.md`の`open`、`save`、`folder`と各Cancel経路を実行する。
Dialog結果をそのまま信頼せず、Project Root相対Locatorへ再検証してからFiles操作へ使用することを確認する。

## Expected Safety

- Files UIはNative Filesystem APIを直接呼ばず、全Mutationを`FilesWorkspaceService`経由で実行する。
- 通常Deleteは永久削除せず、Project-local Trashから復元できる。
- Cancel、競合、Root脱出、再検証失敗では既存Source DataとRecovery Dataを変更しない。
- 外部変更通知の欠落またはOverflow時は、差分推測ではなく権威的な再走査へ退避する。
- Editor再起動後もRecovery Catalogを再構築し、削除済みEntryを復元できる。
