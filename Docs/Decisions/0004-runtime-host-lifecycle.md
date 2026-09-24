# ADR-0004: RuntimeHost の構成とライフサイクル

- Status: Accepted（M03-01 の契約。実装は M03-02～03）
- Date: 2026-09-25
- Issue: [M03-01](https://github.com/tochouseito/CueEngine/issues/15)
- 関連: [ADR-0001](0001-architecture-boundaries.md)、[ADR-0003](0003-frame-thread-contract.md)

## 背景

M02 の `CueWindowHost` は Window と `FrameController` の接続を 16ms のダミー処理で検証する実行 Target である。製品用 Host は、同じ Frame 契約を利用しつつ、Update／Render 処理を外から登録できる必要がある。M04 の Renderer 接続前に、Process の起動、Frame 進行、終了の所有者を固定する。

## 決定

`CueRuntimeHost` を製品用 Executable とし、その内部の `RuntimeHost` を Composition Root とする。`CueWindowHost` は M02 のダミー検証用に残す。`RuntimeHost` は Windows の `WindowSystem`／`Window`、時計・待機・Thread Service、`FrameController` を一意所有する。Window の Message Pump と生成・破棄は構築 Thread（MainThread）だけで行う。Update／Render Callback は `FrameController` の契約に従う Worker Thread、または単一 Thread fallback では MainThread から実行され、Window を直接操作しない。

M03 の Host は Runtime World、Scene、Project、Renderer、GPU Resource を所有しない。M04 では Host が Renderer／Presentation の実装を選び、Render Callback へ接続する。GPU Resource、Descriptor、Fence、Frame Resource の所有者は Renderer／Presentation 側とし、Host はそれらを Frame の間に直接操作しない。

### API と状態

- `RuntimeHostDesc` は Window 設定、`FrameControllerDesc`、Test 用の自動終了 Frame 数を持つ。Test 指定なしでは Close 要求まで動作する
- `initialize(update, render)` は構築 Threadで一度だけ呼ぶ。Callback は `FrameController` 開始前に登録する。Callback が捕捉した参照の Owner は `shutdown()` 完了まで生存させる
- `pump_events()` は MainThread で Window Event を処理し、Close または Test 終了条件なら `false` を返す。その周回で `step()` を呼ばない
- 初期化成功後、Main のループは `pump_events()` の判定と `frame_controller().step()` の呼出だけを行う
- `shutdown()` は Worker の停止・join を完了してから `FrameController` と借用先 Service、Window、WindowSystem を破棄する。同じ構築 Threadから複数回呼べる
- 公開 Header は Win32 型を出さない。Windows の具体 Factory は Host 実装に閉じ込める

状態は「未初期化」「実行中」「停止済み」の一方向とする。`initialize()` の途中失敗では部分生成物を非公開のまま逆順で破棄して「停止済み」に移る。同じ Host の再初期化は行わず、新しい Host を構築して再試行する。`pump_events()` と `frame_controller()` は初期化成功後から `shutdown()` 前まで有効とする。

### 失敗の伝播

Window／Service／Controller の初期化失敗、Message Pump 失敗、Callback 失敗は `Result` で呼出側へ返す。失敗した実行経路も `shutdown()` を呼び、停止・解放を完了する。Callback の最初の失敗と停止中の失敗が重なった場合は、最初の失敗を終了理由として保持し、Cleanup の失敗も診断可能にする。`shutdown()` は途中で失敗しても残りの解放を続ける。例外は Executable の境界で非 0 の終了 Code に変換する。

## 検証と後続判断

M03-02～03 で、正常 Close、自動終了、単一 Thread fallback、無効な Callback、Window 作成失敗、Callback 失敗、二重停止を検証する。Debug／Development／Release の Build と CTest は M03 Completion Gate で判定する。Native Window Handle の Renderer への引渡し、GPU 完了待ち、Resize と描画投入の停止点、Present と FPS 制御の関係は M04 で決める。
