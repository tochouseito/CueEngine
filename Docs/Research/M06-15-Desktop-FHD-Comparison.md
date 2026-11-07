# M06-15 / Issue #144 — FHD の性能比較

2026-10-25。この PC の現行作業 Copy で計測。**FHD は平均 1,310.252 FPS、同時再計測した 720p は 1,351.088 FPS。今回の差は -3.022%**。

前回の 2,073.259 FPS は別の Binary・表示条件である。今回は未 Merge の Profiler 整理を含むため、過去値との差を解像度または Profiler だけの影響とは扱わない。

## 条件

- Branch: `m06/issue144/gpu-waits-and-telemetry`。HEAD `ba1c485` に未 Commit の Profiler 整理と計測 Tool の変更を含む。
- Ryzen 7 3700X / RTX 3060 / Driver 32.0.16.1742 / 高パフォーマンス電源プラン。Hardware Adapter の Vendor 0x10DE、Device 0x2503、Software 0 を全試行で確認。
- Development x64。同一 Binary、詳細 CPU / GPU Trace ON。通常の Test UI と性能統計、ClearFinalColor + ImGui。
- Buffering 2、Worker ON、Update Lead 1、未完了 GPU 上限 2、SwapChain Buffer 2、Queued Presents 2。
- Waitable ON、VSync OFF、Tearing ON、FPS 上限なし。
- 両条件とも枠なし Window、Exclusive Fullscreen は使わない。同じ DISPLAY2、Pointer / Window 位置を試行内で固定。720p と FHD では寸法に合わせて配置を変える。
- 各回 Warmup 2 秒、測定 20 秒。順序は 720p → FHD → FHD → 720p → 720p → FHD。各解像度の合計は約 60 秒。
- Trace は起動前に確保した RAM へ保持。CSV と結果表示は測定・Worker 停止・GPU 回収後。ETW / PIX / Nsight Collector は起動しない。
- Binary SHA256: `992144D34C321A862B75B1F907C607AD160B24D12F2013B9DC914F5EC3921266`。全試行の開始・終了 Hash が一致。

## 結果

FPS は完了 Render Frame 数の合計 / 実経過秒の合計。瞬間 FPS の算術平均ではない。

| 解像度 | 試行 | Frame 数 | 実経過秒 | 合算平均 FPS | 1 Frame の経過時間相当 |
|---|---:|---:|---:|---:|---:|
| 1280 × 720 | 3 | 81,067 | 60.001277 | 1,351.088 | 0.740144 ms |
| 1920 × 1080 | 3 | 78,616 | 60.000654 | 1,310.252 | 0.763212 ms |

| 試行 | 解像度 | FPS |
|---|---|---:|
| Trial1-HD | 1280 × 720 | 1,328.976 |
| Trial2-FHD | 1920 × 1080 | 1,306.645 |
| Trial3-FHD | 1920 × 1080 | 1,308.693 |
| Trial4-HD | 1280 × 720 | 1,366.942 |
| Trial5-HD | 1280 × 720 | 1,357.346 |
| Trial6-FHD | 1920 × 1080 | 1,315.419 |

## 区間の平均時間

包含関係と Main / Render の並列動作があるため、以下の区間を単純に加算しない。GPU 時間は完了した Query から計算する。

| 区間 | 720p | FHD |
|---|---:|---:|
| CPU Frame.Main | 0.241506 ms | 0.249628 ms |
| CPU Frame.Render | 0.728585 ms | 0.751493 ms |
| CPU Graph.Record | 0.242240 ms | 0.253375 ms |
| CPU Queue.Submit | 0.087879 ms | 0.092513 ms |
| CPU DXGI.Present | 0.242340 ms | 0.310167 ms |
| CPU DXGI.Ready | 0.027517 ms | 0.012022 ms |
| CPU ImGui.Build | 0.197987 ms | 0.205562 ms |
| CPU ImGui.RecordWait | 0.165019 ms | 0.174078 ms |
| GPU ClearFinalColor | 0.003912 ms | 0.006223 ms |
| GPU ImGui | 0.057521 ms | 0.054540 ms |
| GPU Pass envelope | 0.067900 ms | 0.067053 ms |

