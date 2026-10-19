# M06-15 / Issue #144 作業・性能調査の統合記録

- 更新日: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- 作業 Branch: `m06/issue144/gpu-waits-and-telemetry`
- 作業開始時の Commit: `752d7fd9d7a12c4768acaf7620b84516e5244cca`
- 提出方針: 本書を含む累積変更を上記 Branch に Commit / Push する。`develop` への Merge は行わない。

本書は、GPU 待機と統計取得の改善、入力遅延計測、その後のスパイク調査を引き継ぐための入口である。段階別の報告にある「未 Push」「Kernel ETW 未取得」などは、その報告時点の状態である。最新の結論と制限は本書を参照する。

## 1. 作業の目的と要求

Development 構成で、主に RenderTarget の Clear、ImGui、Present を行う場面でも平均約 650 FPS に留まり、Max が平均から大きく離れることが出発点だった。Waitable Presentation を OFF にすると改善したが、できるだけ ON を維持したいという要求がある。

目的は次の四つ。

1. GPU 完了・表示準備の待機と実処理を分離し、不要な待機や計測処理の負荷を減らす。
2. 統計を間引かず、直近 120 標本の平均・nearest-rank p95・Max・件数という情報品質を保つ。
3. **入力イベント到達 → その入力を反映した Frame の CPU 処理・Present**を計測する。
4. スパイクを同一 Frame の CPU / GPU / OS 記録で調べ、確定した事実と未確定の原因を区別する。

FPS の数値だけを上げるために Waitable、Buffering、Thread 優先度、電源設定を一律変更する方針は採っていない。計測境界の外へ待機を移して改善したように扱うこともしない。

## 2. 現在の結論

**全ての根本原因が判明した状態ではない。** 計測が追加した余分な ImGui Mutex 再取得は修正した。一方、残るスパイクには CPU の実行再開待ち、DXGI の表示準備待ち、GPU Fence 待ちがあり、一つの原因で説明できない。

特に、GPU Pass 区間が約 0.03～0.05 ms でも、CPU 提出終了から最初の GPU Timestamp まで約 8.8 ms / 30.4 ms 空く事例を確認した。軽い Shader であることだけでは、この待機を解消できない。

最新の約 49.6 ms の再現は**非前面化の後**だった。ただし、以前の約 33 ms は前面状態を確認した実行にも存在する。今回の非前面化だけで過去の全スパイクを説明することはできない。

## 3. 実装した変更

### 3.1 統計取得の負荷低減

`Foundation/TimingSamples.h` は、標本を追加する際に固定容量の整列済み配列と整数の商・余りによる総和を維持する方式へ変更した。統計を取得するたびの全標本コピー・Sort を避け、`statistics()` は定数時間で結果を返す。

- 直近 120 標本の全てを対象とし、間引きや近似 Percentile は使用しない。
- Warmup 中の標本数、重複値、置換、長い期間の整数平均を Test する。
- 追加処理は最大 120 要素の移動を伴う。全体が O(1) になったという意味ではない。
- FrameController の共有 Lock 内では、小さな集計結果を取得する。UI は件数も表示する。

### 3.2 GPU / 表示準備の待機

- `DX12GpuCommandQueue::wait_for_fence_for()` を追加し、完了 Event、Timeout、Message による復帰を扱う。
- `GpuFrameQueue` から最古の完了条件を参照し、GPU 上限待ちで実際の Fence 完了通知を使う。
- Single Thread の描画開始前の待機では、GPU / DXGI Event と Message を扱い、固定 1 ms Sleep 後の再確認だけに依存していた経路を変更する。
- Stop / Close / 最小化などの確認を残す。有限 Timeout と再確認は残り、Engine 全体の Sleep や Polling を全廃した変更ではない。
- Main の開始許可待ちを GPU、DXGI、Resource に分け、Render 外の待ち時間も公開する。

`DX12SwapChain` は Present API の開始・終了時刻と統計問い合わせ期間を分離する。Present 全体、API、統計取得、Present Return 間隔、表示時刻由来の間隔を混同しないようにした。

### 3.3 入力遅延

`Platform/InputEventTiming.h` と `WindowsHost/InputLatency.h` を追加し、Windows の Key / Mouse Message を UI Handler に渡す前に単調時刻と Event ID を記録する。ImGui の入力受付と Snapshot の採用を通して、実際に記録・Present された Frame に対応付ける。

