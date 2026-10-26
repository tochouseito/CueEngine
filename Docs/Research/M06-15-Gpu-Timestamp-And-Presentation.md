# M06-15: GPU Timestamp と CPU 提出・表示待機の対応

続報: [循環 ETW 保存と非前面化の観測](M06-15-Triggered-Spike-Capture.md) で約 49.6 ms を同じ ETW に保存した

- 日付: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 前回: [Waitable Latency Followup](M06-15-Waitable-Latency-Followup.md)
- 記録: `out/validation/Performance20261019/GpuTimeline144/`
- 実行方針の変更、Commit、Push、Merge は行っていない

## 結果

今回、長い区間を次の二種類に分けられた。

1. **描画 Pass が短くても続く DXGI 表示枠待機**: NativeQ2 の Frame 5237 は CPU Render 33.606 ms、DXGI 待機合計 32.5901 ms。直前の Frame 5236 の最後の GPU Timestamp から表示待機解除まで約 31.739 ms あった。
2. **CPU 提出後、最初の GPU Timestamp までの遅延**: NativeQ2 の Frame 5317 は ExecuteCommandLists が戻ってから最初の Pass Timestamp まで約 8.823 ms。Pass 全体の Timestamp 区間は 0.033792 ms で、後続 Frame 5319 の Fence 待機 7.890 ms と対応した。

以前の約 50 ms は再現しなかった。約 33 ms の連続スパイクは ETW なしの回で発生し、ETW 同時記録の二回では再現しなかった。このため、その 33 ms を発生させた DWM・Driver・Kernel の具体的な Event は未確定である。ETW の有無が原因だったという判定もしない。

## 今回追加した計測

- 既存 Pass Query の生の Begin/End Tick、周波数、Queue 識別子、Pass 番号を保存
- `mark_submitted` 時点の論理 Frame ID を Slot に保持し、Slot 再利用時の別 Frame ID を付けない
- GPU 完了後に回収し、Shutdown 時も最後の Slot を回収
- Queue ごとに提出直前と Fence 完了待機後の `GetClockCalibration` を保存。失敗の状態値も残す
- `ExecuteCommandLists` 呼出の QPC Begin/End と、それに対応する Fence 値を保存
- QPC と `steady_clock` の対応を開始・終了時に取得。ETW なしの場合も Log に保持
- Probe は GPU 記録用の固定容量を開始前に確保し、文字列は借用期間中に複写。計測中の File 出力、再確保、間引きを行わない
- GPU の回収は CPU 計測終了後も Host Shutdown まで継続し、CPU と GPU の計測境界を区別

`CUE_ENABLE_TIMING_TRACE` を明示的に有効にした調査 Build で使用する。通常の 120 標本の平均・p95・Max を置き換えたり、標本数を減らしたりしていない。最終 CMake Cache は **OFF** に戻した。

調査時には Calibration API と記録処理の負荷が増える。ETW の記録も実行状態へ影響し得るため、ここで得た FPS を無計測時の性能や完全に同一の再現条件とは扱わない。

### 時刻の意味と制約

`GetClockCalibration` は GPU Counter と CPU QPC を対応付ける。Timestamp は先行処理の完了点を記録するため、Pass 区間を純粋な Shader 実行時間と同一視しない。[Microsoft: Clock Calibration](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-getclockcalibration)、[Microsoft: Timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)

- 生 Tick は整数のまま保存し、解析では `Fraction` を使用。約 1.79×10^18 の絶対 Tick を先に float に変換しない
- 同一 Queue の提出直前の Calibration で投影し、対象 Timestamp より後の Calibration でも投影する。両者の差を `calibration_sensitivity_ns` として全標本に残す
- この差は変換の感度であり、Driver の Clock 対応誤差を保証する上限ではない。GPU Clock は Idle 中に相関が変化し得る。`SetStablePowerState` は使用していない
- CPU 提出の開始と終了の両方を保持。GPU が API の Return より先に進む場合も負の差をゼロへ丸めない
- 提出から最初の Pass Timestamp までには、Driver、Queue、GPU Scheduling、最初の Query より前の Command が含まれる。これを単独で「GPU Scheduler の待ち」と断定しない
- 最後の Pass Timestamp は Resolve・Signal・Present 全体の完了点ではない
- 複数 Batch の同一 Frame を一提出と誤認しない。今回の単一 Queue Probe では一 Frame 一提出である。解析器は複数提出を未対応として明示する

## 条件と集計

Development、1280×720、Buffering 2、GPU 上限 2、SwapChain 2、通常 UI、FPS 上限なし、VSync OFF、Tearing ON、Waitable ON。前面、可視、非最小化、Client Size の状態は全 Step 境界で正常値 11 を維持した。可視 Flag だけでは他 Window による遮蔽を判定できない。

計測中に Build や解析を並行しなかった。OS や他 Process の負荷、GPU Clock は固定していない。各行は一回の試行で、FPS の優劣を確定する比較ではない。

