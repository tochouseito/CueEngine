# M06-15: 閾値検出による循環 ETW 保存と非前面化の観測

- 日付: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 前回: [GPU Timestamp と CPU 提出・表示待機の対応](M06-15-Gpu-Timestamp-And-Presentation.md)
- 成果物: `out/validation/Performance20261019/Trigger144/`

## 結果

**約 49.6 ms のスパイクを GPU Timestamp・CPU 生区間・Kernel Scheduling・表示 ETW と同時に保存できた。** 今回の大きなスパイクは非前面化の後に発生した。

| Run2 の Frame | CPU Render ms | Running / Waiting / Ready ms | 同 Frame の GPU Pass 区間 ms |
| --- | ---: | --- | ---: |
| 100817（保存を Trigger） | 21.8321 | 0.7800 / 21.0432 / 0.0089 | 0.034816 |
| 102173（保存後の継続区間） | 49.5572 | 0.8815 / 48.5894 / 0.0863 | 0.035840 |
| 102175 | 31.5072 | 0.8587 / 30.5172 / 0.1313 | 0.034816 |

対象三 Frame の Unknown / Ended は 0。GPU Pass 区間は最初の Begin Query から最後の End Query までであり、純粋な Shader 実行時間や Present 全体の完了時間を表すものではない。

この記録では CPU の実行再開待ちだけで約 49.6 ms を説明できない。表示 Event の待機と、提出後に GPU の最初の Query が進むまでの区間が大きい。前面を維持した元の状況も同じ原因であるという確定には至っていない。

## 計測の変更

### 検出と保持

- 手動 Probe に Trigger mode を追加。開始後 6 秒から、CPU Render 20 ms、または提出終了 → 最初の GPU Timestamp 5 ms を目安に最初の超過を検出する
- Trigger の種類・Frame・期間・検出時刻を一度だけ公開し、その後も 2 秒記録する。発生しない場合は指定時間で終了する
- GPU 側の検出は完了 Query を回収した時点で行う。提出・Calibration・Pass の Frame / Queue / 種類・周波数を検証し、巨大な絶対 GPU Tick は差を取ってから変換する
- GPU 検出の目安は一つの Calibration による推定である。保存後の解析では前後の Calibration による投影差も確認する
- CPU は Thread ごとに 524,288 区間、GPU は 262,144 記録を開始前に確保して循環保持。容量を動的に増やさない
- CPU は保持した全区間に元の `source_sequence` を保存する。GPU の累計件数・保持件数・上書き件数も明示する
- 120 標本の平均・p95・Max と入力遅延の既存統計は従来どおり更新する。保存範囲の全標本を残し、間引きや平均値への置換を行わない

**生記録の保存対象は循環保持範囲である。** 待受全期間については累計件数を残し、保持開始より古い履歴の上書きと、保持範囲内の欠落を区別する。

### ETW の保存順

1. Kernel 128 MiB、GPU / 表示 256 MiB を目安に Memory Buffering session を開始。Kernel は CSwitch / ReadyThread / DPC / Interrupt / Hard Fault などを取得する
2. ClockSync と終了後の CPU Marker 用には別の逐次 Session を使用する
3. Trigger 後 2 秒の CPU 記録を終え、Probe が保存を要求する
4. Collector が DxgKrnl の CaptureState を取得し、Kernel / GPU session を `-flush -f` で保存して停止する
5. 保存確認後に Probe が Shutdown・残りの GPU Query 回収・CPU Marker 出力・CSV 保存を行う
6. Marker Session を停止し、三つの ETL を Merge する

これにより、大量の CPU Marker 出力や CSV 保存の間に、Kernel / GPU の対象期間が循環 Buffer から上書きされることを防ぐ。Buffering session の停止だけでは保存されないため、明示的に flush を行う。[Microsoft: Xperf start / buffering](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/start)、[Microsoft: InBuffer profile](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/xperf-profile-overview)

初回の保存経路検証では開始時の `-f` との組合せが Windows に拒否された。開始時の File 指定を外し、保存時の flush に変更した再検証は成功した。失敗時のログも `Validation/` に保持している。

### 保存範囲と条件の検証

`validate_spike_capture.py` を追加した。