| 計測点 | 内容 |
| --- | --- |
| 入力到達 | Window Procedure で Key / Mouse Message を受け取った時刻 |
| Frame capture | 対象 Frame が反映する入力を確定した点 |
| Render start | その Frame の Render Callback 開始 |
| Present call / return | 同じ Frame の Present API 呼出直前 / 直後 |
| CPU Render complete | Host の Render Callback 完了点 |

入力イベント単位で直近 120 標本を集計する。入力のない Frame を 0 ms の標本にせず、未描画、Consumer 不在、取消、Error を成功した遅延に混ぜない。観測・反映・Present・Skip・不正値の累計と最後の Event / Frame / Present ID も公開する。

これは物理操作、OS Queue 投入からの時間、GPU 完了、画面への実表示までの遅延ではない。既存の Frame 採用時刻からの統計は名称を変更し、入力イベント到達からの値と区別した。

### 3.4 調査中に発見・修正した計測由来の競合

最初の入力計測では Present 後の `was_frame_recorded()` が ImGui Context Mutex を再取得していた。最小 UI の Frame 4707 では Render 6.5717 ms のうち、この確認が 6.1289 ms、Mutex 取得が 6.1263 ms だった。

`ImGuiManager::render_frame()` が既に必要な Lock を保持している間に実記録結果を返し、EditorHost が Render Thread 内で Frame ID と結果を保持する方式へ修正した。後から再取得する Getter は削除した。

修正後の四条件では `ImGui.VerifyFrame` の Trace は全て 0 件。成功 / Skip / Error の結果伝達を実 GPU / WARP で確認した。実際の描画記録に必要な Context 排他や GPU Resource 再利用の Fence 条件は残る。

### 3.5 任意の詳細 Trace と調査 Tool

| 範囲 | 追加・変更内容 |
| --- | --- |
| Foundation | `TimingTrace.h`、CPU 区間 / 論理 Frame / GPU 生記録の Sink。`CUE_ENABLE_TIMING_TRACE` の既定は OFF |
| Runtime / Host / ImGui | Main / Update / Render、Message Pump、UI 構築・Copy・Mutex・確保、GPU / DXGI 待機などの区間 |
| DX12 / FrameGraph | 提出前後、Queue / Fence ID、GPU Pass 生 Timestamp、提出時の元の論理 Frame ID、Clock Calibration |
| 手動 Probe | `SpikeTimingProbe.cpp`。設定比較、前面状態、固定容量の記録、終了後の CSV / ETW Marker 出力 |
| ETW | `SpikeEtwExport.cpp`、`capture_spike_etw.ps1`。Kernel Scheduling と表示 / GPU Provider の採取・Merge・Export |
| 解析 | `analyze_spike_trace.py`、`analyze_spike_etw.py`、`analyze_gpu_timeline.py`、`validate_spike_capture.py` と Test |
| 閾値検出 | `SpikeTrigger.h` と Test。最初の超過を保存し、前後の循環履歴を確保 |

計測中の CSV 出力や容量拡張を避け、配列を事前確保する。Trigger mode は CPU が Thread ごとに 524,288 区間、GPU が 262,144 記録を循環保持する。古い履歴の上書きと保持範囲内の欠落を別々に報告し、保持中の標本は間引かない。

Trigger は原則、開始後 6 秒から CPU Render 20 ms または CPU 提出終了 → 最初の GPU Query 5 ms を検出し、その後も 2 秒記録する。GPU 側の検出は完了 Query 回収時に行う。Kernel / GPU ETW を保存・停止した後に終了処理と大量の Marker 出力を行い、後処理による履歴の上書きを避ける。

## 4. 調査結果

### 4.1 CPU Scheduling によるスパイク

ETW `Run2` の Waitable OFF、Frame 1982 は Render **11.8211 ms**。内訳は Running 3.2836、Waiting 0.5589、Ready **7.9786 ms**、Unknown 0 だった。優先度 8 の Render Thread が、優先度 9～13 の Thread に実行を譲る事例を 33 回確認した。

この Frame には OS の CPU Scheduling が大きく寄与している。DPC も確認したが、別 CPU の重なりを足して Render の遅延と扱っていない。Worker の ON / OFF だけを全スパイクの原因とは判断できない。

