# M06-15 Scheduling / GPU Fence / DWM の追加調査

> 続報: [Waitable 待機の条件確認と再測定](M06-15-Waitable-Latency-Followup.md) で前面状態を確認した比較と 20 秒の ETW を取得した。約 50 ms は再現せず、最長 10.73 ms の DWM からの復帰を追跡した。GPU 内部の分離は未達で、その範囲も記録した。

- Date: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 前報: [Frame Spike の ETW 追跡](M06-15-Frame-Spike-ETW-Investigation.md)
- 状態: 調査用 Script と記録を追加。Engine の Thread / GPU 同期方針は変更していない。未 Commit / 未 Push / 未 Merge

## 今回確認できたこと

| 要因 | 同一区間での観測 | 判断できる範囲 |
| --- | --- | --- |
| CPU の優先度 Scheduling | Render の優先度 8 から優先度 9～13 の別 Thread へ 33 回切替 | 約 7.98 ms の Ready を含む Frame には、高い優先度の Thread による中断がある |
| GPU Fence | 同じ同期 Object / Fence 値の Signal Packet 完了記録から CPU Wait 復帰まで約 5～8 µs | 選択した Spike の大半は Signal Packet が未完了の期間。完了後の CPU 復帰が数 ms 遅れた例ではない |
| Waitable Presentation | DWM の PresentHistory 処理に続き、DWM Thread が Render を Ready にする | 選択した Waitable Spike は DWM を経由する表示枠の通知待ち。GPU Pass の演算時間だけでは説明できない |

これらは別々の Frame の事例であり、時間を合算しない。全ての Spike が同じ原因という結論でもない。

## 1. 約 7.98 ms の CPU 割り当て待ち

Run2 / Waitable OFF / Frame 1982 / Render TID 2960 を、取得済み ETL の CSwitch / ReadyThread と xperf の優先度 Decode で追跡した。

- Render 全体: 11.8211 ms
- Running: 3.2836 ms、Waiting: 0.5589 ms、Ready: 7.9786 ms、Unknown: 0 ms
- Switch Out: 34 回
    - Ready / WrDispatchInt: 31 回
    - Ready / WrPreempted: 2 回
    - Waiting / UserRequest: 1 回
- Ready の 33 回の Switch Out 全てで、Old Priority は 8、New Priority は 9～13
    - 8 → 9: 17 回
    - 8 → 11: 12 回
    - 8 → 10: 2 回
    - 8 → 12 / 13: 各 1 回
- Ready は 34 区間に分かれる。最長は Fence 待機解除後の 1.8486 ms。別の中断では 1.5555 ms がある

切替先には Service、RuntimeBroker、Browser の Thread が現れる。この記録では、Render の実行可能状態を保ったまま高い優先度の Thread が選択されている。これは Windows の優先度 Scheduling の動作と一致する。[Scheduling Priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)

最後に実行していた CPU の Ready 中の実行者も集計した。ただし Render は複数 CPU に移動しており、その CPU の実行者だけで「なぜ他の CPU にも配置されなかったか」は説明できない。Process 別の重なり時間を、因果関係やシステム全体の使用率とは扱わない。

### DPC / ISR

同じ Frame と重なる DPC / ISR の最大単発は約 0.346 ms の `dxgkrnl.sys` DPC。最大の集計は CPU 1 の約 2.205 ms だが、この Frame の Render は CPU 1 では実行されていない。

各 CPU の時間は同時に進むので足して Frame の内訳にしない。ISR / DPC の影響がゼロという結論ではないが、「単発の約 8 ms の DPC が原因」という説明はこの記録では支持されない。33 回の優先度切替は直接観測できている。