- Trigger の元 Frame より前 5 秒、検出後 1.95 秒までの時刻範囲を検証する。記録自体は検出後 2 秒続け、Provider の末端 Event 周期に最大 50 ms の余裕を取る
- 対象 CPU Thread、各 CPU の Kernel CSwitch 先頭、DxgKrnl / DXGI / DWM の Event 先頭・末尾時刻を確認する
- CPU の保持範囲内の連番欠落、Probe / ETW の欠落、Collector の保存・停止・Merge 状態を検証する
- 全生 CPU Marker と元 CSV の照合は既存の `analyze_spike_etw.py` が行う
- 非前面化は `condition_changed`、閾値未検出は `not_triggered`、保持範囲不足は `incomplete` と区別する
- Event の時刻範囲検査だけで Driver 内部や全 Provider payload の完全性を保証するものではない。対象 Frame の Scheduling の Unknown も別途確認する

起動時に一度前面化を試み、計測中は Step 境界で前面・可視・最小化・Client Size の状態を観測する。今後の記録では初期状態も Log に明示する。

## 試行条件

共通: Development、1280×720、Buffering 2、GPU 上限 2、SwapChain 2、通常 UI、FPS 上限なし、VSync OFF、Tearing ON、Waitable ON。計測中に Build や解析を並行しなかった。収集・Calibration 自体の負荷、OS や他 Process の負荷による影響は残る。

| 試行 | 設定と実時間 | 結果 |
| --- | --- | --- |
| Validation2 | 表示系 Provider、CPU 閾値 0.1 ms、約 8 秒 | 保存経路の検証専用。前後範囲・前面条件・全 Marker 照合が成功 |
| Run1 | GPU Base / Scheduling / HWS あり、閾値 20 / 5 ms、60 秒 | 開始後 6 秒からの検出なし。前面を維持した。保持中の CPU Render Max は 7.508 ms |
| Run2 | 表示系 Provider、CPU 閾値 20 ms、GPU 閾値 1000 ms、119.104 秒 | CPU Trigger を捕捉。途中で非前面化し、保存後 2 秒内に約 49.6 ms も発生 |

Run2 は表示待機の再現を優先して GPU 開始遅延による Trigger を実質無効にした。GPU の生 Query と Calibration は取得している。GPU Queue / Scheduling の大量 Event はこの回では省いているため、Driver / GPU Scheduler の内訳の決定には使えない。

## Run2 の前面状態との関係

PID 40460、Render TID 36640。

| 時点 | 計測開始から ms |
| --- | ---: |
| `Probe.WindowState` が 10（非前面、可視、非最小化、1280×720）になった観測 | 117065.7328 |
| Frame 100817 の CPU Render 開始 | 117081.4002 |
| 21.8321 ms の終了・Trigger 検出 | 117103.2323 |
| Frame 102173 の CPU Render 開始 | 118876.9856 |
| 49.5572 ms の CPU Render 終了 | 118926.5428 |
| 記録停止 | 119103.869 付近 |

非前面化の観測から約 15.667 ms 後に Trigger Frame が始まった。状態の変化は一回。可視 Flag は遮蔽の有無を表さず、どのアプリが前面を取得したかはこの記録から決定できない。

保持中の CPU Render をこの観測の前後で分けると:

| 区間 | 標本数 | 平均 / p95 / Max ms |
| --- | ---: | --- |
| 非前面化の観測より前 | 29,280 | 1.1670 / 4.3693 / 10.1370 |
| 非前面化の観測後 | 1,491 | 1.3553 / 4.4785 / 49.5572 |

これは長さの異なる一回の自然な状態変化であり、非前面化だけを操作した比較試験ではない。表は保持範囲の標本で、待受全期間の分布ではない。

証拠: `Run2/FocusComparison.json`、`TriggerCpuAndWindow.json`、`on-q2-spans.csv`

## 約 49.6 ms の表示待機

Frame 102173 の DXGI.Event は合計 **48.7194 ms**。個別の Return は:

| 実測区間 ms | 結果 |
| ---: | --- |
| 6.8837 | Timeout |
| 16.0031 | Timeout |
| 15.7585 | Timeout |
| 10.0741 | Signaled |

CPU Render 全体は Running 0.8815、Waiting 48.5894、Ready 0.0863 ms。最後の ReadyThread は DWM TID 11008 の実行 Context 中に発行されていた。同 ETL の Process/Thread 一覧で **dwm.exe PID 27836** と照合した。

Ready Event の Header PID/TID は無効値だったため、同じ CPU の直前 CSwitch を使った。DWM Context 中の通知という観測であり、ISR/DPC や DWM 内部の具体的な呼出元を Stack で分離したものではない。1 ms Timeout 指定と実際の Return 間隔も区別する。

この Frame の GPU Pass 区間は 0.035840 ms。ただし、その Pass 自体が進むのは次節のように提出から約 30 ms 後だった。

## 提出後の約 30 ms と後続 Fence 待機

