# M11 Editor Build / Package / Run の検証

- 日付: 2026-11-04
- 対象: `m11/issue146/project-loading` の既存作業を保全した追加実装
- 契約: [ADR-0019](../Decisions/0019-editor-build-and-run.md)
- Commit / Push / Merge / Issue 更新は実施していない

## 実装

1. `Build > Build Settings...` で Platform / Architecture / 構成を選択する。
   現在は Windows / x64、Debug / Development / Release。Panel は初期非表示。
2. 未保存なら ImGui で Save and Build / Build Saved Files / Cancel を確認する。
   保存失敗・保存先取消では開始しない。Saved Files は編集内容を保持する。
3. 専用の CMake Tree で非同期 Configure → 製品 Build / Package を実行する。
   二重要求と製品稼働中の再 Build を拒否し、Cancel は Job の子孫を回収する。
4. 状態 / 終了 Code / 出力先 / startupScene / 上限付き Log を表示する。
5. 成功した選択対象と一致する配布物を再検証し、Run で独立起動する。

`Cue.ProjectBuild` が公開状態と抽象 Service、`Cue.ProjectBuild.Windows` が Process の実装、
`Cue.Editor.Ui` が表示と所有 Action、`Cue.EditorHost` が保存と要求の確定を担当する。
起動 Main は生成時の Tool の絶対 Path を注入し、起動 Shell の環境に依存させない。

## 自動検証

- 標準 Debug Build: 成功。`out/validation/EditorGameBuildBuild.log`
- 最終 Service / Editor / Test Target の再 Build: 成功。
  `out/validation/EditorGameBuildFixtureBuild.log` / `EditorGameBuildProbeBuild.log` / `EditorGameBuildFinalBuild.log`
  初回 `EditorGameBuildTargetBuild.log` の Fixture 規格指定不足は `cxx_std_20` を追加して修正した。
- 全 Debug CTest: **113 / 113 成功、104.55 秒**。`out/validation/EditorGameBuildFullTests.log`
- 最終 Header の直接 Include 整理後、Editor の対象 2 Test も成功。
  `out/validation/EditorGameBuildFinalTests.log`、1.30 秒
- `clang-format` / `git diff --check`: 成功

新しい検証項目:

| Test | 確認内容 |
| --- | --- |
| Cue.Editor.BuildUi | 実 ImGui 入力でメニューから設定を開く、Target 変更、Build / Cancel / Run、状態別の抑止、保存確認三択、正本不変 |
| Cue.Editor.GameBuild | 実 Editor / WARP で保存失敗・取消時の未開始、Saved Files の編集保持、保存後開始、Thread 拒否、通常 Frame の継続、製品中の再 Build 拒否、停止 |
| Cue.Project.GameBuildService | 実 Process / Pipe / Job で Unicode / 空白 Path、同 Thread の別 Service と他 Editor の排他、Configure / Build の失敗、取消した孫の停止、再試行、Log 上限、配布物再検証、独立製品、排他 File 偽装時のユーザー File 保全 |

UI Test の初回失敗はテスト側の内部 Menu ID / Popup 名への依存を修正した。
標準 Mouse Event でメニューを開き、実際の Popup Window を使うようにして成功した。

## 実 CMake 接続

元 Sample を TEMP の `cue-ui-cad09dd3` へコピーし、Source / Content の元 File を変更せず確認した。
Editor で使う同じ Windows Service を `CueGameBuildServiceTests --engine-smoke` から実行した。
この任意モードは通常の CTest に含めず、全 Test で Engine の Cold Build を繰り返さない。

- 専用 Tree: `C:/Users/sinse/AppData/Local/Temp/cue-ui-cad09dd3/Intermediate/EditorBuild/8aa15dc2f5782682/Windows/x64`
- 対象: Windows / x64 / Release
- Configure → 実 MSBuild → Package → 配布物の再検証: **成功、終了値 0**
- Log: `out/validation/EditorGameBuildRealRelease.log`
- 出力: `C:/Users/sinse/AppData/Local/Temp/cue-ui-cad09dd3/Build/Windows/x64/Release/Player/RotateCube.exe`

生成した配布物を `CueGamePlayerProcessTests` でさらに別の一時 Directory へ移し、別 CWD から WARP で起動した。
可視 Window、静的登録、ゲーム DLL / Editor / PlayHost の非 Load、Script 回転、90 Render 以上、正常終了を確認した。
完成 Marker を欠損させた再起動も拒否した。終了値 0、成功時出力なし。
記録は `out/validation/EditorGameBuildRealPlayer.log`。

TEMP に生成したため MSBuild の MSB8029 Warning は出たが、Build / Package は成功した。
通常の Project 配置では専用 Tree もその Project の Intermediate 配下となる。

## 画面確認と未実施

実 Editor の Scene View と既定非表示の Build Panel / Build メニューを確認した。
続く手動操作は利用者の PC 操作と競合したため継続していない。
全手動制作経路の完走、画面上の Build ボタンから実 CMake 完了・Run までの連続操作は未確認。
UI Action → Host と Service → 実 CMake / 配布起動は、それぞれ上記の自動検証で確認した。

開発用ゲーム DLL の Build / Script Types Reload は従来の開発経路を利用する。
他 OS / Architecture、他 PC の Tool 導入条件、Development の製品出力は今回未検証。
配布更新失敗時に前の Package 一式を保持する保証はない。未完成 Marker の配布物は起動拒否とする。
