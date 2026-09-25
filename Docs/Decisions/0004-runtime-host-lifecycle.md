# ADR-0004: WindowsHost と共通 Runtime の構成とライフサイクル

- Status: Accepted（M03 の契約。Windows 固有の Host と共通 Runtime へ改訂）
- Date: 2026-09-25
- Issue: [M03-01](https://github.com/tochouseito/CueEngine/issues/15)
- 関連: [ADR-0001](0001-architecture-boundaries.md)、[ADR-0003](0003-frame-thread-contract.md)

## 背景

M02 の `CueWindowHost` は Window と `FrameController` の接続を 16ms のダミー処理で検証した。M03 では製品用の起動、Frame 進行、終了の所有者を定める。当初の `RuntimeHost` は Windows の Window と Thread Service を直接生成していたため、別 Platform の Host が同じ Runtime を利用できる境界へ分ける。

## 決定

`CueWindowsHost` を Windows 用 Executable とし、`WindowsHost` をその Composition Root とする。将来の別 Platform は固有の Host を用意する。共通の `Runtime` は Window や Win32 型を持たず、注入された `Clock`、`Waiter`、`ThreadFactory` を借用して `FrameController`、Update／Render Callback、Worker の実行寿命を所有する。M02 の `CueWindowHost` と旧 `RuntimeHost` は重複するため削除する。

`WindowsHost` は `WindowSystem`、`Window`、Windows 用 Thread Service、`Runtime` を一意所有する。Window の Message Pump と生成・破棄は構築 Thread（MainThread）だけで行う。Host の `step()` は Window Event を処理し、Close 要求がなければ `Runtime::step()` を呼ぶ。Close 要求を受けた周回では Frame を進めない。Update／Render Callback は `FrameController` の契約に従う Worker Thread、または単一 Thread fallback では MainThread から実行され、Window を直接操作しない。

M03 の Host と Runtime は Runtime World、Scene、Project、Renderer、GPU Resource をまだ所有しない。M04 では WindowsHost が Windows 用 Renderer／Presentation 実装を選び、共通 Runtime へ必要な契約を渡す。GPU Resource、Descriptor、Fence、Frame Resource の所有者は Renderer／Presentation 側とする。Editor Play でも共通 Runtime を使えるよう、Runtime から Windows 実装への直接依存を作らない。

### API と状態

- `WindowsHostDesc` は Window 設定と `FrameControllerDesc` を持つ。Host は Close 要求まで動作する
- `WindowsHost::initialize(update, render)` は構築 Thread で一度だけ呼び、Window と Service を生成してから Runtime を開始する。Callback が捕捉した参照の Owner は `shutdown()` 完了まで生存させる
- `WindowsHost::step()` は MainThread で Window Event を処理し、継続する場合に Runtime の Frame を一度進める。Close 要求なら `false` を返す
- `Runtime::initialize(update, render)` は借用 Service を使い、Callback を `FrameController` 開始前に登録する。Runtime 自体は Window を生成しない
- `Runtime::step()` と `Runtime::progress()` は実行中だけ有効。Host の `progress()` は Runtime の進行状態を返す
- `WindowsHost::shutdown()` は Runtime の Worker を停止・join してから Service、Window、WindowSystem を解放する。同じ構築 Thread から複数回呼べる

Host と Runtime はそれぞれ「未初期化」「実行中」「停止済み」の一方向とする。途中失敗では部分生成物を逆順で破棄して「停止済み」に移る。同じ Object の再初期化は行わず、新しい Object を構築して再試行する。

### 失敗の伝播

Window／Service／Runtime の初期化失敗、Message Pump 失敗、Callback 失敗は `Result` で呼出側へ返す。失敗した実行経路も `shutdown()` を呼び、停止・解放を完了する。Callback の最初の失敗と停止中の失敗が重なった場合は、最初の失敗を終了理由として保持し、Cleanup の失敗も診断可能にする。`shutdown()` は途中で失敗しても残りの解放を続ける。例外は Executable の境界で非 0 の終了 Code に変換する。

## 検証と後続判断

Window を生成しない Runtime の起動と Frame 進行、製品用 WindowsHost の正常 Close、Test 実行ファイルでの Frame 完了と単一 Thread fallback、無効な Callback、Window 作成失敗、Callback 失敗、二重停止を検証する。Test 用の自動終了と失敗注入は製品用 Main と Host 設定に含めない。Debug／Development／Release の Build と CTest は M03 Completion Gate で判定する。Native Window Handle の Renderer への引渡し、GPU 完了待ち、Resize と描画投入の停止点、Present と FPS 制御の関係は M04 で決める。