### 4.2 Fence 待ちは GPU 側の進行と対応する例がある

ETW `Run3` の Frame 4084 は Fence 4083 を 4.8315 ms 待った。CPU 状態は Waiting 4.8172、Ready 0.0045、Running 0.0098 ms。対応する GPU Queue Packet の Stop から CPU Return までは約 7.6 µs だった。

この例は、完了後の CPU 再開遅延だけで待ち全体を説明できない。ただし Packet の Submit → Stop には Queue / Driver / Scheduling が含まれ、Shader が同じ時間だけ演算したとは言えない。

### 4.3 前面を維持した実行にも表示待ちがある

GPU Timestamp を追加した Native Q2 実行の Frame 5235～5238 は、Render 約 28.7～33.6 ms、DXGI 待ち約 28.0～32.6 ms に対し、同 Frame の GPU Pass 区間は約 0.035～0.051 ms だった。計測境界で前面状態を確認しているが、この回には Kernel / 表示 ETW がない。

別の Frame 5317 は CPU 提出終了 → 最初の GPU Query が **8.823244 ms**。GPU Pass は 0.033792 ms、前後 Calibration の投影差は約 3.492 µs。後続 Frame の対応 Fence 待ちは 7.89 ms だった。提出後の Driver / Scheduler / Query 前の Command の内訳は未確定である。

### 4.4 最新の Trigger 捕捉: 非前面化後の大きなスパイク

`Trigger144/Run2` は Development、1280×720、Buffering 2、GPU 上限 2、Waitable ON、VSync OFF、Tearing ON、通常 UI、FPS 上限なしで約 119.104 秒記録した。表示系 Provider を使い、GPU Trigger の閾値を 1000 ms にして CPU の再現を優先した。

計測開始から **117065.7328 ms** に非前面・可視・非最小化を観測。その約 15.667 ms 後に Trigger Frame が始まり、継続する 2 秒にも大きなスパイクが残った。

| Frame | CPU Render ms | Running ms | Waiting ms | Ready ms | GPU Pass 区間 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 100817: Trigger | 21.8321 | 0.7800 | 21.0432 | 0.0089 | 0.034816 |
| 102173 | 49.5572 | 0.8815 | 48.5894 | 0.0863 | 0.035840 |
| 102175 | 31.5072 | 0.8587 | 30.5172 | 0.1313 | 0.034816 |

三 Frame の Unknown / Ended は 0。Frame 102173 の DXGI Event 待機合計は **48.7194 ms**。最後の Ready 通知は同 ETL で照合した `dwm.exe` の Thread 実行 Context 中だった。通知元の Stack や ISR / DPC を分離した結果ではなく、DWM 内部の原因までは断定しない。

同じ記録では提出後の大きな空白も捕捉した。

- Frame 102173 の提出終了 → 最初の GPU Timestamp: **30.392140 ms**。
- Frame 102174: **29.436856 ms**。二つの Pass 区間は約 0.036 / 0.034 ms で続けて完了。
- Frame 102175 が対応する Fence を待った期間: **29.1953 ms**。対象 Frame の最後の Query から CPU Return まで約 89.07 µs。
- 前後 Calibration の投影差は Frame 102173 / 102174 が約 0.680 / 0.664 µs。

これらは同じ期間に重なる区間を含み、49.6 ms に 30.4 ms を単純加算する値ではない。この回は GPU Base / Scheduling を省いたため、GPU 内部の実行開始が遅れる詳細原因は決定できない。

保持中の CPU Render は非前面化前 29,280 標本が平均 / p95 / Max = 1.1670 / 4.3693 / 10.1370 ms、観測後 1,491 標本が 1.3553 / 4.4785 / 49.5572 ms。期間の異なる一回の自然な状態変化であり、制御した因果比較ではない。前面を取得した別アプリは記録していない。

### 4.5 未検出の実行と情報品質

同じ Trigger 調査の Run1 は GPU Base / Scheduling / HWS を含めて 60 秒記録し、前面を維持したが開始後 6 秒からの 20 / 5 ms 超過は未検出だった。**保持中**の Render Max は 7.508 ms。CPU の古い記録は循環上書きされているため、全 60 秒の生記録 Max とは表現しない。

