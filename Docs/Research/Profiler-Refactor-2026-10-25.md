# Profiler への計測整理

対象 Branch: `m06/issue144/gpu-waits-and-telemetry`。この変更は develop へ Merge しない。

## 責務

- 各 Module は計測区間の開始・終了、Frame / 入力の対応付けを所有する。
- `Cue.Profiler` は標本保存、直近 120 標本の正確な平均・nearest-rank p95・最大、Snapshot と固定容量 Trace 領域を所有する。
- DX12 は GPU Timestamp Query、Fence 完了確認、GPU Tick から時間への変換を所有する。完了確認前の値を Profiler に追加しない。
- Editor は既存の集計取得 API を使って表示する。
- 調査 Tool は Windows の Clock / Thread Cycle 取得と、CSV / ETW 出力を所有する。

Profiler は Foundation と抽象 Platform の DTO に依存する。Win32 / DX12 / ImGui / Logger / FileSystem 実装には依存しない。

## 記録先と寿命

WindowsHost が一つの Profiler を所有し、Runtime / FrameController、Graph、ImGuiManager / DX12ImGuiBackend、入力遅延集計へ渡す。単体で生成する既存 API は省略可能な `Profiler*` を受け、未指定時は自分の記録先を所有する。

`ProfileMetric` は初期化時に登録する共有 Handle。各 Producer の記録点では `add(duration)` だけを呼び、登録配列の Lock、動的確保、ログ、File 出力を行わない。標本数や計測点の間引きは追加しない。

各系列は短い独立 Mutex で保護する。既存の Module Lock は制御状態と複数項目の整合性のため維持する。全体 Snapshot は Registry の参照を複写して Lock を解放してから各系列を読む。複数系列の厳密な同時点 Snapshot は保証しない。

Registry は Producer 破棄後も回収済みの最終値を保持する。Host の `shutdown()` 後も `profiler_snapshot()` を取得できる。借用 `Profiler*` は Producer の生成・再構築を終えるまで生存させる。Host の借用 API は Owner Thread 専用。

同名で稼働中の別 Producer は異なる ID を持つ。停止済み同名系列は再利用して ID と標本を初期化する。Resize の CPU Graph 集計は既存 Handle を継続し、GPU Pass は従来どおり再構築後の標本から集計する。

## 維持した項目

| 所有元 | Profiler 系列 |
|---|---|
| FrameController | `Frame.Main` / `Update` / `Render` / `FpsWait` / `Interval` / `CycleInterval` / `AdoptedToRenderStart` / `AdoptedToRenderComplete` / `PresentationWait` |
| WindowsHost | `Host.Present` / `GpuQueueWait` / `PresentationReady` / `PresentInterval` / `DisplayInterval` / `PresentApi` / `PresentationStatistics` / `AdmissionWait` / `MainGpuWait` / `MainPresentationWait` / `MainResourceWait` |
| ImGui | UI 構築、Snapshot Copy、Context Lock 待機、GPU Fence 待機 |
| Graph | `Graph.Record` / `Graph.FrameFenceWait`、完了済み各 Pass の `GPU.<Pass名>` |
| 入力遅延 | `Input.ToFrameCapture` / `ToRenderStart` / `ToPresentStart` / `ToPresentReturn` / `ToCpuComplete` |

入力イベント件数、GPU 未完了枠数、Present / Display の連番など、制御と対応付けのための値は既存 DTO に残す。既存の `frame_timing_info()` / `ui_timing_info()` / `graph_performance()` / `input_latency_info()` を維持する。

## 詳細 Trace と外部出力

`CUE_ENABLE_TIMING_TRACE` は既定 OFF。ON の調査 Build でだけ Sink の区間記録を有効にする。通常統計の記録とは独立する。

`TraceBuffer<T>` は起動前に容量を確保し、記録中は RAM のみ更新する。循環履歴、上書き・欠落件数を維持する。GPU の複数 Producer 排他と名前の複写は調査 Adapter が担当する。

`SpikeTimingProbe` の Recorder と `TraceOutputAdapter` を分離した。`SpikeTraceExport.h` は CPU / GPU の記録中には File を開かず保存を拒否し、停止後の CSV 保存を担当する。ETW の即時配送は明示設定した Adapter の機能であり、Profiler 内部からは呼ばない。既存の遅延 ETW 配送も維持する。

共通 GPU Trace は `cpuBefore` / `cpuAfter` と Clock ID / 周波数を持つ。Windows Tool の CSV は互換性のため `qpc_before` / `qpc_after` 列を維持する。Sink の登録解除は Worker 停止・GPU 回収後に行い、早期 return でも RAII で寿命を守る。

`TimingSamples.h` / `TimingTrace.h` は Foundation から Profiler へ移動した。WindowsHost の `InputLatency.h` は互換 Include として残す。

## 制限

- 正確な既存集計を維持するため、各系列の記録には Mutex と固定容量の更新処理がある。無負荷とは扱わない。
- 全体 Snapshot と系列登録は確保を伴う。Frame 内の記録経路から繰り返し呼ばない。表示の既存 Getter は確保不要の系列統計を利用する。
- 本変更は計測の整理であり、高速化を実測して保証する変更ではない。
- エンジンの通常 Logger、Shader / 設定 File の読込、ImGui 設定保存など、計測以外の I/O はこの変更の対象外。

検証結果は `out/validation/ProfilerRefactor-20261025/` の Build / CTest Log に保存する。

## 検証結果

- 既定 Debug の全 Target Build 成功。Trace OFF を確認。
- Debug CTest は 55 / 56 成功。`Cue.WindowsHost.Lifecycle` は既存の全体制限 10 秒で Timeout。単独 CTest も同じ制限で Timeout したが、同じ EXE の直接実行は 11.89 秒、終了 Code 0 で全 Case が完了した。Test 本体と制限は Before と同一で、過去にも同じ Timeout の記録がある。今回の所要時間の変動原因は未分離。制限値は変更していない。
- Trace ON の別 Build 成功。対象 CTest 6 / 6 成功（Profiler Core / Trace Export / TimingTrace / SpikeTrigger / DX12MainFrameGraph / EditorHost）。
- 既存 Getter と Profiler の平均・p95・最大・標本数・直近値の一致、Resize 後の CPU 系列一意性、停止後の Snapshot 保持を Editor 統合 Test で確認した。
- 実際の SpikeTimingProbe を UI あり / なしで各 1 秒記録した。CPU Trace は 6,188 / 7,665 件、GPU 記録は 505 / 1,275 件。いずれも欠落 0、記録中の File 保存試行 0、測定区間の Process Write I/O 0 byte / 0 operation。停止後に CSV を保存し、列、時刻順、GPU の 3 種類の記録を読み戻して確認した。
- Probe は前面固定条件を要求しない短い機能確認であり、FPS や処理時間の性能比較には使わない。
- Core 内に Windows / DX12 API、File 出力、ログ出力がないことを Source 検査した。他 OS での Build、ETW Collector と接続した配送、厳密な性能比較は未実施。

Git の Commit / Push / Merge は実施していない。
