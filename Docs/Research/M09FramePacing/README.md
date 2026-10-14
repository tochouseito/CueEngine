# M09 周期・表示同期の実測

## 条件

2026-10-15 JST、Windows 11 Home 10.0.26200、NVIDIA GeForce RTX 3060、Driver 32.0.16.1742。Development / x64 の CueM09FramePacingProbe、Client 320 x 240、Test / TEST UI と FinalColor Clear、Worker 有効、CPU 2 / 先行 1 / GPU 2 / GPU 上限 2 / SwapChain 2 / Waitable 有効 / 表示 Queue 上限 1 / 60 FPS / Hybrid / Spin 500 us / Prediction Slack 1 ms / Tearing 無効

各条件 180 Frame、先頭 60 Frame を Warmup、最新 120 標本を比較。各条件 1 Run であり、旧実装より高速になったことの検証ではない。全行は [DevelopmentTiming.csv](DevelopmentTiming.csv) に保存する

## 結果

| VSync | 要求 / 実効 Mode | 周期 avg / p95 ms | Present avg ms | Present 戻り間隔 avg / p95 ms |
| --- | --- | --- | --- | --- |
| OFF | FixedPeriod | 16.671 / 16.685 | 0.321 | 16.672 / 17.240 |
| OFF | PresentationReady | 16.677 / 16.712 | 0.330 | 16.680 / 17.382 |
| OFF | DisplayPrediction / FixedPeriod | 16.674 / 16.703 | 0.350 | 16.672 / 17.239 |
| ON | FixedPeriod | 16.671 / 16.680 | 0.322 | 16.673 / 17.128 |
| ON | PresentationReady | 16.674 / 16.694 | 0.343 | 16.672 / 17.323 |
| ON | DisplayPrediction | 16.680 / 16.710 | 0.354 | 16.680 / 17.438 |

DXGI Refresh 周期の標本は約 13.338 ms（約 75 Hz）。60 FPS 上限が次周期の下限になるため、この条件の Prediction は FixedPeriod より早く開始しない。GPU 未完了数の観測最大は 1〜2 で指定上限 2 以下。VSync OFF の Prediction は実効 FixedPeriod / DisplayTimingUnavailable と公開された

入力代理時刻から CPU Render 完了は平均 17.81〜18.00 ms。Main の Frame 採用時刻が基準であり、入力機器の操作から画面までの遅延ではない。表示間隔は DXGI 標本から導出した値で、物理表示の撮影測定ではない

## 未検証と制約

- VRR、複数 Monitor、他の Fullscreen App、実ゲームの更新 / GPU 高負荷
- 実入力イベントから光子までの遅延、Tearing や動きの見た目
- DisplayPrediction は Refresh 位相から次周期の候補を得る実装。次 Frame の CPU / GPU 作業量を予測して Present を保証する機能ではない
- DXGI 統計が取得できない / 古い / 不連続な環境は FixedPeriod に戻る

単体 Test では予測なし / 古い候補 / 過大候補の Fallback、表示通知が期限から 10 ms 遅れた単一 Thread の実待機計測を検証した。Native Test では Waitable の初回通知、取消、取得済み通知の保持、Resize、Close と設定変更を検証した