| 実行 | 保持 CPU 区間 | 保持 GPU 記録 | CPU CSV と ETW Marker の全件照合 | ETW Event / Buffer Lost |
| --- | ---: | ---: | --- | --- |
| 保存経路検証 Validation2 | 392,308 | 35,956 | 成功 | 0 / 0 |
| Run1 | 1,106,170 | 230,434 | 成功 | 0 / 0 |
| Run2 | 1,149,355 | 262,144 | 成功 | 0 / 0 |

Run2 は CPU 累計 4,438,940 区間中の古い 3,289,585 区間、GPU 累計 403,262 記録中の古い 141,118 記録が上書き済み。保持開始境界の 2 Frame / 4 Pass は提出情報が残らず、未対応理由を保存した。

Trigger 前 5 秒・検出後約 2 秒の時刻範囲と保持連番、Collector の保存・停止・Merge は確認できた。一方、前面固定条件は不成立で、Run2 の `Coverage.json` は **`data_coverage_succeeded: true`、`status: condition_changed`、`succeeded: false`**。記録範囲がそろうことと、実験条件が維持されたことを分けている。

## 5. 計測結果の読み方と制限

- 異なる Frame の Max や、独立の 120 標本の Max を足し合わせない。
- Single Thread は Render Callback の外にも開始許可待ちがある。Render 時間だけで FPS や入力遅延を判断しない。
- Native Wait の Timeout 指定 1 ms は、実際の Return 時間が 1 ms 以下という保証ではない。Timeout と Signaled を生記録で区別する。
- CPU の Running には割込み等の影響が残り、純粋な関数演算時間ではない。Ready と Waiting、Unknown を別々に保持する。
- GPU Pass は Timestamp Query 間の区間であり、Preemption 等も含み得る。Present 完了や画面表示そのものではない。
- QPC と CPU 単調時計は ClockSync で対応付け、GPU は Queue ごとの Calibration と周波数を使う。巨大な絶対 GPU Tick は差を取ってから計算し、前後 Calibration による投影差も保存する。
- CPU 提出開始・終了を両方残す。GPU が API Return 前に進む場合があり、負の差を 0 に丸めない。
- Trace、Calibration、ETW 自体に負荷がある。通常 Build は Trace OFF。短い別実行の FPS 差を本変更全体の改善率として採用していない。
- HWS / GPU Event を取得できても、既存の解析 Export で全ての内部 Scheduling を復元できたわけではない。解析できないことを GPU 作業が存在しない証拠にしない。

## 6. 検証結果

以下は累積変更を調査する過程で実施した結果。本書の作成時は Source を変更していない。

| 検証 | 結果 |
| --- | --- |
| 指定の `pwsh -NoProfile -File scripts/codex_build.ps1` | 最新の既定 Debug Build 成功 |
| Development / Trace ON | Probe / Exporter / Test Build 成功、TimingTrace / SpikeTrigger の 2 Test 成功 |
| 最新 Debug / Trace OFF の関連 Test | TimingTrace / SpikeTrigger / DX12 FrameGraphFrames / MainFrameGraph の 4 Test 成功 |
| Python 解析 Test | GPU 3、CPU 3、保持範囲検証 4、全 10 Test 成功 |
| 入力遅延・実記録結果 | 調査前段で入力遅延と ImGui DX12 実 GPU / WARP Test 成功。Buffering 1 / 2 / 3 × Waitable ON / OFF、Skip / Error / Consumer 不在 / 入力繰越を確認 |
| 過去段階の Debug 全体実行 | 53 件中 52 成功、`Cue.WindowsHost.Lifecycle` が一度 10 秒 Timeout。単独再実行は 5 回連続成功（7.26～7.51 秒）。原因未確定、Timeout 設定は変更せず |
| 最終設定 | `CUE_ENABLE_TIMING_TRACE:BOOL=OFF` を確認 |

全体 Test の過去の Timeout を、関連 Test の成功だけで解消したとは扱わない。性能値は CTest の速度合否基準にしていない。

## 7. 再現と引き継ぎ

### Build

Repository Root で実行する。

```powershell
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=ON
cmake --build --preset windows-vs2026-development --target CueSpikeTimingProbe CueSpikeEtwExport CueSpikeTriggerTests
```

