# ADR-0016: C++ Script と別プロセス Play

## 決定

- ゲーム側の継承入口は `Behaviour` とし、World 全体の実行処理は既存 `WorldSystem` の派生 `ScriptSystem` に集約する
- `GameObject` は世代付き Entity Handle と Callback 中だけ借りる API の窓口。Transform / Component の実データは World の ECS が所有する
- ゲーム開発用 DLL と製品用 Static Library は同じ Source / 型登録を使う。C++ Virtual Interface、STL、Result、Allocator の所有権は DLL 境界を越えない
- Module 境界は `Script/Abi.h` の Version 付き C ABI。固定幅の値、上限付き借用文字列、固定長 Status、Opaque Instance Token を使う
- Module は自分の Instance を生成・破棄し、ScriptSystem はその Token と DLL の寿命を所有する
- ABI Version / Table Size / Pointer Size / Build Configuration / Metadata を検査してから利用する
- SDK / 公開データ / Runtime の公開契約へ Win32 / DX12 / ImGui 型を含めない。DLL Load と Process / Pipe の具体実装だけを Windows に閉じる

## ゲーム側の登録入口

Editor の `Script > New Script...` が `Source/<Name>.cpp` と v2 の対応表を作る。
各 cpp は `Script/ModuleEntry.h` の `CUE_DEFINE_SCRIPT(...)` で、自分の型登録だけを定義する。
ゲーム側の手書き `Game.cpp` は不要とし、CMake が一つの接続入口を Binary Tree へ自動生成する。

```cpp
CUE_DEFINE_SCRIPT(RotateCube,
    cue::make_script_registration<game::RotateCube>({"game.RotateCube", "Rotate Cube", 1, {}}))
```

型 ID / 表示名 / Schema Version / Property は各登録で明示する。
`cue_add_game_scripts(target)` は v2 の `ScriptSources.cueide` に載る Source と登録 Symbol を検証する。
生成入口が各登録関数を明示参照するため、Static Link でも登録が除去されない。
自動生成される `CUE_DEFINE_SCRIPT_MODULE(...)` は `CueScriptGetModule`、Module の寿命と例外境界を共通化する。
生成済みの Result 失敗は保持し、登録式が例外を投げた場合は次回呼出で初期化を再試行する。
既存の手書きの入口も利用できるが、同じ Module へ二つの入口を定義しない。
旧 v1 の対応表と手書き `cue_add_game_module` は利用を継続できる。新規作成機能は v2 への移行を要求する。
Source 作成は排他取得後に CreateNew し、登録表を最後に原子的に公開する。失敗時は未変更の新規 Source だけを回収する。
登録式は初回呼出中に評価するため、他 cpp の可変 Global 初期化順序や自動収集に依存させない。
ABI Version / 型 ID / 所有権 / Instance の破棄手順は変更しない。

## 所有と Lifecycle

1. 検証済み Scene を実行 World に実体化し、ScriptComponent を追加する
2. World に ScriptSystem を登録する。全割当の型・Schema・Property を検証する
3. Module の Factory が既定値を適用して Instance を生成し、保存設定で上書きする
4. 初めて有効になったとき start を一度呼び、以後有効な間だけ update を呼ぶ
5. Entity / Script の削除、Play 終了、失敗時には開始済み Instance の stop を一度呼ぶ。部分 start の回収も stop の責務
6. 全 Token の destroy を試みてから DLL を解放する

非 Active な Entity / Component / Script も型検証する。無効化は更新を止めるだけで start を繰り返さない。
ScriptComponent の削除 / Entity 削除は次の更新で照合するため、stop の GameObject は既に無効な場合がある。
後始末は保存した所有資源を回収し、World 操作が必要なら先に is_alive を確認する。
Native Code の例外は ABI 内で Status に変換する。Setter / Lifecycle の失敗後は実行を継続しない。
destroy が失敗した DLL は Unload せず、失敗した Play プロセスの終了で OS に回収させる。
SDK の利用者は Callback 内の GameObject / Engine API の参照や Component 借用を保存しない。
診断は ScriptSystem に Host が注入する Callback へ同期配送する。Runtime 自身は File 出力先や Native Logger を所有しない。
Setter の成功は要求された設定値を受理したことを表す。ゲーム内部の可変状態を自動で読み戻す Reflection ではない。

## データと保存

- ScriptData は Runtime / Editor / SDK から独立した所有値を提供する
- 型の安定 ID、割当 UUID、Schema Version、有効状態、Property ID と型付き値を保存する
- 初期の Property は Number（有限 double）と Boolean。文字列、配列、Object / Asset 参照は後続の Schema 拡張
- `.cuescene` v1 は読み互換を維持する。Script を含む Scene は v2 に昇格し、未知 Script も設定を失わず保持する
- 型未解決、Schema 不一致、未知 Property は Play 初期化で拒否する。黙った型変換や値破棄を行わない
- Editor の割当・削除・設定変更は一回の Undo Command として正本へ確定する

## Editor と Play

- Editor は Game DLL を Load しない。専用 CuePlayHost プロセスで型情報を取得し、所有データだけを IPC で受け取る
- Play 開始時、未保存の Scene も Snapshot として IPC で子へ渡す。起動のために Scene を自動保存しない
- Editor の Preview World と子の実行 World は独立する。ゲーム実行が Preview World の状態や SceneDocument を書き換えない
- 初期 CuePlayHost は Main / Update を一つの World Owner に固定する。将来 Worker 化するときは、その Thread で World の生成・初期化・更新・停止・破棄を行う
- 描画は確定した World から所有 Render Snapshot を抽出して既存 SceneRenderer へ渡す
- Play 中の Inspector は Script Property 変更要求を Session / Request ID 付きで送る。子の更新前境界で適用し、受理応答後に Play 用表示値だけを更新する
- 通常編集は Play 中に抑止する。Play の変更を Authoring Scene へ自動で戻さない
- Process 終了 / Crash / 通信失敗を親が検出する。無応答時の Stop は期限後に所有 Job ごと回収する
- 通信の Version / Session / 長さ / 件数を検証し、Main の poll を非待機にする
- Inspector の入力 Widget は Authoring / Play / Session ごとに分離し、終了・Crash をまたいで未確定入力を編集正本へ持ち越さない
- 通信と別プロセス化は障害の分離であり、信頼できない Code を安全に実行するための Sandbox ではない

## 初期 Sample と後続

`Samples/RotateCube/Source/RotateCube.cpp` は `sample.RotateCube` を登録する。
`speed` は毎秒の回転角度、`isRotating` は更新の ON / OFF。数学計算は Math Module を使う。
CueEditorHost の Sample 起動では実行 File 隣の CuePlayHost.exe / RotateCubeGame.dll を明示する。
他 Project への Module の自動適用はしない。Library 利用時は EditorHostConfig に両 Path を指定できる。
Project ごとの Build / Artifact 選択 UI は別段階で整備する。

初期再読み込みは Stop → Build → Play。状態を保持した Hot Reload、Play の埋込み表示、一般の World State 同期、
任意 Component の ABI 登録、独立アプリの Package / Build UI はこの基盤だけで完成したものと扱わない。

Editor SceneView への埋込み表示と実行 Transform の同期は、後続の
[ADR-0031](0031-editor-embedded-play.md)で定義する。Game DLL の Process 分離と編集正本の保全は維持する。
