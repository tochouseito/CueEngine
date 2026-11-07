# M11 手順5: Cube 追加・Transform 編集

- 日付: 2026-11-02
- 対象 Branch: `m11/issue146/project-loading`
- 契約: `Docs/Decisions/0015-editor-scene-document-and-ui.md`
- 作業前からの未 Commit 変更を保全し、Git / GitHub の更新は行っていない

## 実装

- Cue.Editor: SceneDocument / EditorApplication、選択・履歴・保存基準・Preview World の管理
- Cue.Editor.Ui: File / Edit / View、Objects、Inspector、Scene View、Status
- Cube の追加・選択、Position / XYZ 回転（度）/ Scale の編集
- 一回の連続入力を一件にまとめる Undo / Redo、Escape による取消
- Scene の新規・Open・Save・Save As、未保存確認と終了取消
- Math の Quaternion → Euler 変換、入力中の Quaternion を不用意に再生成しない表示 Buffer
- Windows 固有 UUID / Dialog / 終了予約の private サービス
- 保存成功は File 公開で判定し、電源断耐性は FileSaveOutcome の別情報として維持

UI が返した所有値の Action を、ImGui の借用と Lock を解放した後に Main で適用する。
Preview の変更を同じ Frame ID の値 Snapshot に反映し、Render へ World / Entity を渡さない。
通常の Transform 編集では World を交換せず、構造変更は候補 World の準備後に公開する。

## 自動確認

- 既定 Debug / x64 を `scripts/codex_build.ps1` で Build
- 追加 Test: Cue.Editor.Application / Document / Ui / WindowsServices、
  Cue.Math.Euler、Cue.Platform.Windows.CloseCancellation
- 不正 Transform、候補 World 生成失敗、保存公開前失敗、Content / Link 境界、
  保存内容へ戻る Undo / Redo、履歴128件、Owner Thread、Close 状態復帰を検証
- 実 ImGui 入力の Cube 追加、保存要求、回転への `nan` 入力、Menu 実クリック、
  旧 Layout の上端・下端補正、論理 Scene Texture ID を CPU Test で検証

保存と Layout の修正後、既定 Debug Build と全93 Test が成功した（77.88秒）。
続けて、数値の入力中 Preview / Escape 復元を追加し、Editor / UI Test を対象 Build した。
追加回帰を含む `Cue.Editor.Ui` も成功した（0.15秒）。

## 実際の Editor 操作

元の Sample を維持するため、`out/validation/EditorEditingProject` に Project をコピーして確認した。
実操作は computer-use の対象を検証用 CueEditorHost に限定して行った。

1. File / New Scene で Camera だけの Scene を生成
2. Add Cube、一覧選択、Inspector 表示
3. Position X=0.4、Rotation Y=30度、Scale X=1.2を入力して表示変化を確認
4. Undo / Redo による Scale と見た目の復元
5. Save As で `Content/Scenes/Manual.cuescene` を保存
6. Open Scene で保存内容を再読込し、Cube の描画を確認
7. 未保存終了の取消後に Editor が継続することを確認

最終版では、通常 Save 後に `Scene saved` と表示され、未保存マークが消えることも確認した。
数値入力を Enter で確定する前に値が Preview へ反映され、Title Bar から終了すると
未保存確認が開くことを確認した。終了取消後にも位置を再編集できた。
その後、Position X=0.4 を再保存して通常 Close で終了した。
別の確認では、終了確認の「はい」で保存してそのまま閉じる経路も通した。

保存本文も読取り、UUID、Camera、Cube、上記 Transform が含まれることを確認した。
実操作で見つかった Menu / Status の重なり、Shell の Path 区切り、
保存公開と電源断保証の混同を修正した。

## 記録

- `out/validation/EditorEditingVerifiedBuild.log`: 最終の既定 Debug Build
- `out/validation/EditorEditingVerifiedCTest.log`: 最終コードでの全 Test
- `out/validation/EditorEditingCoreCTest.log`: 初期の編集ドメイン・Native サービス検証
- `out/validation/EditorEditingFinalCTest.log`: Menu / Host 関連の個別回帰検証
- `out/validation/EditorEditingLiveInputBuild.log` / `EditorEditingLiveInputCTest.log`:
  数値入力途中の Preview / Escape 復元を含む対象 Build / Test
- `out/validation/EditorEditingProject/Content/Scenes/Manual.cuescene`: 手動作成の検証 Scene

## 未実施・制約

- Development / Release / Windows 以外の Build は未実施
- 実 OOM / Device Lost / 電源断の故障注入は未実施
- Scene の保存で Project の startupScene は自動更新しない
- Content の Path 検証は保守的に大小文字を区別し、Link を拒否する。
  外部 Process が検証後に Link を差し替える競合までは防がない
- Gizmo / Picking / Bounds、Object 削除、汎用 Component 編集、Script、Play、アプリ Build は未実装
- Escape の所有権を観測する処理だけ ImGui 内部 API を使う。
  ImGui 更新時の追従確認は `Engine/Source/EditorUi/README.md` に記録した
- Script / アプリ出力を含む RotateCube の手動制作経路全体の完了とは扱わない

## 追記: 通常の未保存確認を ImGui に統一

通常の未保存確認を EditorUi の ImGui Modal へ移した。
Native の `confirm_unsaved_scene` / `UnsavedSceneChoice` は削除し、
ImGui 初期化前の Project 読込失敗に使う Main.cpp の MessageBox は維持した。
File 選択の Windows Dialog は継続利用する。

Host が New / Open / Exit と選択済み Path を所有値で保留し、
UI は Save / Discard / Cancel の Action だけを返す。
確認中は背景操作と Shortcut を抑止し、Frame の構築・描画を継続する。
Title Bar Close は一度取り消し、確認後に Close を再予約する。
保存先選択の取消・保存失敗は編集へ戻し、古い保留要求を後から実行しない。
応答消費時の `ui.reset()` により、false Frame を挟まない連続 Close でも再表示できる。

検証結果:

- 既定 Debug Build 成功
- 全93 Test 成功（70.42秒）
- 保存後の新規 Scene 表示通知を調整後、関連7 Test も成功（15.70秒）
- UI Test で3択、Escape、右上 X、初回 Escape、連続要求、背景入力抑止、Document 不変を確認
- 実 Editor で終了 Cancel / Escape、Save 後の New、Save As 取消後の編集継続、
  Discard 後の New、保留した Path による Open、Discard 後の正常終了を確認
- 検証用コピーは `out/validation/ImGuiUnsavedProject`。元の Sample は変更していない

記録:

- `out/validation/ImGuiUnsavedVerifiedBuild.log` / `ImGuiUnsavedFullCTest.log`: 全体検証
- `out/validation/ImGuiUnsavedUiBuild.log` / `ImGuiUnsavedUiCTest.log`: UI 単独検証
- `out/validation/ImGuiUnsavedFinalBuild.log` / `ImGuiUnsavedFinalCTest.log`: 最終の関連検証

Development / Release、実 OOM / Device Lost の故障注入は未実施。
ImGui 内部の Escape 観測 API に関する追従条件は EditorUi/README.md に記録している。
Commit / Push / Merge / Branch 作成は行っていない。
