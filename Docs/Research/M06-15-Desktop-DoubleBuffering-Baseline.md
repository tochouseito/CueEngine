# M06-15 / Issue #144 — デスクトップ PC の Double Buffering 基準測定

- 測定日: 2026-10-24（日本時間）
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- Source Commit: `ba1c485d895683cc50e7db390b3ac27a8f2a01a7`
- 表示経路の確認: [Desktop-Presentation-Path](M06-15-Desktop-Presentation-Path.md)
- Engine Source の変更、Commit / Push / Merge は行っていない。

## 1. 結果

**正式な 3 試行の合算は 124,400 Frame / 60.002164 秒 = 2,073.259 FPS。**
同じ Binary と同じ設定の通常 UI / ClearFinalColor で、今回の合算平均は目標の 2,000 FPS を超えた。
各試行の変動と約 6.36 ms の最大 Frame 間隔は残っており、常時 2,000 FPS の保証ではない。

| 正式試行 | 保存名 | Frame 数 | 秒 | FPS | Frame 間隔 平均 / p95 / Max ms |
|---|---|---:|---:|---:|---|
| 1 | `20261024-181714/Trial2` | 43,686 | 20.000032 | 2,184.297 | 0.457803 / 0.8461 / 2.7119 |
| 2 | `20261024-181714/Trial3` | 40,895 | 20.001758 | 2,044.570 | 0.489104 / 0.9546 / 6.3570 |
| 3 | `20261024-181855/Trial1` | 39,819 | 20.000374 | 1,990.913 | 0.502285 / 0.8370 / 4.6101 |

FPS は完了 Frame 数 / 実経過秒で求め、瞬間 FPS の算術平均にはしていない。
Frame 間隔は連続した CPU Render 完了点の差。3 試行の標本をまとめると平均 0.482331 ms、
nearest-rank p95 0.8815 ms、最大 6.357 ms、124,394 標本。
別試行の終了から次試行の開始までの時間は Frame 間隔へ含めていない。

## 2. 条件

| 項目 | 値 |
|---|---|
| CPU / Memory | AMD Ryzen 7 3700X / 搭載 Memory 約 32 GB |
| GPU | NVIDIA GeForce RTX 3060、Vendor / Device `10DE / 2503`、Software 0 |
| Device LUID | 全試行 `0x00000000000117F2`、現在の有効表示経路と一致 |
| DriverVersion（Windows 表記） | `32.0.16.1742` |
| 電源プラン | 高パフォーマンス。測定のための変更はしていない |
| Build | Development x64、`CUE_ENABLE_TIMING_TRACE=ON` で Build した同一 Binary |
| Binary SHA256 | `37C7DAC0B4B887E175008623528A8E060B8E4AA99A191E4B4F08BDE745CFA7F9` |
| UI / Scene | 通常の Test UI と性能統計、ClearFinalColor + ImGui。UI の省略や更新間引きなし |
| Window | Client 1280 × 720、前面・可視・非最小化 |
| Buffering | 2、CPU / GPU 枠 2、Worker ON、Update lead 1 |
| 未完了 GPU 上限 / SwapChain / Queued presents | 2 / 2 / 2 |
| 表示 | Waitable ON、VSync OFF、Tearing ON、FPS 無制限 |
| 時間 | 各回 Warmup 2 秒 + 測定 20 秒 |
| 記録 | CPU / GPU 全記録を CSV へ保存。ETW、PIX、Nsight の Collector は起動しない |

測定後の有効モニターは BenQ GL2480（1920 × 1080 / 74.973 Hz）と ASUS VP228
（1920 × 1080 / 60 Hz）の 2 台で、どちらも RTX 3060 の Source / Target に一致した。
前回の GPU 確認では ASUS の 1 経路だったため、その古いモニター構成を今回の測定条件と同一とは扱わない。
今回の Probe は各試行中の前面・可視・最小化・Client Size を全 Step で検査する一方、
各 Frame のモニター名や表示構成の変更履歴は記録していない。

## 3. 合算した処理区間

CPU の包含区間には他の処理や待機を含む。以下の値は単純に足し合わせない。
GPU 時間は同じ Queue の生 Timestamp 差と Frequency から求め、CPU 時間と混同しない。

