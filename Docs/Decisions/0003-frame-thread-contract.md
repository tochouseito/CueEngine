# ADR-0003: Frame制御の時間・役割Thread契約

- Status: Accepted（M02-01の契約。実装とHost接続は後続Issue）
- Date: 2026-09-24
- Issue: [M02-01](https://github.com/tochouseito/CueEngine/issues/11)
- 関連: [ADR-0001](0001-architecture-boundaries.md)

## 決定

Windowの生成・Message Pump・破棄を行うHost ThreadをMainThreadと呼ぶ。MainThreadを別のWorkerとして生成しない。UpdateThreadとRenderThreadは処理の役割であり、実RuntimeやRendererへのThread所有権はこの段階では定義しない。

FrameControllerはPlatform公開契約の`Clock`、`Waiter`、`ThreadFactory`を借用し、OS固有型を公開しない。HostはControllerとWorkerの停止・join後にこれらを破棄する。`Clock`は単調時刻を返す。`Waiter`は通知世代によって待機開始直前の通知を取りこぼさず、停止要求で待機を解除する。`Thread`は協調停止、join、Routine結果の伝播を担当する。

最初のFrame進行は有界な先行数を持つ単純な順序制御とし、Updateが完了したFrameだけをRenderへ渡す。単一Thread fallbackを維持する。Mailbox、Backpressure、VSync、実GPU Submit、Render Snapshot形式、Script Thread契約はこのIssueで固定しない。

## 採否

旧`FrameController`のMain／Update／Renderの役割と同期点は参考にする。旧実装のBuffer数に結び付いた状態機械やThread実装は直接移植しない。新RepositoryにはRendererとSwap Chainがなく、Buffer数をFrame先行上限の根拠にできないためである。

## 検証と残る判断

公開HeaderはWindows SDKなしでCompileする。Windowsの実装、通知・停止の実動作、Frame順序、Hostでの16msダミー処理はM02-02〜04で検証する。実Runtime／Renderer接続時はFrame所有のRender Data、GPU Resourceの所有Thread、Fence、Resize時の停止点を別途決定する。