| 時点 | ms |
| --- | ---: |
| Frame 102173 / Fence 102174 の CPU 提出終了 | 118926.16115 |
| Frame 102174 / Fence 102175 の CPU 提出終了 | 118927.16045 |
| Frame 102175 が Fence 102174 の CPU 待機を開始 | 118927.4589 |
| Frame 102173 の最初 / 最後の GPU Timestamp（推定） | 118956.553290 / 118956.589130 |
| Frame 102174 の最初 / 最後の GPU Timestamp（推定） | 118956.597306 / 118956.631098 |
| Frame 102175 の Fence 待機 Return | 118956.6782 |

- Frame 102173 の提出終了 → 最初の GPU Timestamp: **30.392140 ms**
- Frame 102174 も **29.436856 ms** 空いた後に進み、二つの GPU Pass 区間は約 0.036 / 0.034 ms で続けて完了
- CPU の Fence 待機は計 **29.1953 ms**。待った Frame 102173 の最後の Query から CPU Return まで **89.07 µs**
- Clock の前後投影差は Frame 102173 が **0.680 µs**、102174 が **0.664 µs**

保持中の非前面化より前の GPU 提出終了 → 最初の Query の最大は約 1.719 ms。この記録では大きな提出後の区間も非前面化の後に集中した。

この区間には Query より前の Command、Driver、Queue / GPU Scheduling、他 GPU 作業の影響が含まれ得る。Pass の Shader 処理や Worker の CPU Ready 待ちだけを原因とする根拠にはならない。

証拠: `Run2/FocusComparison.json`、`on-q2-gpu-frames.jsonl`、`on-q2-cpu-states.csv`、`FocusedScheduling.json`

## 記録品質

| 試行 | 保持 CPU 区間 | 保持 GPU 記録 | CPU Marker と元 CSV の全件照合 | ETW Event / Buffer Lost |
| --- | ---: | ---: | --- | --- |
| Validation2 | 392,308 | 35,956 | 成功 | 0 / 0 |
| Run1 | 1,106,170 | 230,434 | 成功 | 0 / 0 |
| Run2 | 1,149,355 | 262,144 | 成功 | 0 / 0 |

各回の Probe の保持範囲内の容量不足・競合による欠落、Native Decode Error は 0。

- Run1 の CPU 累計は 2,544,532 区間、保持開始より前の上書きは 1,438,362 区間。GPU の上書きは 0
- Run2 の CPU 累計は 4,438,940 区間、古い履歴の上書きは 3,289,585 区間。GPU は累計 403,262、上書き 141,118 記録
- Run2 は 65,507 Frame / 131,014 Pass を Clock 対応付け。保持開始境界の 2 Frame / 4 Pass は提出が既に上書きされており、未対応の理由を保存した
- Run2 の Trigger 前 5 秒・後約 2 秒について、CPU / Kernel / 表示 Provider の時刻範囲と連番検査は成功。全期間の前面固定条件だけが不成立で、`Coverage.json` は **condition_changed** として保持している
- Kernel の未分類 State は Validation2 / Run1 / Run2 全 Process 合計で 65 / 109 / 86 件を保持。今回の対象三 Frame は Unknown 0
- 各 Collector は Kernel / GPU 保存、Kernel / GPU / Marker Session 停止、Merge 完了を確認

## 変更範囲と検証

今回の変更は手動 Probe、検出 Helper / Test、ETW Collector / Exporter、保持範囲の解析・Test、Test の CMake 登録と本報告。Engine の描画・待機方針を切り替える変更は含まない。

- `pwsh -NoProfile -File scripts/codex_build.ps1`: 既定 Debug Build 成功
- Development / Trace ON: Trigger と TimingTrace の 2 Test 成功
- Debug / Trace OFF: 上記に DX12 FrameGraphFrames / MainFrameGraph を含む 4 Test 成功
- Python: GPU 解析 3、CPU 解析 3、保持範囲検証 4 Test 成功
- `git diff --check` 成功。CMake の `CUE_ENABLE_TIMING_TRACE` は OFF に復元
- 既存の未 Commit 変更を保全。Commit / Push / Merge は行っていない

## 残る調査対象

今回、約 50 ms を同じ ETW に捕捉でき、非前面化との時間的な関係、表示待機と提出後の GPU 区間を分離できた。次に原因を絞るなら、**前面 / 非前面への切替を制御した比較**と、その場面の GPU Base / Scheduling 記録が対象になる。

今回の非前面化が元の全スパイクを説明するとは断定しない。特定の Driver 設定、GPU Scheduling 方針、DWM 内部処理を原因名として決める証拠はまだない。