| 区間 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|
| CPU Main | 0.131401 | 0.2120 | 1.1497 |
| CPU Render（待機を含む） | 0.474885 | 0.8702 | 6.3501 |
| Graph.Record | 0.123997 | 0.2218 | 1.1797 |
| Queue.Submit | 0.064824 | 0.1281 | 0.5141 |
| Present API | 0.162789 | 0.2468 | 0.8509 |
| DXGI.Ready | 0.063322 | 0.0948 | 5.6707 |
| UI Callback | 0.057315 | 0.0848 | 0.5222 |
| GPU ClearFinalColor | 0.004640 | 0.006144 | 0.008192 |
| GPU ImGui | 0.050890 | 0.058368 | 0.068608 |
| GPU 最初の Pass 開始～最後の Pass 終了 | 0.061618 | 0.071680 | 0.081920 |

GPU Pass 全体は約 0.062 ms だが、CPU Render は約 0.475 ms。GPU Pass の演算時間だけでは
CPU 側の Present / 記録 / 待機を説明できない。次の改善対象を選ぶ際は、包含区間の重複や Frame 対応を
確認してから切り分ける。今回、最大スパイクの根本原因の追加調査は行っていない。

## 4. 品質確認と不採用の記録

正式な 3 試行は、以下を生 CSV と Log で確認した。

- Exit 0、開始時と終了時の Binary Hash 一致。3 試行間も同じ Hash。
- 前面 / 可視 / 非最小化 / Size の状態 11 を維持、不一致 0、状態変化 0。
- 計測区間内の Snapshot 追加確保 0。
- CPU Trace の欠落 0、各 Thread の連番は 0 から連続し、宣言件数と CSV 行数が一致。
- GPU Record の欠落 / 上書き / エラー Status 0、宣言件数と CSV 行数が一致。
- 計測された各提出について BeforeSubmit と提出が各 1 件、ClearFinalColor / ImGui の完了 Query が各 1 件。

CPU 行数は 2,137,941 / 2,002,352 / 1,948,828、GPU 行数は 176,358 / 165,208 / 160,939。
各試行の CPU Render Trace は完了 Frame 数より 1 件少なく、開始境界をまたいだ区間が記録対象から外れる。
GPU は非同期で、各試行で Warmup 由来の Pass 1 Frame を別扱いにし、GPU 統計には計測中の提出に
対応する 43,687 / 40,896 / 39,820 Frame を使った。終了付近の提出と CPU 完了の件数は同一とは限らない。

最初の `20261024-181714/Trial1` は 1,916.533 FPS だったが、Window 不一致 38,230 回、
状態変化 1 回、最終状態 10（前面でない）、Snapshot 追加確保 2 件だったため合算から除外した。
失敗試行も生 CSV / Log とともに残し、3 回目を追加して正式 3 試行を揃えた。

## 5. 検証範囲と保存先

- Development の `CueSpikeTimingProbe` をこの PC で Build 成功。
- Engine Source は変更していない。Debug Build / 全 CTest は今回実行していない。
- Trace の Configure Cache は測定後に OFF へ戻した。保存済み計測 Binary は Trace ON のまま。
- 計測アプリは終了済み。OS / GPU / Driver / 表示設定は変更していない。
- 物理表示 FPS、入力遅延、重いゲーム Scene、Release / Debug 構成、長時間の安定性は未検証。
- 前のノート PC とは CPU、GPU、表示構成、Build Binary が異なるため、差をコードの改善率としない。

生記録は Git 管理外の `out/validation/Performance20261024/Desktop144/` に保存した。

- `RunTrials.ps1`: 同一条件の起動、引数 / PID / Hash / 終了結果の記録。
- `AnalyzeTrials.py`: CPU 連番、GPU 完了 Query、品質条件の検証と合算統計。
- `TrialAnalysis.json`: 全試行と正式 3 試行の合算結果。
- `20261024-181714/`、`20261024-181855/`: 生 CPU / GPU CSV、Log、起動 / 終了情報、Binary の控え。
- `DisplayPathsAfter.json`、`Environment.json`: 測定後の表示構成と Hardware / OS / 電源プラン。

別 PC で Clone しても、Git 管理外の証跡は存在しない。再計測時は同じ条件を明示し、前面が外れた試行を
正式結果に混ぜない。
