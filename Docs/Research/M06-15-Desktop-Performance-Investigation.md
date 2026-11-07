# M06-15 / Issue #144 — デスクトップ PC の処理時間と改善候補

- 調査日: 2026-10-24（日本時間）
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- Source / Binary: [Desktop 基準測定](M06-15-Desktop-DoubleBuffering-Baseline.md)と同じ `ba1c485` / SHA256 `37C7DAC0B4B887E175008623528A8E060B8E4AA99A191E4B4F08BDE745CFA7F9`
- 実施: 保存済み全区間の再解析、Code 調査、詳細 Trace Sink の ON / OFF 対照、Win32 API の補助計測。
- Engine / Third-party Source、OS / Driver 設定は変更していない。Commit / Push / Merge は行っていない。

## 1. 結論

**Present / 提出 API に加えて、Main の ImGui Win32 処理と Render の Context 排他競合が重要。**
最初の 2,073 FPS は今回も有効な観測だが、同じ設定で必ず再現する固定性能値ではない。
追加の 10 秒試行では Trace ON が合算 1,273 FPS、OFF が 1,383 FPS となり、Main の
`ImGui_ImplWin32_NewFrame()` が約 15 µs から約 351 µs に増えた。
それに伴い Render 内の ImGui Mutex 取得区間も約 74 µs から約 406 µs に増えている。

Win32 Backend のマウス下 Window 照会が有力な切り分け先。
現在位置の補助計測では `WindowFromPoint` 平均約 247 µs を観測した。
ただし補助 Process の値であり、Engine Frame 内で API 単体を分離した原因確定ではない。

## 2. 最初の基準測定を重複なく分ける

Development、通常 UI、Double Buffering、Waitable ON、Queued 2、VSync OFF、Tearing ON、
1280 × 720、無制限、各 Warmup 2 秒 + 20 秒 × 3 回。採用 Frame は 124,400。
CPU Render Trace の 124,397 区間を、同じ Thread / Frame の子区間で分割した。
包含する子区間を再加算せず、各 Render の分類合計が親の経過時間に一致することを検査した。

| Render の経過時間内訳 | 平均 µs / Frame |
|---|---:|
| Present API | 162.788 |
| ImGui Mutex 取得区間 | 74.408 |
| Queue.Submit | 64.824 |
| DXGI 表示準備 | 63.323 |
| Graph 実行の残余（記録 / Submit などを除く） | 35.950 |
| Graph 記録の残余（ImGui 記録を除く） | 25.118 |
| ImGui Backend の描画記録 | 21.510 |
| Fence 待機 | 7.787 |
| その他 | 19.176 |
| **合計 CPU Render** | **474.885** |

これは実行再開の遅れを含む経過時間であり、CPU の純粋な演算時間ではない。
Main の約 131 µs や GPU Pass 全体約 61.6 µs は並行して進む別区間なので、この表へ加算しない。
Fence.Wait の呼出時平均は約 199 µs だが、発生回だけの条件付き平均である。
全 Render Frame に均すと約 7.8 µs であり、199 µs / Frame の負担とは扱わない。

### 実際の提出 API

GPU CSV の QPC 区間（終了境界の提出を含む 124,403 件）から以下を得た。

| API | 平均 µs | p95 µs | Max µs |
|---|---:|---:|---:|
| ExecuteCommandLists | 46.886 | 94.3 | 489.2 |
| GetClockCalibration（BeforeSubmit） | 5.663 | 7.4 | 267.1 |

Queue.Signal の平均は 5.116 µs、Queue / CommandPool の Mutex 取得待ちは各約 0.3 µs。
Submit の約 65 µs を Pool の Lock 削減だけで取り戻せるという根拠はない。

## 3. ImGui の競合と追加試行で変化した箇所

`ImGuiManager.cpp:765` の `record_draw_data()` はグローバル Context Mutex を取得してから
Backend 記録へ進む。Main は `begin_frame()` から `end_frame()` まで同じ Mutex を保持し、
Win32 NewFrame / ImGui NewFrame / UI Callback / Render をその中で行う。

