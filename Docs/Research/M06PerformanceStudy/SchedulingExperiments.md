# Issue #101: 同期・Present・Command 再利用の追加検証

## 結果と作業境界

2026-10-08、`m06/issue101/render-performance-study` で実測した
研究変更は未マージ。採用する変更は別 Issue と `develop` からの Branch で実装する

- 次 UI を前 Frame の ImGui 提出追跡・Snapshot 回収後に構築すると、Render 平均は **0.5889 → 0.5258 ms**、p95 は **0.8315 → 0.6722 ms** に下がった
- Main 平均は **0.1813 → 0.4493 ms** に増えた。待ち時間の移動を含み、Engine 全体の CPU 作業削減とは判断しない
- 一枠化と Worker 無効化でも Render は下がったが、将来の Update / Render 並列性を減らす比較条件である
- Command 二枠化、SwapChain 三枠化、Tearing、VSync は一貫した改善を示さなかった
- Present の大部分は Native 呼出区間だった。Wrapper の整理で大幅に短くなる測定結果ではない
- 正式計測は **33 Process / 19,800 Render Sample**、全実行正常終了、記録欠落ゼロ

## 条件と方法

| 項目 | 条件 |
| --- | --- |
| OS / CPU | Windows 11 Home 10.0.26200 / Ryzen 7 3700X、8 Core / 16 Logical Processor |
| GPU | RTX 3060、Driver 32.0.16.1047 |
| 実 Device | Vendor 4318 / Device 9475、Dedicated Memory 12,703,498,240 bytes |
| Build | MSVC 19.51 / x64 / Development / O2、D3D12 Debug Layer・GBV 無効 |
| UI | 1280×720 Window、既定 Test Window と詳細統計表示、設定 File 読込・保存なし |
| 基準 | Worker 有効、FrameController 二枠、SwapChain 二枠、60 FPS、VSync / Tearing 無効 |
| 統計表示 | 全条件で 15 Frame ごとの取得。毎 Frame の生 Sample 収集は継続 |
| GPU Timestamp / PIX Marker | 全条件で有効。Capture を実施した意味ではない |
| 観測 | 各 Process 120 Frame Warmup、600 Frame 測定、各提案三反復 |
| 順序 | 基準を各反復の前後に配置。反復 1 / 3 は下表順、反復 2 は逆順 |

条件比較は 30 Process、CPU Cycle 計測は別の 3 Process
両 Phase の実行 File SHA-256: `125D91D0FFFC8BA8F3F19BBA384F4F11A14822C5F034B9FB1ED65FAA522B3873`

実装・Build・Test は正式計測前に完了し、実行中に変更しなかった
全 Worker Join 後に CSV / JSON を書き、全 Process 停止後に集計した
基準は 3,600 Sample、各提案は 1,800 Sample。p95 は生 Sample の nearest-rank
以前の実験と実行 File が異なるため、以前の数値と混ぜて改善率を計算しない

電源状態、Clock、背景 Application、Window の重なりは固定していない
基準の各実行の平均は 0.5761～0.6135 ms と揺れがある
最終 UI の Vertex 数は 2,328～2,432、Index 数は 3,528～3,684。統計文字列の長さによる差があり、完全に同じ Geometry ではない
Update 平均は約 0.0002 ms。重い Gameplay Update は検証していない

## Render 比較

単位は ms。基準との差は pooled 平均の比較

| 条件 | avg | p95 | max | 平均差 | 判断 |
| --- | ---: | ---: | ---: | ---: | --- |
| `next-baseline` | 0.5889 | 0.8315 | 2.2839 | — | 統計表示間引きのみの基準 |
| `next-ui-gate` | 0.5258 | 0.6722 | 1.2542 | −10.7% | 三反復とも avg / p95 が低下。Main 待機増加を伴う |
| `next-frame1` | 0.5053 | 0.6584 | 1.2014 | −14.2% | CPU 先行を減らす診断条件 |
| `next-main` | 0.4879 | 0.6214 | 1.0891 | −17.1% | Worker 無効の診断条件 |
| `next-buffer3` | 0.5969 | 0.8356 | 1.6397 | +1.4% | 改善方向が反復で揃わない |
| `next-tearing` | 0.5899 | 0.8354 | 1.6212 | +0.2% | 実 Tearing 有効。改善方向が揃わない |
| `next-command2` | 0.5915 | 0.8268 | 1.4014 | +0.4% | Reset / Execute の平均を改善しなかった |
| `next-combined` | 0.5331 | 0.6870 | 1.4289 | −9.5% | UI Gate + 三 Buffer + Command 二枠。Gate 単独への上積みなし |
| `next-vsync` | 0.5949 | 0.8172 | 1.8920 | +1.0% | 表示条件の診断用 |