今回の軽負荷では GPU Pass 全体が約 0.067 ms、CPU Render が約 0.75 ms。FHD 化に伴う GPU Clear の増加は見えるが、GPU 演算だけが全体時間を占める結果ではない。Present の平均は増え、Ready 待機は減っており、解像度の演算量だけから差を説明しない。

## サイズと品質の確認

- 初期確認では、装飾枠付き FHD Window の Client が 1920 × 1061 に制限された。その試行はサイズ不一致として不採用。
- 計測 Tool に Client 幅・高さの指定と実寸法の拒否チェックを追加した。明示的な解像度比較時だけ両条件を同じ枠なし形式にし、1920 × 1080 / 1280 × 720 の実 Client Size を確認。既存の引数では従来の枠付き形式を維持する。
- 正式 6 試行は全て Exit 0、前面 / 可視 / 非最小化 / Client Size の状態 11 を維持、状態不一致 0、状態変化 0。Pointer 変化 / Window 移動も 0。
- Snapshot 追加確保 0、CPU / GPU Trace の欠落・上書き 0。宣言件数と CSV 行数が一致し、CPU 連番と Render Frame の連続性を検証。GPU Status は全件成功。
- FHD の Trial3 / Trial6 は、最初の提出一件だけ BeforeSubmit の Clock 対応記録がない。計測開始が Clock 対応記録と ExecuteCommandLists の間に入った可能性のある開始境界として明示的に分離した。最初の一件を含む全提出について CPU / GPU Clock が完全照合できたとは扱わない。両 Pass の完了 Query はあり、後続提出は全て Clock 対応記録と照合できる。一般の途中欠落を許可する判定にはしていない。GPU Pass の時間差は Query 自体の Tick / Frequency から計算し、CPU Clock の対応は使わない。
- CPU FPS は全計測区間の完了数を使い、上記 GPU 境界の有無で標本を間引いていない。
- Engine 計測の File 保存試行は全試行 0。Process 全体の Write I/O は 720p で 0、FHD では各 20 秒に 11 operation / 1,613 byte。発生元は未特定で、Profiler / CSV 保存によるものとは断定しない。
- Computer Use による前面化は App 承認待ちで Timeout し操作していない。正式試行の前面維持は Probe の取得値で確認した。

## 変更と検証範囲

- 変更した Code は `Engine/Tests/Performance/SpikeTimingProbe.cpp` の解像度指定・計測用 Window 形式・実サイズ検証のみ。Engine 本体の Window / Runtime / Renderer は今回変更していない。
- 既定 Debug 全 Target と Development の計測 Target の Build 成功。FHD の短時間動作確認後に正式比較を実施。
- 重い Scene、実表示 Frame の周期、入力遅延、Present Mode の厳密な分類、旧 Binary の今回条件での再計測、Profiler 整理前後の性能差は未検証。枠なし FHD が画面全体を覆うため、720p と OS の合成経路が同じとは保証しない。
- Commit / Push / Merge、OS / Driver / モニター設定の変更は実施していない。計測アプリは全て終了済み。

## 証跡

Git 管理外の `out/validation/Performance20261025/FHD/` に保存。

- `RunComparison.ps1` / `AnalyzeTrials.py`: 起動条件、Hash、全 CPU / GPU CSV の品質検証と集計。
- `20261025-181918/`: 6 試行の生 CSV / Log、起動・終了 JSON と計測 Binary の控え。
- `HD-Analysis.json` / `FHD-Analysis.json`: 全期間の区間別集計、試行別品質チェック、GPU 境界の識別。
- `Environment.json` / `BuildManifest.json`: Hardware / 電源 / Build 条件と Source・Binary Hash。
- `Before/SpikeTimingProbe.cpp`: 今回の変更前の Probe。Profiler 整理後の Source を保全。
- `Debug-Build.log` / `Development-Build.log`: Build 結果。
- `Smoke-FHD*.log` / CSV: 初期サイズ制限の確認と短時間動作確認。正式合算には使わない。
