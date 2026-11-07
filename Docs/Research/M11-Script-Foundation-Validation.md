# M11 Script 基盤の実装・検証

## 実装範囲

設計は [ADR-0016](../Decisions/0016-native-script-and-isolated-play.md) に従う。

| Module | 内容 |
| --- | --- |
| ScriptData | 保存用割当、Number / Boolean、型情報、Schema 検証 |
| Script | C++ Behaviour / GameObject と C ABI Adapter、Module 内 Instance 所有 |
| ScriptRuntime | 動的 / 静的 Game Module 検証、WorldSystem 派生 ScriptSystem、Scene 割当接続 |
| ScriptPlay | Windows の子 Process / Job / 非待機 Pipe、Session / Request の検査 |
| PlayHost | 子だけで DLL・実行 World・Renderer を所有する CuePlayHost |
| Scene / Editor / EditorUi | v2 Script 保存、Undo / Redo、割当・設定変更、Play / Stop |

Sample のゲーム Source は `Samples/RotateCube/Source/RotateCube.cpp`。
開発用 RotateCubeGame.dll と製品 Link 用 RotateCubeGameStatic.lib を同じ Source から生成する。
元の Sample Scene の配置・Transform・Cube 数・保存状態は変更していない。

## 操作経路

1. 通常の Debug Build 後、CueEditorHost を既定の RotateCube Project 引数で起動する
2. 起動時に別の検査プロセスが型情報を返す。失敗時は Script → Reload Script Types で再試行できる
3. Object 一覧から Cube を選び、Inspector の Add Script で Rotate Cube を割り当てる
4. Speed (degrees/sec) を入力して Enter で確定する。Rotate は ON / OFF
5. 必要なら Scene を保存する。Script の追加・削除・有効化・Property 編集は Undo / Redo 対象
6. Play を押すと、未保存 Scene も Snapshot として別の CueEngine Play ウィンドウへ送る
7. Play 中の Property 変更は子への要求となり、応答受理後だけ Play 表示値を更新する
8. Stop または Play Window の Close で終了する。Authoring の値・Dirty・履歴へ書き戻さない

ゲーム Source の変更後は Stop → 通常 Build → Reload Script Types → Play を使う。
Editor 内から Source Build を起動する UI、Hot Reload、Play 画面の埋込み表示は未追加。
Sample 以外の Library 利用では EditorHostConfig の playExecutable / scriptModule に絶対 Path を明示する。

## 自動検証

Windows x64 / Debug、2026-11-02 の検証。

- `pwsh -NoProfile -File scripts/codex_build.ps1`
- `ctest --preset windows-vs2026-debug -R 'Cue\.(Script\.|Scene\.Script|Editor\.Script)' --output-on-failure`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- ABI Header を MSVC の C モード `/TC /Zs /utf-8` でも構文確認
- `git diff --check`

追加テストは SDK、Runtime、Scene Script、Editor Script、ModuleLoading、ScriptUi、Play の 7 件。
公式 Debug Build は成功。追加 7 件は全件成功（6.30 秒）、既存を含む全 100 件も全件成功（85.79 秒）。

| 検証 | 内容 |
| --- | --- |
| SDK / ABI | 型・Property・既定値・失効 Token、Result / 例外、開始・停止順 |
| Runtime | 複数 Behaviour、同じ Scene の複数 Instance、無効化、削除、診断注入、Owner Thread |
| 失敗 | 欠損型 / Schema を開始前に拒否、部分 start の stop、update 例外後の再更新拒否 |
| ModuleLoading | 実 DLL と Static 入口で同じ RotateCube を更新し、Quaternion の回転結果を数値検証 |
| Scene / Editor | v1 読み互換、v2 round-trip、未知型保持、重複 / 不正入力、Undo / Redo、候補生成失敗の Rollback |
| ScriptUi | Number / Boolean、Enter まで入力保持、一回だけ送信、Play 終了をまたぐ未確定入力の破棄 |
| IPC | 150 KB Scene、約 2.3 MB Metadata、誤 Header / 未知 Request / 部分末尾、終了時の排出 |
| Process | 別 Window の描画経路を 10 Tick 完走、Property 応答、再 Play、起動直後取消、Native 終了相当・通常失敗・無応答の回収 |

ログ:

- `out/validation/ScriptFoundationFinalBuild.log`
- `out/validation/ScriptFoundationTests.log`
- `out/validation/ScriptFoundationFullCTest.log`

## 検証の限界と残る作業

- Development / Release の今回の変更は未検証。Static 登録の実行確認も Debug で行った
- UI 入力は ImGui の自動テスト。実 Windows UI で手動制作手順を最初から最後まで操作した検証ではない
- Play の描画は実行経路の正常完了を確認した。Play Window の画像内容を自動 Readback / 目視で比較したものではない
- 初期 Play は Main / Update を単一の World Owner に固定。Worker Update への移行時は生成から破棄までの Thread を揃える
- 実行用 Scene は一つ。一般の Entity 状態の Inspector 同期、複数 Scene を指定する IPC は後続
- ゲームの Native Code は同じ Play プロセスを落とせる。別プロセス化で Editor を分離する
- 汎用 Project の成果物選択、Editor Build / Package UI、独立製品 exe の出力完走は後続

初回の制限環境では MSVC の PDB 管理起動が C1902 で失敗したため、通常実行環境で公式 Build を検証した。
Code 規約の例外は追加していない。Commit / Push / Merge / Issue 更新は行っていない。
