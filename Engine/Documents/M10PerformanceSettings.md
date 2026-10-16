# M10 Performance Settings Corrections

Milestone: [M10](https://github.com/tochouseito/CueEngine/milestone/11)

| Issue | 修正対象 |
| --- | --- |
| [#132](https://github.com/tochouseito/CueEngine/issues/132) | Worker OFF の FPS 待機方式と Spin |
| [#133](https://github.com/tochouseito/CueEngine/issues/133) | 単一 Thread の GPU 先行上限と物理枠再利用 |
| [#134](https://github.com/tochouseito/CueEngine/issues/134) | Waitable OFF の表示 Queue 未適用 |
| [#135](https://github.com/tochouseito/CueEngine/issues/135) | Worker 切替での Update 先行数保持 |
| [#136](https://github.com/tochouseito/CueEngine/issues/136) | 条件付き設定と要求 / 実効値の UI |

## #132 FPS 待機

周期の期限と CPU 完了 / 周期完了の分離は M09 を維持する。Sleep / Hybrid / Spin の実効計算を共通化し、単一 Thread でも Waiter の高精度 Timer を使う。Main は一回最大 1 ms の Sleep または Spin 区間で Message Pump へ戻る。表示準備の再確認にも粗い Condition Variable の Timeout を使わない。Worker は同じ方針で期限まで停止可能に待つ

実効 Spin は FPS 上限 0 または Sleep の場合は 0、それ以外は要求値と周期の 1/8 の小さい方。実待機時間は OS Scheduler と Timer の対応状況に依存する

RecordingWaiter による決定的な Test で、単一 Thread の Sleep / Hybrid 選択、期限前の投入拒否、1 ms 以下の待機要求、Spin 上限を検証する。FrameControllerTimingProbe の `--fps-only` は GPU / Window を含めず Worker ON/OFF を同じ 60 FPS 設定で比較する。実入力から画面までの遅延や全環境の 60 FPS 達成を保証する測定ではない

2026-10-16、Debug / x64、空 Callback / 60 FPS / 120 Frame を 2 回測定した。単一 Thread は変更前 32.54〜32.55 FPS、変更後 60.44〜60.46 FPS。変更後の Worker は 60.40〜60.48 FPS。最初の Frame が即時なので壁時計平均は 60 を少し超える。単一 Thread の直近間隔は 16.669 / 16.699 ms。Debug Build と Runtime / WindowsHost / EditorHost の関連 CTest 12 件が成功した