各反復の前後の基準 1,200 Sample と比較した Gate の平均差は −7.94%、−9.61%、−14.62%
一枠化・Worker 無効化も三反復とも平均 / p95 が低下した
Command 二枠化の平均差は −0.19%、+1.17%、+0.31%
全比較 Process は 720 Render Frame を 11.9895～11.9951 秒で終了した
60 FPS 上限を維持しており、短縮を FPS 向上の証拠として扱わない

## ImGui 同期

Main の統計取得 / UI 構築前に Gate で待つ
Render の `finish_submission()`、Snapshot 回収、`consumedFrames` 更新の後で解除する
GPU 完了は待たない。Present / FPS 待機はその後に進み、次 UI 構築と重ねられる
Context Lock は待機中に解放し、Main Stop Token で取消可能にした

| 区間 | 基準 avg ms | Gate avg ms |
| --- | ---: | ---: |
| Render | 0.5889 | 0.5258 |
| Main | 0.1813 | 0.4493 |
| Main Gate 待機 | 対象外 | 0.2820 |
| UI 構築 | 0.1606 | 0.1514 |
| ImGui 記録時の Context 待機 | 0.0501 | 0.00016 |
| Present | 0.2294 | 0.2345 |

Render の Context 待機はほぼ消えたが、Main の待機が増えた
並列区間を含む Main と Render の平均を単純合計して Frame 時間にはしない
Gate は次 Frame の Update 開始も遅らせ得る。Update / Render がそれぞれ長く動く将来要件にはそのまま採用しない
入力応答、Gameplay の遅延、重い Update の並列性は未検証

次の設計検討候補は、ImGui の描画記録が共有 Context を借用する範囲を減らし、所有 Snapshot だけで記録できる境界を設けること
公式 Backend が Context にアクセスする現在の処理から単に Mutex を外す変更は行っていない

## Command 再利用と Native API

研究条件だけ Graphics Command Context を二枠事前確保して Close し、貸出順を巡回させた
Fence 完了確認、借用判定、Reset 失敗の回復、提出状態不明の停止は維持した
両枠が使用中の場合は既存の増設 / 上限時待機を維持する

実貸出回数は各反復、基準が Slot 0 に 721 回、二枠条件が Slot 0 / 1 に 361 / 360 回
Warmup と停止直前の追加 Frame を含む。三つ目以降の Slot は使われなかった

| 基準の区間 | avg ms | 内訳 |
| --- | ---: | --- |
| Command acquire | 0.0485 | Allocator Reset 0.0083、List Reset 0.0355 |
| Command submit | 0.0978 | ExecuteCommandLists 0.0865、Signal 0.0082 |
| Present 全体 | 0.2294 | Native Present 0.2288 |

二枠でも Allocator Reset 0.0085、List Reset 0.0357、ExecuteCommandLists 0.0866 ms と平均はほぼ同じ
Native Present と上記四つの Native Command API の平均合計は約 0.3674 ms。これらの区間は重複しない
現在の条件では Render 平均の約 62% を占める。Wrapper の整理だけで Render 全体を 0.1 ms 以下にする根拠は得られなかった
観測平均であり、あらゆる Hardware / 表示条件での下限を示すものではない

## 別 Phase の CPU Cycle 計測

Windows 専用 Probe が `QueryThreadCycleTime()` を提供し、同じ Render Thread の API 前後を測った
API 復帰直後に終了 Cycle を採り、Collector 保存は区間外に置いた
Cycle は User / Kernel の処理を含むが、経過時間に変換する用途ではない
取得 API 自身の費用が含まれ、Driver 内部関数や GPU 待機理由までは分解できない
仕様: [Microsoft QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)

