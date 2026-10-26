# M06-15 Frame Spike 調査

> 続報: UAC 許可後の [ETW 追跡](M06-15-Frame-Spike-ETW-Investigation.md) で Kernel Scheduling を取得し、Waiting と Ready の異なる Spike を確認した。以下の ETW 未取得の記述は、管理者記録前の調査時点の状況。

- Date: 2026-10-19
- Issue: #144
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 状態: ローカルで調査と計測由来の待機を修正。Commit / Push / Merge は行っていない

## 結論

数 ms の CPU Spike は再現した。主な区間は GPU Fence / DXGI Event の待機であり、最小 UI でも残る。平均と Max の比だけで原因を判断せず、同一 Frame の開始・終了を照合した。

また、前回の入力計測で追加した Present 後の `was_frame_recorded()` が ImGui Context Mutex を再取得していた。最小 UI でこの取得に 6.1263 ms を要した事例があり、計測のために Render 完了を遅らせていた。この再取得は除去した。

全ての Spike を修正した結論ではない。Kernel ETW は管理権限不足で取得できず、GPU / Driver / OS Scheduling の内訳は未確定である。

## 計測方法

- Development、実 Window 1280×720、FPS 無制限、VSync OFF、Tearing ON、SwapChain 2 枚
- GPU 未完了上限は Buffering と同数。Worker の導出方針は従来どおり
- 起動後 2 秒の Warmup、各実行 3 秒または 4 秒。Snapshot と保存設定は実行ごとの専用 Storage へ分離
- 既定の Test / 性能設定 UI と、文字列一つだけの最小 UI を比較
- 全ての完了区間を Thread ごとの固定容量配列へ記録。File 出力と集計は Host 停止後
- 区間名、論理 Frame ID、Native Thread ID、開始・終了の単調時刻、Thread Cycle、実行 CPU、Fence 値、Native Wait 戻り値を保存
- Frame 採用前の開始許可 / GPU 待機には架空の Frame ID を付けず、`UINT64_MAX` を未割当として記録
- Trace は計測 Window 内で開始・終了した区間を対象とし、境界をまたぐ区間は除外。容量不足による欠落は別に件数を報告し、上書きしない
- 元の直近 120 標本の平均 / p95 / Max / 件数と更新頻度は変更していない
- 調査 CSV の分布は採取した全区間から算出。UI の直近 120 標本や、異なる項目の Max と混ぜない

`QueryThreadCycleTime` は Thread が消費した Cycle の補助指標として使用し、ms へ換算しない。低い Cycle 数で長い経過時間になっている事例は待機・実行中断の手掛かりになるが、それだけで Scheduling 原因を断定しない。[Microsoft の API 契約](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)

## 同一 Frame で確認した内訳

以下は修正前の調査 Trace であり、異なる Frame の Max を足していない。

| 実行 | Frame | Render | その Frame の長い区間 |
| --- | ---: | ---: | --- |
| `b2-off-full-trace1` | 3294 | 5.6577 ms | Fence Event 5.4370 ms、戻り値 `WAIT_OBJECT_0` |
| `b2-on-q1-full` | 2057 | 12.7516 ms | DXGI Event 12.0166 ms、戻り値 `WAIT_OBJECT_0` |
| `detail-off-full` | 3353 | 15.6004 ms | Fence Event 5.0006 ms (`WAIT_TIMEOUT`) + 9.9634 ms (`WAIT_OBJECT_0`) |
| `detail-off-minimal` | 4707 | 6.5717 ms | Present 後の `ImGui.VerifyFrame` 6.1289 ms、そのうち Mutex 取得 6.1263 ms |

Fence / DXGI の Native Wait へ渡す Timeout は 1 ms だが、観測した経過時間の上限は 1 ms になっていない。例えば `detail-off-full` の Frame 3353 では最初の待機が約 5 ms 後に Timeout として復帰し、次の待機が約 10 ms 後に通知として復帰した。

これは「GPU の Pass が約 15 ms の演算をした」ことを示す値ではない。完了通知までの時間と、Thread が復帰して時刻を記録するまでの時間を含む。待機 Timeout の精度は System Clock の分解能にも依存する。[Wait Functions](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions)

## 計測処理の余分な Mutex 取得を修正

### 発生したこと

`detail-off-minimal` の Render Frame 4707 は Present 後に `was_frame_recorded()` を呼び、Main の Frame 4708 が使う Context Mutex の取得を待った。