基準試行の Record 内 Mutex 取得は平均 74.035 µs、p95 158.3 µs、最大 1,081.6 µs。
10 µs を超える 113,094 件中、113,044 件が Main の所有区間の近似と重なった。
取得区間合計 9.2097633 秒のうち、Main の取得完了から ImGui.End 終了までの包絡との重複は
7.5017514 秒（81.45%）、UI Callback との重複は 5.7395254 秒（62.32%）。
所有区間の末端には微小な解放後処理も含むため、厳密な Native Mutex 保持時間とは区別する。
残余には Scheduling / 他の排他操作を含み得る。

### 同一 Binary の Trace Sink ON / OFF 対照

通常 UI、120 標本の平均 / p95 / Max / 件数、GPU Query は維持し、Sink だけを切り替えた。
各 Warmup 2 秒 + 10 秒。順序は OFF → ON → ON → OFF → OFF → ON。

| 組 | Trace OFF FPS | Trace ON FPS |
|---|---:|---:|
| 1 | 1,414.482 | 1,202.988 |
| 2 | 1,341.265 | 1,285.554 |
| 3 | 1,393.569 | 1,330.566 |
| **合算** | **1,383.106** | **1,273.036** |

6 試行とも Exit / Hash / 前面 / Size / 可視状態 / Snapshot 追加確保を検査した。
Trace ON は生 CPU 連番、欠落 / GPU エラー Status も検査し、異常はなかった。
OFF は詳細 CSV を生成しない条件であり、OFF の生 Trace 欠落を検証したという意味ではない。
今回 OFF は ON より約 8.65% 高かったが、実装改善率や固定の観測負担とはしない。
Mouse Hover 対象・Window 配置・OS Scheduling を全 Frame で固定 / 記録した比較ではない。

| 包含区間の平均 µs | 最初の Trace ON 基準 | 追加 Trace ON |
|---|---:|---:|
| CPU Render | 474.885 | 777.898 |
| CPU Main | 131.401 | 471.944 |
| ImGui.PlatformNewFrame | 15.438 | **350.626** |
| UI Callback | 57.315 | 58.641 |
| Render 内 ImGui Mutex 取得（分割後） | 74.408 | **406.088** |
| Present API | 162.789 | 156.711 |
| ExecuteCommandLists API | 46.886 | 46.828 |

後半の遅さを Present の悪化と説明する根拠はなく、UI Callback 自体の負担もほぼ同程度。
大きく変化したのは Win32 側処理と、それを含む Context 排他競合である。

### WindowFromPoint の補助計測