| Native 区間 | avg elapsed ms | avg cycles | p95 cycles |
| --- | ---: | ---: | ---: |
| 空の Cycle 読取・保存 | 0.00214 | 1,151 | 1,438 |
| Allocator Reset | 0.00930 | 31,288 | 39,523 |
| List Reset | 0.03736 | 131,254 | 163,481 |
| Close | 0.01808 | 59,569 | 82,639 |
| ExecuteCommandLists | 0.08784 | 311,076 | 404,572 |
| Signal | 0.00915 | 30,836 | 40,242 |
| Present | 0.23227 | 815,727 | 1,024,724 |

Close の elapsed は上位の Close 処理、Cycle は Native Close であり、完全に同じ境界ではない
空区間の elapsed は読取と Collector 保存も含み、一律に差し引ける補正値ではない
この Phase の Render 平均は 0.5978 ms。Cycle Hook のない比較へ Sample を混ぜていない
全七区間で各 1,800 Cycle Sample、取得失敗・保存欠落ゼロ

長い elapsed が必ず同じ倍率の Cycle 増加を伴うわけではなかった
例: Signal 最大 elapsed 0.2194 ms の Sample は 53,896 cycles。最大 Cycle は別 Sample の 419,303 cycles
Present には多い Cycle が記録されたが、GPU 待機・OS Scheduling・Driver 処理の割合はこれだけでは決められない
さらに分類するには CPU Stack Sampling と OS / GPU Timeline が必要

## 検証と限界

- Research Development Build 成功
- `pwsh -NoProfile -File scripts/codex_build.ps1` の Debug Build 成功
- 関連 10 CTest 成功、15.31 秒。ImGui Gate が通知なしでも Stop Token で解除される確認を追加
- 十条件の短時間起動確認成功。Cycle 境界を修正して再 Build 後、正式計測を行った
- 正式 33 Process の status / 記録欠落 / Frame 範囲 / Cycle 数の検証成功
- read-only Review で指摘された Cycle 区間への Collector 費用混入を正式計測前に修正
- PIX / Nsight / cue_engine MCP は Session に公開されておらず、Capture、CPU Stack Sampling、GPU Timeline 検証は未実施
- 既存 WARP / ImGui Backend Test は成功したが、全実験条件の Debug Layer / GBV 有効での再測定は未実施
- Gameplay 負荷、入力遅延、Resize / Minimize 反復、別 Hardware での再現は未検証

## Artifact と再現

- [条件別全区間](SchedulingPooledMetrics.csv)、[実行別](SchedulingRunMetrics.csv)、[反復別比較](SchedulingPairedMetrics.csv)
- [検証結果](SchedulingValidation.json)、[比較環境](SchedulingEnvironment.json)
- [Cycle Phase elapsed](NativeCyclePooledMetrics.csv)、[Cycle 集計](NativeCycleMetrics.csv)、[Cycle 検証](NativeCycleValidation.json)
- 生 CSV / 条件 JSON: `out/validation/M06-PerformanceStudy/SchedulingRuns/` と `NativeCycleRuns/`
- Log: 同 Directory 親の `next-*.log`、`scheduling-runs.log`、`native-cycle-runs.log`

```powershell
. ./scripts/vcpkg_environment.ps1
$env:VCPKG_ROOT = get_vcpkg_root
cmake --preset windows-vs2026-performance
cmake --build --preset windows-vs2026-performance-development
& ./Engine/Tests/Performance/RunEditorTimingStudy.ps1 -BaselineCase next-baseline -Cases @(
    'next-ui-gate','next-frame1','next-main','next-buffer3',
    'next-tearing','next-command2','next-combined','next-vsync'
) -OutputDirectory out/validation/M06-PerformanceStudy/RepeatSchedulingRuns
python Engine/Tests/Performance/SummarizeEditorTimingStudy.py out/validation/M06-PerformanceStudy/RepeatSchedulingRuns --baseline next-baseline
& ./Engine/Tests/Performance/RunEditorTimingStudy.ps1 -BaselineCase next-cycles -CommandBreakdown -OutputDirectory out/validation/M06-PerformanceStudy/RepeatCycleRuns
python Engine/Tests/Performance/SummarizeEditorTimingStudy.py out/validation/M06-PerformanceStudy/RepeatCycleRuns --baseline next-cycles
```
