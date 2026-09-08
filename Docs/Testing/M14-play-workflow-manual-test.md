# M14 Editor Play Workflow Manual Test

## Purpose

ProjectとSceneを開いた同一Editor Processで、編集、Play、Stop、再編集、再Play、終了時停止を確認する。
Game Rendering、Play Changes Apply、Scripting、Performance測定は対象外とする。

## Preconditions

1. `Debug|x64`の全TargetをBuild済みであること。
2. `out/build/windows-vs2026/bin/Debug/CueProjectHubTool.exe`と同じDirectoryに
   `CueEditorTool.exe`が存在すること。
3. Test専用Projectを使用し、確認前にSceneを保存すること。
4. Runtime Consoleを表示できるWindow SizeでEditorを起動すること。

## Edit, Play, and Stop

1. Project HubからTest Projectを開き、保存済みSceneへObjectを一つ追加する。
2. Objectを選択して名前またはTransformを変更し、SceneがDirtyになることを確認する。
3. Runtime ToolbarのPlayを押し、状態が「実行中」へ変わることを確認する。
4. Play中にKeyboardまたはMouseを操作し、Editor自体の操作とRuntime入力で異常終了しないことを確認する。
5. Runtime ToolbarのStopを押し、状態が「停止済み」へ変わることを確認する。
6. Play前のDirty状態、Object内容、Selectionが維持され、Runtime側の変更がAuthoring Sceneへ反映されていないことを確認する。
7. Objectを再編集し、F5でPlay、Shift+F5でStopできることを確認する。
8. Files WindowにFocusがある状態の通常F5はFiles更新だけを行い、Shift+F5はPlay停止だけを行うことを確認する。

## Repeated Session

1. Play、1秒以上待機、Stopを10回繰り返す。
2. 各回で新しいPlayが開始でき、前回の状態やErrorが次のSession開始を妨げないことを確認する。
3. 各Stop後にSceneを編集でき、SelectionとDirty状態が維持されることを確認する。
4. Runtime ConsoleのFilter、Clear、Copyを操作し、Editor操作を妨げないことを確認する。

## Editor Close During Play

1. Playを開始したままEditorを閉じる。
2. 確認DialogでCancelし、EditorとPlayが継続することを確認する。
3. 再度Editorを閉じ、Stopして閉じる選択を行う。
4. Editorが正常終了し、同じProjectをProject Hubから再度開けることを確認する。
5. 保存済みSceneがPlay前の内容を保持し、明示保存していない編集は通常のRecovery／Close契約に従うことを確認する。

## Automated Failure Coverage

利用者操作で意図的に再現しないLoad失敗、Runtime System Start失敗、InputからSystem Updateへの順序、
12回のSession所有解放、Editor破棄時の強制Cleanupは`Cue.Editor.Workflow.ProcessRoundTrip`で確認する。

## Expected Safety

- PlayはEditorDocumentから独立Snapshotを生成し、Authoring SceneをRuntime Worldとして直接使用しない。
- StopまたはEditor終了後にRuntime System、Runtime World、SceneInstanceを保持しない。
- Load／System Start失敗後もEditorDocument、Selection、Dirty状態を維持し、再Playできる。
- Stop入力を受けたFrameでは、その後にRuntime Updateを追加実行しない。