固定 Version の Win32 Backend は毎 NewFrame で Mouse Data を更新し、その中で
`WindowFromPoint` を呼ぶ。Multi-Viewport を無効にしているだけでは、この呼出しは外れない。
[公式 v1.92.9b-docking Source](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/backends/imgui_impl_win32.cpp#L333-L379)

マウス位置を操作せず、別の補助 Process から 100 回 Warmup + 各 1,000 回を測った。
開始 / 終了の座標と対象 PID は同じで、Hover 対象は Explorer だった。

| API | 平均 µs | p95 µs | 最大 µs |
|---|---:|---:|---:|
| GetCursorPos | 0.489 | 0.6 | 0.7 |
| GetForegroundWindow | 0.026 | 0.1 | 0.1 |
| GetClientRect | 0.046 | 0.1 | 0.1 |
| **WindowFromPoint** | **246.710** | **759.6** | **2,050.2** |

これは有力な再現候補だが、追加試行中の Mouse Hover 対象を保存しておらず、
実 Frame 内で他の Backend API や実行再開遅れを除去した測定でもない。
XInput / Monitor 再列挙などを今回の原因と断定する証拠もない。
次は実 Frame の API 内訳または CPU Stack と、Hover 対象を固定した比較で確かめる。

## 4. スパイクは平均の課題と分ける

基準の最大 Render は `20261024-181714/Trial3` Frame 21144、6.3501 ms。
同じ Frame の DXGI.Ready が 5.6707 ms（約 89%）、Present が 0.2733 ms。
その Frame の GPU Pass 全体は生 Tick 差で 0.060416 ms。

追加 Trace ON の最大 Render は Pair1 Frame 4450、4.0892 ms。そのうち Fence.Wait が 3.3318 ms。
従って全てのスパイクを同じ原因としない。GPU Queue 完了、表示許可の通知、Thread の実行再開を
同じ Frame で照合する必要がある。短い GPU Pass だけで、GPU 開始前の Scheduling 遅延まで否定しない。

## 5. 改善候補の順序

| 優先度 | 候補 | 確認できた根拠 / 次の検証 |
|---|---|---|
| 1 | Win32 NewFrame 内の高価な照会を特定 | 追加試行で約 351 µs。WindowFromPoint が有力。実 Frame 内 API または Stack、Hover 対象と Window 位置を固定した対照を最初に行う |
| 2 | OS 入力収集と ImGui Context 反映の境界を見直す | 長い OS 問い合わせを Context 排他中に行うと Render に波及する。入力・Hover・Capture・Focus・DPI・Viewport の正しさを維持できる範囲を設計する |
| 3 | 性能表示の文字列整形を Context 排他外へ移す | EditorHost.cpp:22 の ImGui::Text が数値整形を行う。再利用 Buffer へ事前整形して TextUnformatted を使う候補。Callback 約 57～59 µs 全部が整形時間ではなく、効果は未測定 |
| 4 | Timestamp Frequency の Queue 単位保持 | MainFrameGraph.cpp:416、QueuePool.cpp:60 / 434 で繰り返し取得。API 単独費用は未分離。Queue 再生成時の無効化と、Copy Queue Timestamp 非対応時の従来動作を守る |
| 5 | 小さいコピー・重複準備の整理 | WindowsHost.cpp:1212 の TimingSamples 5 本コピー、ImGui publish / record での prepare。Editor.Statistics 全体約 6.3 µs、TexturePrepare 約 4.7 µs / Frame なので大幅改善を保証しない |
| 6 | Completion の毎 Frame 確保を整理 | FrameGraph.cpp:262 の make_shared と CommandPool.cpp:678 の make_unique は現経路でも発生。費用は未測定。古い Token と Resource が参照する Fence 値・寿命を変えない |

単純な Context Mutex 削除は、TLS でない公式 GImGui の並列利用を壊すため行わない。
Context / Backend の分離はより大きい設計変更で、まず OS 照会と文字列整形の結果を測る。
Pool Lock 短縮と Worker 通知先分割は、停止 / Drain / 取消 / 失敗回復を含む変更となる。
今回の平均 Lock 待ちや単一 Queue 構成から、これらを最初に改造する根拠は弱い。

Frequency は Queue ごとの値であり、Copy Timestamp は対応が任意である。
Copy の取得成功を全 Queue 生成の必須条件にしない。
[Microsoft Timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)

## 6. 証跡と実施範囲

Git 管理外の `out/validation/Performance20261024/Desktop144/`:

- `InvestigateStages.py` / `StageInvestigation.json`: 全 Render の重複なし内訳と同一 Frame の最大事例。
- `RunTraceControl.ps1` / `TraceControl-20261024-184455/`: 追加 6 試行の起動・終了・Hash・生記録。
- `SummarizeControl.py` / 同 Folder の `ControlAnalysis.json`: API QPC 区間、対照条件の検証と合算。
- 同 Folder の `StageInvestigation.json`: 追加 Trace ON 3 試行の重複なし内訳。
- `ProbeWin32Calls.ps1` / `Win32CallProbe.json`: 読み取りのみの Win32 API 補助計測。

詳細 CPU Trace は QueryThreadCycleTime、CPU 番号、Clock、記録処理の観測負担を持つ。
この負担は通常 UI / GPU Query / 120 標本統計と区別して扱う。
Trace Sink OFF は Compile 時から Trace OFF の完全な製品構成と同一ではない。

Engine Source の変更がないため、今回追加 Build / CTest は行っていない。
ETW / PIX / Nsight の新規 Capture は行っていない。OS 内部と実 Frame の API 切り分け、
改善候補の実装 A/B、入力遅延の比較は未実施。計測アプリは全て終了し、Configure Cache は Trace OFF を維持。
表示経路は追加試行後も 2 台とも RTX 3060 だった。各試行の Window が使ったモニターと Hover 対象は
固定・保存していないため、後続の厳密な比較ではそれらも条件へ加える。
