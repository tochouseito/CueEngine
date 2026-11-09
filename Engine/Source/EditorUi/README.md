# EditorUi の入力境界

EditorUi は SceneDocument を直接変更せず、所有値の EditorAction を UI 構築後の適用先へ渡す。
Transform の数値入力は `ImGuiItemFlags_LiveEditOnInputScalar` により Enter 前から有効な値を Preview する。

Escape の観測だけは Private の `TransformInspectorPanel.cpp` と `EditorUi.cpp` で `imgui_internal.h` の
`IsKeyPressed(key, flags, owner_id)` を利用する。現在の依存先は Dear ImGui 1.92.9b。
公開版の `IsKeyPressed` は InputText の所有権で遮断され、`Shortcut` で優先権を奪うと
InputText 自身の Escape 復元を妨げるため、公開 `GetItemID()` が返す実 Scalar の ID を使って
所有権を変更せずに観測する。Escape の観測では内部の文字列 Buffer を変更しない。
選択 / Play Session / Authoring と Play の切替時は、旧 Scalar の Active ID が一致するときだけ
`ClearActiveID` で入力の所有権を破棄し、別の Object や Session に入力を持ち越さない。
未保存確認 Modal は初回表示 Frame の取消も受け取るため、所有者を限定しない観測を使う。
Modal 内に Text 入力は置かず、他の入力欄の内容をこの観測で書き換えない。

Cancel Frame にも Scalar を描画して ImGui 内部の復元・Deactivate を完了させる。
その Frame の外部 Begin / Preview / Commit は抑止し、Cancel を一件だけ返す。
ImGui 更新時には、この内部 Overload・Group の Active Item ID の転送と、
`Cue.Editor.Ui` の数値入力中 Preview / Escape 復元 / 非有限入力の回帰テストを確認する。

Script の型情報は別 Process が返した所有 Metadata を draw 中だけ借用する。
EditorUi は Game DLL を読み込まず、Script の追加・削除・有効化・Property 編集を所有 Action にする。
数値 Property は Enter で確定し、Boolean は切替時に確定する。未解決型と Schema 不一致の設定は保持する。

Play 中は通常の Scene 編集を抑止し、Script Property と Transform を子 Process への変更要求にする。
Transform は子 World の最新 Snapshot から表示し、入力中だけ再同期を抑止する。
Escape は入力開始値を子 Process へ要求し、Authoring の Begin / Preview / Commit は発行しない。
表示には Play 用の所有 Snapshot と受理された応答を用い、SceneDocument の保存値・Undo を変更しない。
Widget ID は Authoring / Play と Session ID で分離し、子の終了・再起動時に未確定の入力を持ち越さない。

SceneView は Host が公開した描画 Texture を表示し、Play 中は別 Process の実行結果を用いる。
右 Mouse と WASD / QE、右 Mouse Drag、Wheel の操作は表示用 Editor Camera の要求に限定する。
Camera 操作は SceneView の Image を Hover し、Panel に Focus がある間だけ有効にする。
`Scene camera` は Editor Camera の上書きを解除し、Scene にある Camera の表示へ戻す。
保存 Scene / 実行 World の Camera と、ゲーム側への入力は変更しない。