| 記録 | 秒 | Queue 上限 | FPS | CPU Render 平均 / p95 / Max ms | 最初〜最後の GPU Pass Timestamp 平均 / Max ms |
| --- | ---: | ---: | ---: | --- | --- |
| NativeQ1 | 10 | 1 | 730.445 | 1.3586 / 2.8705 / 4.1458 | 0.033460 / 0.059392 |
| NativeQ2 | 20 | 2 | 1034.323 | 0.9603 / 2.8150 / 33.6060 | 0.047340 / 0.087040 |
| EtwQ2 | 20 | 2 | 1034.724 | 0.9591 / 3.8007 / 4.9714 | 0.048905 / 0.088992 |
| EtwQ2Repeat | 20 | 2 | 1140.500 | 0.8690 / 3.8985 / 5.2373 | 0.027614 / 0.057344 |

CPU Render は待機を含む全 CPU 生区間の統計。GPU 列は完了 Query の各 Frame における最初〜最後の区間で、UI 表示の直近 120 標本とは区別する。

## 1. 約 33 ms の表示待機

NativeQ2、PID 16056、Render TID 45196。時刻は計測開始からの ms。

| Frame | CPU Render ms | DXGI.Event 合計 ms | 同 Frame の GPU Pass 区間 ms |
| --- | ---: | ---: | ---: |
| 5235 | 28.6699 | 28.0389 | 0.035840 |
| 5236 | 33.3168 | 32.5526 | 0.050176 |
| 5237 | 33.6060 | 32.5901 | 0.034816 |
| 5238 | 32.6673 | 31.8931 | 0.051200 |

Frame 5237 の対応:

| 時点 | ms |
| --- | ---: |
| 直前 Frame 5236 の CPU 提出開始 / 終了 | 3201.5541 / 3201.6634 |
| Frame 5237 の DXGI 待機開始 | 3202.0468 |
| Frame 5236 の最後の GPU Timestamp（推定） | 3202.907868 |
| 1 回目 Timeout | 3215.5708 |
| 2 回目 Timeout | 3231.4360 |
| 表示 Event が Signaled で Return | 3234.6467 |
| Frame 5237 の CPU 提出開始 / 終了 | 3234.9663 / 3235.0718 |
| Frame 5237 の最初 / 最後の GPU Timestamp（推定） | 3235.253396 / 3235.288212 |

直前 Frame の Clock 投影差は 0.584 µs、当該 Frame は 2.904 µs。この標本では、描画 Pass が短いまま表示枠の待ちが続いている。描画命令の最適化だけでこの約 32.6 ms を解消できるという根拠はない。

DXGI の待機は 1 ms Timeout を指定しているが、実測 Return は 13.5240、15.8609、3.2052 ms。Timeout 指定は正確な CPU 再開時刻の保証ではなく、これを短くするだけの変更を改善とは扱わない。今回の区間には ETW がないため、Timeout 超過の内訳を Waiting と Ready に分割できない。

証拠: `NativeQ2-gpu-analysis.json`、`NativeQ2-gpu-frames.jsonl`、`NativeQ2-spans.csv`、`NativeQ2-gpu.csv`

## 2. GPU Pass より前の提出遅延と Fence

同じ NativeQ2 の Frame 5317 と、それを待った Frame 5319。

| 時点 | ms |
| --- | ---: |
| Frame 5317 / Fence 5318 の CPU 提出開始 / 終了 | 3353.4924 / 3353.5494 |
| Frame 5319 が Fence 5318 の CPU 待機開始 | 3354.5985 |
| Frame 5317 の最初 / 最後の GPU Timestamp（推定） | 3362.372644 / 3362.406436 |
| Frame 5319 の Fence 待機 Return | 3362.4885 |

- 提出終了 → 最初の GPU Timestamp: **8.823244 ms**
- GPU Pass 区間: **0.033792 ms**
- CPU Fence 待機: **7.890000 ms**
- 最後の GPU Pass Timestamp → CPU Fence 待機 Return: **0.082064 ms**
- Clock 投影差: **3.492 µs**

Frame 5318 も 3354.4413 ms に提出終了し、3362.410296 ms に最初の GPU Timestamp を記録。連続して提出した二つの Frame が、遅れてから短い間隔で進んでいる。

この標本は「長い Fence 待機だから Shader が重い」という説明を支持しない。一方、最初の Query より前の Native Command、Driver の投入、別 GPU Queue の競合、GPU Scheduling のどれかを、この CSV だけで決定することもできない。

証拠: `NativeQ2-longest-submit-gap.json`（対象 Frame と前後の CPU/GPU 記録を抽出）

## 3. ETW で取得できた表示待機

EtwQ2 の Frame 11170、Render TID 39116。

