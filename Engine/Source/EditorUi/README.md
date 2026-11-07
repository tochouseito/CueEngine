# EditorUi の入力境界

EditorUi は SceneDocument を直接変更せず、所有値の EditorAction を UI 構築後の適用先へ渡す。
Transform の数値入力は `ImGuiItemFlags_LiveEditOnInputScalar` により Enter 前から有効な値を Preview する。

Escape の観測だけは Private の `TransformInspectorPanel.cpp` と `EditorUi.cpp` で `imgui_internal.h` の
`IsKeyPressed(key, flags, owner_id)` を利用する。現在の依存先は Dear ImGui 1.92.9b。
公開版の `IsKeyPressed` は InputText の所有権で遮断され、`Shortcut` で優先権を奪うと
InputText 自身の Escape 復元を妨げるため、公開 `GetItemID()` が返す実 Scalar の ID を使って
所有権を変更せずに観測する。内部 Context や文字列 Buffer は変更しない。
未保存確認 Modal は初回表示 Frame の取消も受け取るため、所有者を限定しない観測を使う。
Modal 内に Text 入力は置かず、他の入力欄の内容をこの観測で書き換えない。

Cancel Frame にも Scalar を描画して ImGui 内部の復元・Deactivate を完了させる。
その Frame の外部 Begin / Preview / Commit は抑止し、Cancel を一件だけ返す。
ImGui 更新時には、この内部 Overload・Group の Active Item ID の転送と、
`Cue.Editor.Ui` の数値入力中 Preview / Escape 復元 / 非有限入力の回帰テストを確認する。

Script の型情報は別 Process が返した所有 Metadata を draw 中だけ借用する。
EditorUi は Game DLL を読み込まず、Script の追加・削除・有効化・Property 編集を所有 Action にする。
数値 Property は Enter で確定し、Boolean は切替時に確定する。未解決型と Schema 不一致の設定は保持する。

Play 中は通常の Scene 編集を抑止し、Script Property だけを子 Process への変更要求にする。
表示には Play 開始時の Scene 設定と受理された応答を用い、SceneDocument の保存値・Undo を変更しない。
Widget ID は Authoring / Play と Session ID で分離し、子の終了・再起動時に未確定の入力を持ち越さない。
実行中の Transform の観測やゲーム描画の Editor 内表示は、この UI 接続とは別の機能である。