### 閾値を待って保存

出力先は新しい空 Directory とする。Collector のみ UAC で昇格し、Probe は通常権限で起動する。既存の NT Kernel Logger がある場合は開始を拒否する。

```powershell
pwsh -NoProfile -File scripts/capture_spike_etw.ps1 `
    -OutputDirectory C:/Users/sinse/source/repos/CueEngine/out/validation/SpikeNew `
    -Seconds 120 -Cases latency2 -Triggered -PresentationOnly `
    -RenderTriggerMs 20 -GpuTriggerMs 1000 -RequireForeground
```

GPU Queue / Scheduling も対象にする場合は `-PresentationOnly` を外し、`-HardwareScheduling` を付ける。CPU 20 ms / GPU 5 ms が既定の検出目安。Trigger mode の記録指定は 8～180 秒、`latency2` のみ。通常の比較 mode と条件を混同しない。

保存後は同じ Prefix の生 CSV と Exported Event を解析する。以下の `RUN` は実際の出力 Directory に置き換える。

```powershell
out/build/windows-vs2026/Engine/Tests/Development/CueSpikeEtwExport.exe RUN/Merged.etl RUN/Events.csv
python -B scripts/analyze_spike_etw.py RUN/Events.csv
python -B scripts/analyze_gpu_timeline.py RUN/on-q2
python -B scripts/validate_spike_capture.py RUN
```

保持範囲検証の Exit Code は valid が 0、incomplete / condition_changed が 1、not_triggered が 2。未検出や前面条件変化を解析成功と取り違えない。作業後は通常設定へ戻す。

```powershell
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=OFF
pwsh -NoProfile -File scripts/codex_build.ps1
```

### 成果物

ローカルの生データと Build / Test Log は `out/validation/Performance20261019/` 配下に保存している。

- `Spikes144/`: 初期比較、ImGui 再取得の修正前後、Lifecycle 再 Test。
- `Etw144/`: CPU Scheduling、Fence / Queue / 表示 Event の照合。
- `Waitable144/`: 前面状態と Queued presents の追試。
- `GpuTimeline144/`: GPU Timestamp / Calibration と CPU 提出の対応。
- `Trigger144/`: 保存手順検証、Run1 / Run2、生 CSV、Merged ETL、保持範囲検査、最終 Build / Test。

最新の根拠は `Trigger144/Run2/FocusComparison.json`、`TriggerCpuAndWindow.json`、`FocusedScheduling.json`、`Processes.txt`、`on-q2-gpu-frames.jsonl`、`on-q2-cpu-states.csv`、`Coverage.json` にある。これらの `out/` データは Git 管理外で、Push / Clone に含まれない。Source、調査 Script、Test、本書と段階別報告を Git に含める。

### 次に調べること

1. 前面 / 非前面への切替を制御し、同じ設定で交互に計測する。前面を取得した Process も残す。
2. その切替と大きな提出後区間を、GPU Base / Scheduling と表示 Provider を同時に有効にした記録で捕捉する。
3. GPU Query より前の Command、Driver / GPU Scheduler、他 Queue の寄与を分け、DWM の表示許可が遅れる理由を追う。
4. ImGui の実記録時の排他短縮や Stop Event と組み合わせた待機設計は、正しい所有権・取消条件を確認してから別途評価する。

現時点では Driver の不具合、特定の電力設定、Worker Thread 自体を一律の原因とは確定していない。上記は残る調査候補であり、追加の Engine 改修や Merge を実施済みという意味ではない。

## 8. 段階別の詳細報告

1. [初期スパイク調査と ImGui 再取得の修正](M06-15-Frame-Spike-Investigation.md)
2. [Kernel ETW による Waiting / Ready の分離](M06-15-Frame-Spike-ETW-Investigation.md)
3. [Scheduling と GPU Queue / 表示の照合](M06-15-Frame-Spike-Scheduling-And-Presentation.md)
4. [Waitable / Queued presents の追試](M06-15-Waitable-Latency-Followup.md)
5. [GPU Timestamp と CPU 提出・表示待機](M06-15-Gpu-Timestamp-And-Presentation.md)
6. [閾値検出による循環保存と最新の非前面化観測](M06-15-Triggered-Spike-Capture.md)
