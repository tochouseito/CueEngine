# ADR-0015: Scene の編集正本と Editor UI の分離

- 日付: 2026-11-02
- 状態: 採用
- 対象: M11 手順5、Cube の手動追加・Transform 編集・Scene 保存

## 所有境界

`Cue.Editor` は Native UI / ImGui / DX12 に依存せず、SceneDocument と
EditorApplication を持つ。SceneDocument が Scene、選択中の永続 Object ID、
保存先、保存済み内容、編集履歴と編集中の Transform を保持する。
EditorApplication が変更操作を検証し、派生 Preview World を更新する。
全変更・破棄は生成した Main Thread に限定する。

`Cue.Editor.Ui` は File / Edit / View メニュー、Objects、Inspector、Scene View、Status を定義する。
UI は不変 Document を借り、所有値の EditorAction を返す。
Panel の構築中には Scene / World / FileSystem を変更しない。
回転の表示は度数法 XYZ、保存は既存の Quaternion とし、相互変換は Math が担当する。

EditorHost は Window、ImGui、EditorApplication、EditorUi と Renderer を接続する。
ImGui の構築と Lock が終わった後に Action を適用し、Preview を同期して、
同じ Frame ID の Scene Snapshot と UI Snapshot を公開する。
Render Thread は World / Entity を借りず、値 Snapshot のみ参照する。
既存の性能診断・設定 UI は Host の既存拡張として維持し、View / Diagnostics で表示する。

## 編集と履歴

- New Scene は一つの Perspective Camera（位置 Z=3）を作る。
- Add Cube は UUID を発行し、組み込み Cube と Transform を追加して選択する。
- 選択には永続 Object ID を使い、World の再構築後も意味を保つ。
- Position / Rotation / Scale を編集できる。Bounds、Picking、Gizmo は含めない。
- Drag / 数値入力は Begin → Preview → Commit とし、一操作につき履歴一件とする。
  Escape は開始値へ復帰し、履歴を増やさない。
- Undo / Redo の上限は128件。保存済み内容との比較で未保存表示を判定する。
- 通常の Transform 編集は既存 World / Entity を維持し、構造変更は候補 World を完成させてから交換する。
- 不正値や候補 World の生成失敗では、正本・選択・履歴を部分公開しない。

## 保存と切替

Open / Save / Save As は `.cuescene` を対象にし、Project 指定時は Content 配下へ限定する。
既存の FileSystem 抽象と `replace_file` による原子的公開を使う。
公開前の失敗は保存先・保存基準を維持する。
公開成功時は保存基準を更新して未保存表示を解除する。
電源断耐性が未確認の場合も、通常の保存成功とは区別して FileSaveOutcome に保持する。
Windows の現行実装は Directory Entry の電源断耐性を保証しないため、
この値を未保存表示・正常終了の拒否に使わない。
選択・履歴・UI 状態は Scene File に保存しない。
Scene の保存で Project の startupScene は変更しない。

Windows の UUID、File Dialog、終了予約は EditorHost の private Native サービスに閉じる。
通常の未保存確認は EditorUi の ImGui Modal とし、New / Open / Exit の要求と選択済み Path を
Host が所有値で保留する。確認中も Frame を継続し、背景の編集操作・Shortcut を抑止する。
Title Bar Close は一度取り消して Window の CloseRequested を要求前の状態へ戻し、
保存・破棄が選ばれた後に通常の Close を再要求する。
Save の取消・公開失敗では保留要求を破棄して編集へ戻る。重複 Close は最初の保留要求を上書きしない。
ImGui の初期化前に起きる Project 読込失敗などは Windows の MessageBox を維持する。
従来の必須 Close Event と ImGui の Message Handler 所有は維持する。

## 今回の範囲外

Script の作成・実行・割当、Play / Stop、アプリ Build、汎用 Component 編集、
Object 削除、親子階層編集、Asset Import、Gizmo / Picking は後続とする。
