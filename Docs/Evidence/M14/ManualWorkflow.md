# M14 Editor Play Workflow Record

## Status

Pass.

2026-09-08にComputer UseでLocal Windows x64のDebug Toolを実Window操作した。
通常のPlay／Stop、Shortcut、10回反復、Runtime Console、Play中終了、再OpenはPassした。
ADR-0021が手動確認を求める開始失敗後の再Playは、実WindowからFailureを注入する経路がないためNot Runとする。

## Procedure

- `Docs/Testing/M14-play-workflow-manual-test.md`

## Environment

- Source Commit: `56f6aa68a3980cb8f79141060d23bbd54688d46d`
- Source Tree: `c04f4d312a326bcc57a5ee435c4e3e41d2a8a0d4`
- Configuration: `Debug|x64`
- Project: `C:\Users\sinse\AppData\Local\Temp\CueEngineM12Manual\M12Gate`
- Scene Locator: `Scenes/NewScene.cuescene`
- Tool: `CueProjectHubTool.exe`、`CueEditorTool.exe`

## Result Checklist

| Check | Result | Note |
|---|---|---|
| Project HubからProjectと保存済みSceneを開く | Pass | 同一ProjectとSceneを使用 |
| Object追加、選択、Transform編集、Dirty State | Pass | 追加ObjectのTranslationを`(2, 3, 0)`へ変更 |
| PointerのPlay／Stop | Pass | Session 1で状態遷移と停止を確認 |
| Play中のKeyboard／Mouse操作 | Pass | Editor操作を続けてもRuntime Updateが継続し、異常終了なし |
| Stop後のSelection／Dirty／Authoring内容保持 | Pass | 同じObject選択とTransformを保持 |
| F5 Play／Shift+F5 Stop | Pass | Session 2で確認 |
| Files focus時のF5／Shift+F5 | Pass | F5はFiles更新だけ、Shift+F5はSession 3を停止 |
| 10回連続Play／Stop | Pass | Session 4から13。各回1秒以上実行し、Session／Worldが一意に更新 |
| 各Stop後の再編集可能性 | Pass | 選択、Dirty State、Translation `(2, 3, 0)`を保持 |
| Console Filter | Pass | `Editor Play`で対象行だけを表示 |
| Console Copy | Pass | Pointer操作後もEditor操作を継続 |
| Console Clear | Pass | 実行中Logを消去し、表示が空になった後もPlayとStop操作を継続 |
| Load／System Start失敗後の再Play | Not Run | 実WindowのFailure Injection経路なし。`Cue.Editor.Workflow.ProcessRoundTrip`で自動検証 |
| Play中CloseのCancel | Pass | Session 14が継続し、Frame増加を確認 |
| Stopして閉じる | Pass | Session 14を停止し、未保存変更確認へ遷移 |
| Scene保存とEditor正常終了 | Pass | `保存`を選択し、Editor Process終了を確認 |
| Project Hubから同じProjectを再Open | Pass | 新しいEditor Processで再Open |
| 保存済みScene内容 | Pass | 追加ObjectとTranslation `(2, 3, 0)`を再読込み |
| 最終Process終了 | Pass | CueEditorTool／CueProjectHubToolのWindowが残っていないことを確認 |

## Session Evidence

- Pointer Play／Stop: Session 1
- F5／Shift+F5: Session 2
- Files focus: Session 3
- 10回反復: Session 4から13
- Close During Play: Session 14
- Console Clear再確認: Editor再起動後のSession 1

各Sessionは対応するWorld番号と一致して増加し、10回反復では各回1秒以上待機してから停止した。
停止後は次のSessionを開始でき、前回のRuntime Worldが次回開始を妨げなかった。

## Close and Persistence Evidence

Session 14実行中にEditor終了を要求し、最初のDialogでは`キャンセル`を選択した。
Editorは開いたままRuntime Frameを更新し続けた。再度終了を要求して`停止して終了`を選択すると、
Runtimeが停止して通常の未保存変更Dialogへ遷移した。Sceneを保存して終了し、Project Hubから同じProjectを
再度開いた後、`Scenes/NewScene.cuescene`に追加ObjectとTranslation `(2, 3, 0)`が保持されていることを確認した。

## Automated Supporting Evidence

- `Cue.Input.State`: Pass
- `Cue.Input.Windows.MessageSink`: Pass
- `Cue.GameCore.Clock`: Pass
- `Cue.GameCore.RuntimeSystemRegistry`: Pass
- `Cue.Runtime.SceneSession`: Pass
- `Cue.Runtime.ApplicationSession`: Pass
- `Cue.EditorCore.PlaySession`: Pass
- `Cue.Editor.ImGui.PlaySession`: Pass
- `Cue.Editor.Workflow.ProcessRoundTrip`: Pass
- `Cue.RuntimeHost.Smoke`: Pass

## Recording

Computer Useが返した各時点のScreenshotを直接確認した。Screenshotまたは操作録画はFileとして保存していない。

## Result

Runtime ConsoleへPlay開始Logを表示して`Clear`をPointer操作し、Log表示が空になったことを確認した。
Playは継続しており、続けてStopすると停止要求と停止完了の新しいLogが表示された。
これにより、`Clear`後もEditorとRuntime操作を継続できることを確認した。

開始失敗後の再Playは実Windowでは確認していない。Load失敗、Runtime System Start失敗、
EditorDocument／Selection／Dirty State保持、失敗後の再Playは`Cue.Editor.Workflow.ProcessRoundTrip`で検証済みだが、
実WindowのFailure表示と利用者による再操作は未検証Riskとして残す。