Main の `ImGui.Build` は Render の Lock 待機開始から約 0.389 ms 後に終わっている一方、Render の Mutex 取得は約 6.126 ms 後だった。したがって、この値の全てを「Main が Lock 内で処理した時間」とは扱えない。再競合や復帰 Scheduling の内訳には OS Trace が必要となる。

### 修正内容

- `ImGuiManager::render_frame()` が既に保持している Lock 内で、その Snapshot の実記録結果を呼出側へ返す
- `EditorHost` はその結果と Frame ID を同じ Render Thread で保持し、Present 後の入力計測へ渡す
- 後から Context Mutex を再取得する `was_frame_recorded()` は削除
- 実記録成功 / Skip / Error を区別する。失敗時や未描画の入力を成功した遅延として集計しない
- GPU Resource の再利用条件、実描画時の Context 排他、Buffering / Worker / Queue 設定は変更しない

実 GPU / WARP の転送 Test に、成功時 true、取消時 false、Error 時 false の確認を追加した。既存の入力遅延 Test でも Buffering 1 / 2 / 3 × Waitable ON / OFF、未描画、Consumer 不在、入力繰越を検証する。

## 修正後の再計測

4 条件を各 4 秒計測し、Trace 欠落は全て 0。`ImGui.VerifyFrame` は全 Trace で 0 件となり、該当する再取得経路がなくなった。

| 実行 | Render 標本数 | Render 平均 | Render Max | ImGui Mutex 取得 Max |
| --- | ---: | ---: | ---: | ---: |
| `fixed-off-full` | 4551 | 0.873 ms | 5.246 ms | 0.152 ms |
| `fixed-on-q1-full` | 2609 | 1.525 ms | 6.160 ms | 0.149 ms |
| `fixed-off-minimal` | 4293 | 0.925 ms | 8.202 ms | 0.248 ms |
| `fixed-single-off` | 4139 | 0.259 ms | 0.740 ms | 0.076 ms |

Mutex Max はそれぞれ独立の Frame の値であり、Render Max の内訳としては扱わない。上記は一回ずつの観測で、全環境の上限や改善率を保証しない。

残る長い区間も確認した。

- `fixed-off-full` Frame 5549: Render 5.2456 ms。Fence Event が 0.3158 + 4.6454 ms
- `fixed-on-q1-full` Frame 3663: Render 6.1596 ms。DXGI Ready が 5.5124 ms、Graph Execute が 0.3107 ms、Present API が 0.3154 ms
- `fixed-off-minimal` Frame 2396: Render 8.2018 ms。Fence Event が 7.0806 ms、Graph Execute が 0.4379 ms、Present API が 0.6157 ms
- Single の GPU 開始許可待機は Main / Render の外側にあるため、Render Max が 0.740 ms でも Frame interval Max は 6.018 ms。待機を計測区間の外へ動かして速くなったと解釈しない

GPU Pass は終了時の直近 120 完了標本だけを確認した。修正後の Buffering 2 の既定 UI では ImGui の平均約 0.027 ms / Max 約 0.031～0.043 ms、Clear は平均約 0.002 ms。これは CPU Trace 全区間の GPU 統計でも、特定の Spike Frame と対応した GPU 統計でもない。

## UI 負荷と確保の確認

- 詳細計測では `ImGui_ImplWin32_NewFrame` にも約 1.43 ms の事例があった。Win32 API、Driver、Scheduling のどれが寄与したかは、この区間の計測だけでは確定しない
- 公式 Allocator Hook で ImGui の確保を観測した。`detail-off-full` は 3 回、最大の確保区間は 0.0041 ms。Snapshot 容量拡張は同じ実行で 1 回あった
- 他の詳細計測 3 回では ImGui 確保と Snapshot 拡張は 0。最小 UI でも数 ms の待機 Spike があるため、毎回の ImGui 確保だけを主因とする根拠はない
- この Hook は Engine / CRT 全体の確保を数えるものではない
- UI 構築と描画記録の Context 共有は残る。`ImGui.Record` 内の Lock 待機が次 Frame の UI 構築に重なる事例もあり、将来の排他範囲の検討対象となる

## 残る調査と改善候補