- CPU Render: 4.7740 ms
- DXGI.Event: 4.3426 ms = Running 0.0079 + Waiting 4.3304 + Ready 0.0043、Unknown 0
- 直前 Frame 11169 の最後の GPU Timestamp: 8968.639806 ms（投影差 0.744 µs）
- 表示待機 Return: 8972.9558 ms。直前の最後の GPU Pass Timestamp から約 4.316 ms 後

Kernel QPC:

1. `12302812038123`: Render が Waiting へ Switch Out
2. `12302812081260`: CPU 8 で TID 2392 へ Switch In
3. `12302812081427`: 同 CPU 上で Render TID 39116 の ReadyThread
4. `12302812081470`: Render が CPU 13 で再開（Ready 4.3 µs）

同 ETL の Process/Thread 一覧で TID 2392 は **dwm.exe PID 27836** と確認した。Ready Event の Header PID/TID は無効値だったため、CPU 上の直前 CSwitch と ETL 内の所属を使った。これは DWM の実行 Context 中の Ready 通知という観測であり、ISR/DPC 内での通知や DWM 内部の具体的な呼出元を Stack で分離した結果ではない。

証拠: `EtwQ2/Frame11170-Scheduling.json`、`ReadyCpuContext.json`、`Processes.txt:5005`、`on-q2-cpu-states.csv`

ETW 回の最長 CPU Render は一回目 4.9714 ms、再試行 5.2373 ms。いずれも GPU Pass 区間は約 0.03〜0.04 ms で、CPU の Waiting が大部分だった。約 33 ms の発生条件はまだ同じ ETW 記録内で取得できていない。

## 記録品質と検証

主計測四回の合計:

- CPU 生区間 **3,126,301**、GPU 生記録 **286,092**、いずれも容量超過による欠落 0
- GPU Calibration 71,588、CPU 提出 71,496、Pass 生標本 143,008、Native 取得失敗 / 無効 Pass 0
- 71,496 Frame / 142,992 Pass を対応付け。残る 16 Pass は各回の開始前 Warmup 2 Frame 分で、回収だけが計測期間に入る境界標本。未対応の理由を保持し、別 Frame へ付け替えていない
- CPU 区間・提出・Frame 進捗の件数は開始終了境界で一致しない場合がある。停止前に開始して停止後に終わった CPU Scope と、継続して回収した GPU 標本を同一母集団とはしない
- QPC ↔ steady の開始 / 終了 Offset 差は最大 50 ns。Calibration の投影差の最大は各回 519.924 / 248.796 / 81.616 / 141.060 µs。外れ値も捨てていない
- ETW 二回とも `process_trace=0`、Decode Error / Event Lost / Buffer Lost は 0
- ETW 内の CPU Marker 全 1,910,354 区間を元 CSV と照合。Clock Marker は別に 4 件
- CSwitch の未分類 State は全 Process 合計で 204 / 156 件を保持。今回示した対象区間の Unknown は 0
- 両 Collector は Kernel / User Session の停止と Merge 完了を確認

検証:

- `pwsh -NoProfile -File scripts/codex_build.ps1`: 既定 Debug Build 成功
- Trace ON / Development: `Cue.Foundation.TimingTrace`、`Cue.Renderer.DX12.MainFrameGraph` の 2 Test 成功
- Trace OFF / Debug: 上記に `Cue.Renderer.DX12.FrameGraphFrames` を含む 3 Test 成功
- 複数 Queue の 98 Pass × 2 Frame で、再利用・Shutdown 後の計 196 標本が提出元 Frame 100 / 101 を保持することを確認
- GPU 解析 3 Test、既存 CPU 解析 3 Test 成功。巨大 Tick の精度、Clock 差、測定境界、Frame 結合を確認
- `git diff --check` 成功

## 改善判断と残る対象

現段階では描画 Pass の高速化より、**表示枠待機**と**提出から最初の GPU Timestamp までの経路**を別に扱う方が調査対象に合っている。

- Waitable ON / Queue 上限 2 は今回も通常区間の FPS が高かったが、約 33 ms を防ぐ保証にはならない。Queue を増やす場合は入力から表示までの待ちが増え得るため、FPS だけで既定設定を決めない
- 表示待機を Input / Update の前へ移す設計は、準備済み入力 Snapshot が Render 内で待たされる時間を減らす候補。ただし表示 Event 自体の遅れを解消する処置ではなく、今回その変更は行っていない
- 約 33 ms の具体的な外部要因を決定するには、閾値超過の前後を同じ ETW に残す継続 / Trigger 記録が次の対象。固定秒数の再試行では再現しない場合がある
- 約 8.8 ms の提出後の区間は、同 Frame の GPU Scheduling / Driver Event と一緒に取得する必要がある。今回の Native 回にはそれらがなく、原因名を決め打ちしない
- Busy Wait 化、Timeout 値だけの短縮、Thread Priority の一律引き上げ、統計の間引きを、今回の結果から即座に採用する根拠はない
