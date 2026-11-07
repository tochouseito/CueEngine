# M06-15 Frame Spike の ETW 追跡

> 続報: [Scheduling / GPU Fence / DWM の追加調査](M06-15-Frame-Spike-Scheduling-And-Presentation.md) で優先度切替、GPU Signal Packet と CPU 待機の対応、DWM からの起床を確認した。以下は Run1 / Run2 時点の記録。

- Date: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 先行調査: [Frame Spike 調査](M06-15-Frame-Spike-Investigation.md)
- 状態: UAC 許可後に実機 ETW を 2 回取得。変更は未 Commit / 未 Push / 未 Merge

## 結論

長い Frame には、少なくとも次の異なる要因がある。

1. **Waiting が大きい Frame**: Fence / DXGI Event の待機解除前の時間が大半を占める。復帰可能になった後の CPU 割り当て待ちは小さい例がある。
2. **Ready が大きい Frame**: 実行可能な Render Thread が中断・再開を繰り返し、CPU 割り当て待ちが合計約 7.98 ms になった例がある。Fence 待機だけでなく、Graph 記録・提出と Present の途中にも発生した。

したがって、Worker の On / Off、GPU Pass の処理量、Waitable の一項目だけを全 Spike の原因とは断定できない。今回は観測と切り分けを追加し、Thread 優先度、Affinity、電源設定、Buffering の規則は変更していない。

GPU Provider も有効にして記録したが、確認した Spike の時間窓では Queue / DMA の提出から完了までを対応付けられる Event を得られていない。**Waiting の全時間を GPU の演算時間や特定 Driver の不具合とは扱わない。**

## 追加した計測機能

- 手動 `CueSpikeTimingProbe` の任意指定で `CueEngine.SpikeTiming` TraceLogging Provider を登録する。
- 完了した全区間の Frame ID、Native Thread ID、Thread ごとの連番、開始・終了時刻、補助値、Native Wait の戻り値を ETW に出力する。間引かない。
- 開始・終了で `steady_clock` の読取りを QPC で挟み、Clock 対応と取得幅を記録する。
- `CueSpikeEtwExport` は元 ETL を停止後に読み、Raw QPC の CSwitch / ReadyThread と独自 Marker を CSV に出力する。Decode 不足、File 書込み失敗、ETW 欠落は失敗として報告する。
- `analyze_spike_etw.py` は独自 Marker と元の全区間 CSV を Thread 連番ごとに照合する。名前、Frame、開始・終了、補助値、戻り値、件数の不一致を検出する。
- 同じ Thread の Scheduling 区間を Frame / 子区間の境界で切り詰め、Running / Waiting / Ready / Unknown に積算する。
- 元の平均 / p95 / Max / 件数と直近 120 標本は維持する。ETW は調査用 Probe に限定し、通常実行の出力には追加しない。

区間の ETW Header は出力時刻なので、開始・終了の比較には Payload の時刻を使用する。ETW 出力自体の負荷も測定環境に含まれる。

### Scheduling 分類の限界

- Running はその Thread が CPU に選択されている期間で、同じ CPU 上の ISR / DPC を含み得る。関数が演算した時間そのものではない。
- Waiting から Running へ移る途中に ReadyThread が観測できない場合、その待機区間は Unknown として残す。
- 未知の Thread State は Unknown とし、0 ms や Ready に置き換えない。
- 今回の Windows 26200 は CSwitch Version 5 / 28 bytes を出力し、TDH Metadata の取得に失敗する。その組合せだけ、Microsoft PerfView の CSwitch Prefix と一致する先頭 Field を読む。元の State 値と Version の件数を保持する。
- Run2 の全 854,211 CSwitch について、同じ CPU の前回 New TID と次回 Old TID の矛盾は 0 件。xperf 自身の Decode とも選択区間の TID / State を照合した。