Running は ISR / DPC を含み得る。DPC / ISR の Timestamp と ElapsedTime から区間を切り出し、Module を識別した。[DPC Event](https://learn.microsoft.com/en-us/windows/win32/etw/dpc)

## 2. GPU Provider の不足を修正

前報の modern DxgKrnl `0x800` と Run1 の `0x900236` は、QueuePacket / DmaPacket に必要な Base bit `0x1` を含んでいなかった。legacy DX Provider の指定を併用しても、対象 Window の Queue / DMA は取得できなかった。

今回は modern Provider 自体へ Base / Resource / LongHaul / GPUScheduler / Diagnostics を含む `0x408841` を指定した。Intel PresentMon の Event 定義でも QueuePacket / DmaPacket に Base が含まれることを確認した。[PresentMon の DxgKrnl Event 定義](https://github.com/GameTechDev/PresentMon/blob/main/PresentData/ETW/Microsoft_Windows_DxgKrnl.h)

これにより Run3 では次の Event を取得した。起動・Warmup・停止を含む全 Session の件数であり、測定 Frame 数とは混ぜない。

- QueuePacket Start / Stop: 各 208,764 件
- DmaPacket Info: 133,910 件、Start / Stop: 各 4,130 件
- 対象 Process の Device → Context → HwQueue / ParentDxgHwQueue を辿って対応付けた QueuePacket: 191,791 組
- 同じ Context / SubmitSequence の開始重複、対応付け対象の未完了 Start: 0 件

完了 Event は別 Process / Kernel の実行中に記録されることがあるため、Header の PID だけで GPU Packet の所有を決めていない。HwQueue の Process 内 Handle は再利用され得るので、選択した Fence の照合には Kernel Context、同期 Object、Fence 値、SubmitSequence を使った。

### Run3 の品質と条件

- Development / Buffering 2 / 実 Window 1280×720 / 通常 UI / FPS 無制限 / VSync OFF / Tearing ON / GPU 未完了上限 2
- OFF、ON・Queue latency 1 を各 Warmup 2 秒 + 測定 3 秒
- Collector のみ管理者権限。既存の Session は停止しない
- 全区間 Marker を維持し、今回は CSwitch / ReadyThread の Stack 採取を省略した。Frame の平均 / p95 / Max / 件数と更新頻度は変更していない
- OFF: 3,052 Frame / 132,276 区間、ON: 1,824 Frame / 76,945 区間
- 209,221 区間の ETW / 生 CSV 照合が成功。別に Clock Marker 4 件
- ETW Event / Buffer 欠落 0、Probe dropped 0、対象の Decode Error 0
- CSwitch 651,501 件、ReadyThread 385,463 件。未分類 State 102 件は元の値を保持し Unknown として扱う
- Clock 読取り幅からの不確かさは各 ±50 ns。xperf の Event 時刻には µs 単位の丸めがある
- Background 負荷、記録 I/O、解析処理との競合を排除した記録ではない。Run3 の FPS を非計測実行や Run2 との改善率に使わない

ETL の User Session と Merged Session は開始時刻が異なる。独自 ClockSync の同じ Header QPC と xperf Timestamp を照合し、それぞれの時刻原点を求めた。異なる ETL の相対時刻をそのまま比較していない。

## 3. Fence 待機と Signal Packet の照合

OFF の Render TID 2756 が待った同期 Object `0xffffca85da5d6c10` を、WaitForSynchronizationObjectFromCpu と Signal Packet に照合した。

| 対象 Frame | Fence 値 | CPU の Wait | 同じ Fence の Signal Packet 全期間 | Packet 完了記録 → CPU Wait 復帰 |
| --- | ---: | ---: | ---: | ---: |
| 3552 | 3551 | 2.6991 ms Timeout + 0.3883 ms Signaled | 3.829 ms | 約 5.4 µs |
| 4084 | 4083 | 4.8315 ms Signaled | 5.531 ms | 約 7.6 µs |

時刻は Run3 User Session 相対 µs。最大でも約 1 µs の丸め差を含む。

- Frame 3552: Signal submit 4,549,773 → Stop 4,553,602。CPU Wait の最後の復帰は 4,553,607.4
- Frame 4084: Signal submit 5,083,113 → Stop 5,088,644。CPU Wait は 5,083,820.1 → 5,088,651.6

後者の Wait 内は Running 0.0098 ms、Waiting 4.8172 ms、Ready 0.0045 ms。GPU 側の Signal Packet が未完了である期間が待機の大半を占める。

ただし Signal submit → Stop は、GPU 演算、Queue 滞留、依存同期、Driver / Scheduler の処理を含む。**GPU が約 5 ms 分の Shader を実行したと判断できる値ではない。** この内側の分離は未完了で、GPU の命令実行時間や特定 Driver の不具合は断定していない。

## 4. Waitable の復帰は DWM 経由

ON・Queue latency 1 / Frame 1266 / Render TID 42764:

- DXGI Event: 5.3180 ms、Waiting 5.2837 ms、Ready 0.0054 ms
- 同じ Process の PresentHistoryDetailed Start と DWM の Stop を、Token と PresentCount 6792 で照合
- Merged ETL の ReadyThread では **dwm.exe TID 2392 が Render TID 42764 を Ready にしている**

User Session 相対 µs の順序:

| Event | 時刻 |
| --- | ---: |
| Render の Waitable 待機開始 | 8,765,141.75 |
| 対象 Process の PresentHistoryDetailed Start | 8,765,168 |
| 対象 Present の Software QueuePacket Stop | 8,767,208 |
| DWM の対応 PresentHistory Stop | 8,770,444 |
| DWM → Render ReadyThread | 約 8,770,451.1 |
| Render が CPU に Switch In | 約 8,770,456.1 |
| CPU の Waitable 待機復帰 | 8,770,459.75 |

対象 Present の Packet 完了記録から DWM の履歴処理まで約 3.24 ms あり、Ready 後の CPU 割り当て待ちは約 5 µs。この例では Present の提出・処理と DWM の表示枠解放が異なる時点になっている。

この時系列から、Waitable の長い経過時間を ImGui の描画負荷や CPU の Ready 遅延だけに帰すことはできない。Refresh / Scanout との厳密な対応までは解析しておらず、「必ず 60 Hz に制限される」という意味ではない。

## 5. ETW を出さない比較

同じ Development Probe / Buffering 2 / 通常 UI で、ETW 出力を OFF、全区間 CSV は ON のまま比較した。各 2 秒 Warmup + 3 秒測定。比較中に別の解析や Build は実行していないが、他の Background Process と Foreground の状態は固定していない。

実行順も残し、条件による差と時間経過による変動を混ぜて断定しない。

| 実行順 / 構成 | FPS | Render 平均 | Render p95 | Render Max |
| --- | ---: | ---: | ---: | ---: |
| 1. OFF A | 2,451.042 | 0.4017 ms | 0.7177 ms | 14.4539 ms |
| 2. ON・latency 1 A | 1,524.323 | 0.6467 ms | 0.7700 ms | 4.0388 ms |
| 3. ON・latency 2 A | 1,161.621 | 0.8536 ms | 3.3293 ms | 49.8098 ms |
| 4. ON・latency 2 B | 1,073.771 | 0.9228 ms | 3.9066 ms | 49.6636 ms |
| 5. ON・latency 1 B | 650.432 | 1.5308 ms | 4.6932 ms | 6.1597 ms |
| 6. OFF B | 1,126.327 | 0.8822 ms | 4.0387 ms | 6.0635 ms |

各実行の Probe dropped は 0。Render の分布は全ての完了区間から p95 を nearest-rank で算出した。境界をまたぐ区間は採取対象外なので、Console の Frame 数と区間数が 1 件異なる場合がある。FPS は Console の測定期間全体の値であり、Render 平均の逆数ではない。

### 解釈

- この設定で 650 FPS が固定上限という説明は成立しない。今回も ETW なしで 2,000 FPS 超、Waitable ON で 1,500 FPS 超の実行がある
- 同じ設定でも差が大きい。ETW を止めるだけで安定してこの速度になるという保証や、変更による改善率は出せない
- Queue latency 2 が有利という結果ではない。約 50 ms の Frame が両実行で発生した
- latency 2 A の最大 Frame 2740 は、DXGI Event が 5.6755 / 15.5975 / 15.3393 ms の Timeout と 12.3536 ms の Signaled。合計 48.9659 ms を占める
- latency 2 B の最大 Frame 5443 も DXGI Event の合計が 48.9529 ms。Render 全体 49.6636 ms の大半を占める
- この比較には Kernel ETW がないため、これら約 50 ms の Waiting / Ready の内訳は未分類。Run3 の Frame 1266 と同じ原因とは断定しない

## 変更・検証・残る課題

変更したのは `capture_spike_etw.ps1` の Provider 指定と、Stack を任意指定する調査設定、および本記録。Engine の Priority、Affinity、GPU 再利用条件、Waitable の方針、統計標本は変更していない。`-Stacks` を付けた場合だけ Kernel の CSwitch / ReadyThread Stack を採取する。

- 指定の `pwsh -NoProfile -File scripts/codex_build.ps1`: Debug Build 成功
- 関連 Debug Test 2 件成功: TimingTrace / InputLatency
- Development Exporter を最終 Source から Build し、Run3 の欠落検査と全区間照合が成功
- 通常の CMake 設定は `CUE_ENABLE_TIMING_TRACE=OFF`
- マージ等の Git 操作は行っていない

改善の判断では、CPU の優先度競合、GPU Signal Packet の未完了時間、DWM の枠解放待ちを分ける必要がある。現状の根拠で、Worker を OFF にする、Queue latency を 2 にする、Fence 待機を外す、といった一律の変更は提案しない。

残る調査範囲は、GPU Signal submit → completion の内側と、latency 2 の長い DXGI 待機。安定した性能比較には、Foreground / Background と記録負荷を揃えた長めの反復計測が必要になる。

元データは `out/validation/Performance20261019/Etw144/Run3/`、追加解析と ETW なし比較は `out/validation/Performance20261019/Etw144/Followup/` に保持した。`SchedulingDetails.json`、`GpuDetails.json`、`Comparison/Summary.json` と各 CSV / Log、元 ETL を参照できる。