1. **Kernel ETW / GPUView で起床と GPU Queue を照合する**。Context Switch、Ready 状態、DPC / ISR と GPU 実行を確認し、通知自体が遅れたのか、通知後の CPU 再開が遅れたのかを分ける
2. **1 ms Timeout の周期的再確認を減らす設計を評価する**。Stop / Close 用の通知を Fence / DXGI Event と一緒に待つ案がある。ただし通知で復帰したケースにも Spike があるため、Timeout を変えるだけで解消すると判断しない
3. **実描画時の Context 排他を短縮する**。公式 Backend が参照する共有状態を整理してから行う。Snapshot があるという理由だけで Mutex を外す変更はしない
4. **表示経路・電力状態を管理した比較**。Waitable、Queued presents、GPU Clock / 温度 / 電力、Present Mode を管理して交互に測る

`xperf -on PROC_THREAD+LOADER+CSWITCH+DPC+INTERRUPT` は `NT Kernel Logger: Access denied (0x5)` だった。他の既存 Trace Session は停止していない。Kernel Trace が未取得のため、特定 Driver や OS 割込みを原因とする結論は出していない。[GPUView の記録対象](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/using-gpuview)

## 計測の影響と制限

- Trace の時刻取得、Cycle 取得、配列書込みには Cost がある。通常 Build では `CUE_ENABLE_TIMING_TRACE=OFF` とする
- Sink 不在の対照実行は 1128.8 / 1127.4 FPS、交互の Trace 実行は 1125.3 / 1361.6 FPS。同じ設定でも変動があり、この比較から Trace 負荷の厳密な上限を決めない
- 詳細計測では同じ Waitable ON / Queued presents 1 でも約 1600 / 649 FPS と変動した。FPS 差全体を今回の修正に帰属させない
- 入力の物理時刻、OS Queue 投入時刻、GPU 完了時刻、光学的表示時刻は今回の CPU Trace からは取得していない

## 再現方法と成果物

```powershell
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=ON
cmake --build --preset windows-vs2026-development --target CueSpikeTimingProbe CueTimingTraceTests
ctest --preset windows-vs2026-development -R '^Cue.Foundation.TimingTrace$'
# prefix は存在する出力 Directory 配下の絶対 Path とする
out/build/windows-vs2026/Engine/Tests/Development/CueSpikeTimingProbe.exe 2 1 1 0 4 C:/Temp/CueSpike 1
python scripts/analyze_spike_trace.py C:/Temp/CueSpike-spans.csv
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=OFF
```

Probe 引数は `buffers waitable queued ui seconds prefix traceEnabled`。`ui=0` は既定 UI、`ui=1` は最小 UI、`ui=2` は空 UI。`traceEnabled=0` は同じ Build の Sink 不在対照実行となる。

ローカル成果物は `out/validation/Performance20261019/Spikes144/` に保存した。Git 管理外のため Repository の Clone には含まれない。

- `b*-spans.csv`: 初回計測、`detail*-spans.csv`: ImGui 内部区間と確保の詳細、`fixed*-spans.csv`: 再取得除去後
- `*-statistics.csv`: 全採取区間の平均 / p95 / p99 / Max / 件数、`*-worst.json`: 同一 Frame の内訳と他 Thread との重なり
- `*.log`: FPS、CPU 使用量、Snapshot 拡張、Trace 欠落件数、終了時の GPU 直近標本、Build / Test 結果
- `BeforeTrace/`、`BeforeInputFix/`: この作業開始時と再取得修正前の差分照合用 Source。既存の未コミット変更を保全
- `FinalTraceSourceHashes.csv`: 詳細計測前の主要 Source、`AfterInputFixSourceHashes.csv`: 修正後の主要 Source

## Build / Test

- Trace 有効の Development: Probe Build 成功、Trace / 入力遅延 / ImGui DX12 実 GPU / WARP の 4 Test 成功
- 指定の `pwsh -NoProfile -File scripts/codex_build.ps1`: Trace OFF の最終 Debug Build 成功
- 最終 Debug 全 53 Test の実行では 52 件成功、`Cue.WindowsHost.Lifecycle` が一度 10 秒の Timeout。単独再実行は 5 回連続成功（7.26～7.51 秒）。Timeout の原因は未確定であり、全体初回の失敗を記録に残す
- 再取得修正前の Trace OFF Debug 全 53 Test は成功している。Lifecycle Test の Timeout 値は変更していない
- 最終設定は `CUE_ENABLE_TIMING_TRACE:BOOL=OFF`。`FinalDebug*.log`、`LifecycleRetest.log` に検証結果を保存
- 性能値を CTest の速度合否には使用しない