参考: [Microsoft CSwitch](https://learn.microsoft.com/en-us/windows/win32/etw/cswitch)、[ReadyThread](https://learn.microsoft.com/en-us/windows/win32/etw/readythread)、[Microsoft PerfView Parser](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/KernelTraceEventParser.cs)。

## 実行条件と記録品質

- Development、実 Window 1280×720、通常 UI、FPS 無制限、VSync OFF、Tearing ON
- SwapChain 2 枚。GPU 未完了上限は Buffering と同じ。Worker は Buffering から導出
- 各起動で Warmup 2 秒 + 測定 3 秒。Storage を実行ごとに分離
- Collector のみ UAC で管理者起動。Probe は通常権限で起動
- Kernel: Process / Thread / Loader、CSwitch、ReadyThread、DPC / ISR、CSwitch / ReadyThread の Stack
- User: 独自 Marker、DxgKrnl、DXGI、DWM。Run2 は GPUView の設定を参考に legacy DX Provider と CaptureState も追加
- 既存の NT Kernel Logger があれば開始を拒否し、自分が開始した Session だけを停止する
- バックグラウンド Process は制限していない。ETW の記録、Stack 採取、記録の I/O による観測負荷を含む

| 記録 | 構成 | 測定 Frame | 平均 FPS | 全区間 Marker |
| --- | --- | ---: | ---: | ---: |
| Run1 | Buffering 2 / Waitable OFF | 2,860 | 953.059 | 126,001 |
| Run1 | Buffering 2 / Waitable ON、Queue latency 1 | 1,933 | 644.074 | 83,393 |
| Run1 | Buffering 1 / Waitable OFF | 3,377 | 1,125.663 | 160,819 |
| Run2 | Buffering 2 / Waitable OFF | 2,443 | 814.285 | 104,577 |
| Run2 | Buffering 2 / Waitable ON、Queue latency 1 | 2,069 | 689.617 | 87,165 |

両記録とも ETW Event / Buffer の欠落 0、Probe の dropped 0、対象 Event の Decode Error 0。全区間の ETW / 生 CSV 照合が成功した。Run1 は 370,213 区間、Run2 は 191,742 区間を保持している。Clock Marker はこの区間数とは別に各 Process 2 件。

Clock の読取り幅から算出した不確かさは Run1 各 ±50 ns、Run2 OFF ±350 ns / ON ±50 ns。開始・終了の Clock Offset 差は最大 200 ns。これは Clock 対応の検査値であり、Scheduling Event や計測全体の精度保証ではない。

Provider と実行時の背景負荷が異なるため、Run1 / Run2 の FPS や Max を改善率として比較しない。特に Run2 の 26 ms は、記録なしの通常実行でも同じ頻度で発生するという意味ではない。

## 同一 Frame の内訳

以下の行は互いに異なる Frame であり、各行内だけで内訳を比較する。全て Unknown は 0 ms。

| 記録 / 構成 / Frame | Render 全体 | Running | Waiting | Ready |
| --- | ---: | ---: | ---: | ---: |
| Run1 OFF / 4292 | 3.9436 ms | 0.4574 ms | 3.4691 ms | 0.0171 ms |
| Run1 ON / 1467 | 5.0225 ms | 0.3750 ms | 4.6396 ms | 0.0079 ms |
| Run2 OFF / 3732 | 26.3036 ms | 0.9639 ms | 25.2772 ms | 0.0625 ms |
| Run2 ON / 3178 | 15.4617 ms | 1.0157 ms | 14.3625 ms | 0.0835 ms |
| Run2 OFF / 1982 | 11.8211 ms | 3.2836 ms | 0.5589 ms | 7.9786 ms |

### 待機解除までが長い例

Run2 OFF Frame 3732 は、指定 Timeout 1 ms の Fence Event が 11.1335 ms 後に `WAIT_TIMEOUT` で戻り、次の Wait が 14.2544 ms 後に `WAIT_OBJECT_0` で戻った。対応する Waiting はそれぞれ 11.0570 ms と 14.2159 ms。Ready は 0.0411 ms と 0.0129 ms だった。

Run1 ON Frame 1467 の DXGI Event は 4.6595 ms のうち Waiting 4.6396 ms、Ready 0.0079 ms。これらの例では、CPU が実行可能になった後の割り当て待ちを主因とは説明できない。ただし Waiting の内側には通知条件、Timeout 処理、Kernel / Driver の動作があり、GPU Pass の演算だけには分解できていない。

### CPU 割り当て待ちが長い例

Run2 OFF Frame 1982 の子区間は次のとおり。Graph.Execute は Graph.Record を含むため、この表の各行を合算しない。

| 区間 | 経過時間 | Running | Waiting | Ready |
| --- | ---: | ---: | ---: | ---: |
| Fence Event | 2.4320 ms | 0.0245 ms | 0.5589 ms | 1.8486 ms |
| Graph.Record | 2.4045 ms | 0.4898 ms | 0 ms | 1.9147 ms |
| Graph.Execute | 5.9019 ms | 1.5707 ms | 0 ms | 4.3312 ms |
| Present | 3.0763 ms | 1.4945 ms | 0 ms | 1.5818 ms |

Render Thread の Switch Out には Ready / WrDispatchInt が繰り返し現れ、その間に Browser、RuntimeBroker、Service など別 Process が実行されている。特定の Process 一つを原因とは断定していないが、Render の関数時間が伸びた分を全てその関数の処理量として扱えないことは確認できた。

### Single の注意

Run1 Single の最大 Render は 0.8500 ms で全て Running だった。一方、Frame 採用前の GPU Fence Event には 4.7465 ms の区間があり、Waiting 4.7102 ms、Ready 0.0164 ms だった。Frame ID 未割当のこの区間も保存している。Single にすると Render 内の Max が小さく見えても、Cycle 全体の待機が解消したとは扱わない。

## GPU 側の到達点

Run1 は DxgKrnl / DXGI / DWM を取得したが、GPU Queue / DMA の対応付けには不足した。Run2 は SDK 同梱 GPUView の `log.cmd` を確認し、modern DxgKrnl `0x800` と legacy DX `0x2f`、開始時 CaptureState `0x90ffff` を指定して再取得した。

Run2 の OFF Frame 3732 / ON Frame 3178 の時間窓を xperf で展開し、DXGI / DWM の Present 関連 Event を確認した。しかし、対応する GPU Packet の Queue 滞留・実行開始・完了を一意に結べる Event は確認できていない。Provider の開始成功や ETW 欠落 0 は、必要な種類の Event が全て発生した証明にはならない。

現段階では「Fence / DXGI の待機解除前が長い」までが確定範囲。GPUView / WPA による元 ETL の確認、必要な Provider / Keyword とこの環境の Event 形式の確認が次の調査になる。Driver 更新や HAGS 切替は今回実施していない。

## 検証と再現

- 指定の `scripts/codex_build.ps1` による Debug Build 成功
- 関連 Debug Test: `Cue.Foundation.TimingTrace` / `Cue.Editor.InputLatency` の 2 件成功
- Python の Scheduling 分類 Test 3 件成功: Wakeup + Preemption、観測範囲外、Ready 境界欠落
- Development の Probe / ETL Exporter Build と、実 Window の上記 5 起動が成功
- 通常の CMake 設定は `CUE_ENABLE_TIMING_TRACE=OFF` に戻す
- 性能の絶対値を保証する Test や、新たな優先度調整は追加していない

調査時は専用 Build で Trace を有効にし、既存記録と異なる空の出力先を指定する。

```powershell
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE:BOOL=ON
cmake --build --preset windows-vs2026-development --target CueSpikeTimingProbe CueSpikeEtwExport
pwsh -NoProfile -File scripts/capture_spike_etw.ps1 -OutputDirectory C:/Users/sinse/source/repos/CueEngine/out/validation/EtwNew -Cases all
out/build/windows-vs2026/Engine/Tests/Development/CueSpikeEtwExport.exe out/validation/EtwNew/Merged.etl out/validation/EtwNew/Events.csv
python -B scripts/analyze_spike_etw.py out/validation/EtwNew/Events.csv
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE:BOOL=OFF
```

SDK の WPT / xperf と設定済みの vcpkg Build 環境が必要。元 ETL は環境依存の Process / Thread / Stack 情報を含むため、Repository へ追加せずローカルに保持する。

今回の成果物は `out/validation/Performance20261019/Etw144/` 配下。Run1 / Run2 の `Merged.etl`、`Events.csv`、各 `*-spans.csv`、`*-cpu-states.csv`、`CpuScheduling.json`、Capture / Export Log を残している。
