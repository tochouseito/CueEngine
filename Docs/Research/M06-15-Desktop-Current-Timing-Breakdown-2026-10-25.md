# 現行CueEngineの全期間時間内訳（性能回復後）

2026-10-25。ユーザー許可の NVIDIA オーバーレイ ON→OFF→ON 比較後、Overlay と Game Filters の元の ON 復帰を UI で確認し、現在版を 10 秒 × 2 回計測した。対象は `out/validation/Performance20261025/FpsCause/OverlayCause-20261025-230630/CurrentTiming/`。以下は保存済み全期間 CSV の集計。Engine 実装・Git・Defender 設定は変更していない。

## 条件と全体値

- Current Binary SHA256 `992144D34C321A862B75B1F907C607AD160B24D12F2013B9DC914F5EC3921266`。同じ保存Current Binaryで、Engine修正なし。
- Development、RTX3060、Client1280×720枠付き、通常Test UIと性能統計、ClearFinalColor + ImGui。
- Buffering2、Worker ON、未完了GPU / SwapChain / Queued Presents各2、WaitableON、VSyncOFF、TearingON、FPS上限なし。
- Window `(20,250)`、Pointer `(1546,88)` はアプリ外。両試行のLauncherは `DISPLAY2` を記録。
- Warmup2秒 +測定10秒を二回。FPSは2121.470 /1907.419。外部CPU累積採取OFF、500ms NVIDIA telemetryはON。
- CPU / GPU全記録はRAMへ保持し、測定終了後に保存。早期File書込0、Trace欠落0、GPU欠落・上書き0、Window / Pointer変化0、前面・可視・非最小化・Client寸法成立。
- NVIDIA OverlayとGame Filtersは元のONへ復帰確認済みの後の計測。設定OFFでもNvSPCapロードが残ったこと、操作前ON状態でも既に高速復帰していたことから、今回の回復をOverlay無効化の効果とは扱わない。

**合算 40,293 Frame / 20.002141秒 = 2014.434 FPS。** CPU Render完了間隔は平均0.496357ms、p95 0.781100ms、最大9.204100ms、40,289標本。二試行の間隔は混ぜていない。

## 最初に読む内訳

- CPU Render全体は約0.486ms。大きい順にPresent約0.154ms、Command記録約0.130ms、GPU完了Fence約0.081ms/Render、Queue提出約0.061ms。
- Mainの採用Frame処理は約0.145ms。その中でUI構築約0.108ms、Snapshot複写約0.010ms。
- Updateは約0.0011ms。現在の軽量処理における値で、実ゲームのUpdate負荷ではない。
- GPUの二Pass全体は約0.0591ms。そのうちImGui約0.0501ms、Clear約0.00373ms。

CPU RenderはGPU Shader時間そのものではなく、Native呼出・Mutex・受付・Fence待機を含む。Main / Update / Render / GPUは並列に進むため、この四つを足してFrame時間にしない。

## Main / Update / RenderとMain側の待機

| 区間 | 件数 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|---:|
| Main/Frame.Main | 40,293 | 0.144884 | 0.229400 | 2.347500 |
| Update/Frame.Update | 40,293 | 0.001096 | 0.001600 | 0.210500 |
| Render/Frame.Render | 40,291 | 0.486039 | 0.747300 | 6.605000 |
| Main/Host.Step | 80,360 | 0.247063 | 0.534800 | 8.513000 |
| Main/Host.FramePermit | 80,360 | 0.162430 | 0.328900 | 5.737200 |
| Main/Host.Pump | 80,360 | 0.007755 | 0.032400 | 7.825700 |

`Host.FramePermit` はFrame.Mainの外側で、次に採用できるFrameの空き枠・開始条件を待つ処理。80,360呼出に対し採用Frameは40,293で、約1.994呼出/Frame。0.162msは呼出当たりの平均で、合計を採用Frame数で割ると約0.323949ms/Frameとなる。Host.Step全体の同じ換算は約0.492740ms/Frame。いずれも待機を含む経過時間で、CPUがその間ずっと演算している意味ではない。FPS上限0なので、この待機を60FPS上限制御の待機とは呼ばない。

`Host.Step`にはFramePermit・Message Pump・採用FrameのMain処理を含む。これらをHost.Stepへ追加して合計しない。Message Pumpの最大7.826msなど少数の長い処理は残る。

## MainのUI内訳

次の行は親子関係を含む。`ImGui.Build`の中にBegin / PlatformNewFrame / NewFrame / Callback / Endがあり、これらをBuildへ重ねて加算しない。

| 区間 | 件数 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|---:|
| Main/Editor.Build | 40,293 | 0.127262 | 0.220600 | 2.201500 |
| Main/Editor.Statistics | 40,293 | 0.007415 | 0.011600 | 0.142500 |
| Main/ImGui.Build | 40,293 | 0.107854 | 0.190100 | 2.154200 |
| Main/ImGui.Begin | 40,293 | 0.042167 | 0.103000 | 2.035300 |
| Main/ImGui.BeginWait | 40,293 | 0.000410 | 0.000400 | 0.064500 |
| Main/ImGui.PlatformNewFrame | 40,293 | 0.021616 | 0.057800 | 1.894000 |
| Main/ImGui.NewFrame | 40,293 | 0.017388 | 0.039100 | 0.621800 |
| Main/ImGui.Callback | 40,293 | 0.056222 | 0.081000 | 0.286700 |
| Main/ImGui.End | 40,293 | 0.007709 | 0.010400 | 0.216600 |
| Main/ImGui.CpuRender | 40,293 | 0.001409 | 0.002200 | 0.076000 |
| Main/ImGui.Snapshot | 40,293 | 0.010088 | 0.019000 | 0.252500 |
| Main/ImGui.InputTransferWait | 40,293 | 0.015263 | 0.037400 | 0.186300 |
| Main/ImGui.Statistics | 40,293 | 0.001745 | 0.002300 | 0.134100 |
| Main/ImGui.TexturePrepare | 40,293 | 0.002574 | 0.003600 | 0.238900 |
| Main/ImGui.Lock | 169,070 | 0.000512 | 0.000400 | 0.135700 |

`ImGui.Callback`はTest UI・性能表示の構築を含む。`ImGui.CpuRender`はImGui::RenderによるCPU描画Data確定であり、GPU描画時間ではない。`ImGui.Lock`は複数箇所の合計呼出なので一Frame一回ではない。

## Render内の排他とCommand記録

| 区間 | 件数 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|---:|
| Render/Graph.Execute | 40,291 | 0.230433 | 0.350500 | 2.595900 |
| Render/Graph.Prepare | 40,293 | 0.008535 | 0.012500 | 0.126000 |
| Render/Graph.FrameReuse | 40,293 | 0.004402 | 0.006300 | 0.083200 |
| Render/Graph.Record | 40,292 | 0.130322 | 0.221800 | 2.324100 |
| Render/ImGui.Record | 40,292 | 0.105480 | 0.191900 | 2.242400 |
| Render/ImGui.RecordWait | 40,293 | 0.079176 | 0.158900 | 2.110000 |
| Render/ImGui.RecordBody | 40,292 | 0.023092 | 0.036800 | 0.238800 |
| Render/ImGui.GpuRecord | 40,292 | 0.021378 | 0.034400 | 0.236300 |
| Render/ImGui.RenderEnterWait | 40,293 | 0.000831 | 0.000500 | 1.439400 |
| Render/ImGui.GpuUsage | 40,293 | 0.000454 | 0.000600 | 0.061900 |
| Render/ImGui.Reacquire | 40,293 | 0.003462 | 0.000500 | 0.953700 |
| Render/Graph.MarkSubmitted | 40,293 | 0.001510 | 0.002200 | 0.024000 |
| Render/Queue.TimestampFrequency | 40,293 | 0.006029 | 0.008200 | 0.103000 |

Graph.Record約0.130msの中にImGui.Record約0.105msがあり、その中の取得待ちRecordWaitは約0.079ms、取得後のRecordBodyは約0.023ms。BackendのGPU command記録そのものを囲むImGui.GpuRecordはCPU時間で約0.021ms。取得待ちが実記録より大きいため、今後検討するならMain/RenderのContext競合が候補になるが、Mutexを取り外してよい結論ではない。

Graph.ExecuteはGraph.Record・Command提出などを包含し、ImGui.RenderScopeも複数段階を覆う。これら包含行を加算してRender時間を求めない。

## Queue / 表示 / Fenceの呼出別時間

| 区間 | 件数 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|---:|
| Render/CommandPool.Submit | 40,291 | 0.062969 | 0.094300 | 0.349600 |
| Render/CommandPool.SubmitLock | 40,292 | 0.000297 | 0.000400 | 0.081700 |
| Render/Queue.Submit | 40,291 | 0.060719 | 0.091400 | 0.346800 |
| Render/Queue.SubmitLock | 40,292 | 0.000301 | 0.000400 | 0.023100 |
| Render/Queue.Signal | 40,293 | 0.004739 | 0.007700 | 0.177900 |
| Render/DXGI.Present | 40,293 | 0.154301 | 0.238500 | 0.918000 |
| Render/DXGI.Statistics | 40,293 | 0.002092 | 0.002800 | 0.093600 |
| Render/DXGI.Ready | 40,293 | 0.003256 | 0.002900 | 0.352900 |
| Render/DXGI.Event | 40,296 | 0.002465 | 0.001600 | 0.318800 |
| Render/Fence.Wait | 1,919 | 1.701238 | 2.993400 | 5.337000 |
| Render/Fence.Event1ms | 1,918 | 1.683158 | 2.970400 | 5.290200 |

Fence.Wait平均1.701msは**発生した1919呼出だけの平均**。全RenderFrameが毎回1.7ms待つ意味ではない。DXGI.EventやFence.Event1msは上位Ready / Fence.Waitの内訳で、合計へ重ねて足さない。1ms指定は実際のReturn時間の上限ではない。

## Renderの重複しない時間配分

各完全Frame.Renderの時刻範囲内で、Graph.Record / Queue.Submit / Present / Ready / Fence.Waitを切り取り、区間の重複を検査した。5分類間の重複は全40291Frameで0。残りはRender親区間からこれらの和集合を差し引いた。

下表は**待機がないFrameも0として含めた同一40291Frame**の統計。前表の呼出別統計とは標本定義が異なる。

| 区間 | 件数 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|---:|
| DXGI.Present | 40,291 | 0.154299 | 0.238500 | 0.918000 |
| Graph.Record | 40,291 | 0.130323 | 0.221800 | 2.324100 |
| Fence.Wait | 40,291 | 0.081027 | 0.000000 | 5.338000 |
| Queue.Submit | 40,291 | 0.060719 | 0.091400 | 0.346800 |
| DXGI.Ready | 40,291 | 0.003257 | 0.002900 | 0.352900 |
| Remaining | 40,291 | 0.056415 | 0.101200 | 1.655600 |
| Frame.Render | 40,291 | 0.486039 | 0.747300 | 6.605000 |

平均の配分: DXGI.Present 31.75%、Graph.Record 26.81%、Fence.Wait 16.67%、Queue.Submit 12.49%、DXGI.Ready 0.67%、Remaining 11.61%。

Fenceは発生頻度が5%未満なので、この全Frame表ではp95が0になる。待機発生時の1.701ms平均・p95 2.993msと併記し、0だけを見て問題がないと判断しない。Remainingは準備・再利用・Completion・統計・Traceなどの残余で、単一の関数名ではない。各行のp95や最大を足してRenderのp95・最大にしない。

## GPU Queryの全期間値

GPU Queryは計測中に提出されたFrameへ対応するものに限定。開始前から残っていたQuery-onlyの各試行1Frameは別扱いにし、全期間統計へ混ぜない。終了時はGPU回収済み。Current二試行で提出40295Frame、両Passの完了Query各40295、対応Envelope40295。CPU完了40293Frameとは非同期境界により件数が異なる。

| 区間 | 件数 | 平均 ms | p95 ms | 最大 ms |
|---|---:|---:|---:|---:|
| ClearFinalColor | 40,295 | 0.003735 | 0.004096 | 0.006144 |
| ImGui | 40,295 | 0.050091 | 0.053248 | 0.413696 |
| BetweenPasses | 40,295 | 0.005255 | 0.006144 | 0.008192 |
| PassEnvelope | 40,295 | 0.059081 | 0.062464 | 0.422912 |

PassEnvelopeは最初のPass開始〜最後のPass終了。Clear・ImGui・BetweenPassesを含む。GPUのPass時間はCPU Present / 記録時間へ加算しない。Query区間にはGPU schedulingなどの影響も含み得る。最大ImGui約0.414msは全体平均と分けて残している。

`ImGui` は Pass 名であり、この構成の `ImGuiPass::execute()` は BackBuffer の Clear / RenderTarget 設定から UI 記録までを含む。UI の Draw だけを単独で計った値とは扱わない。

## 計測位置の Source 対応

- Present API: `Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:207`
- 描画枠の受付待機: 同 File `:229`
- Graph の Command 記録: `Engine/Source/Renderer/DX12/Private/DX12MainFrameGraph.cpp:306`
- Queue への提出: `Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:223`
- GPU Fence 待機: 同 File `:361`
- ImGui Context 取得待ち: `Engine/Source/EditorHost/Private/ImGuiManager.cpp:786`
- UI Pass の Clear / 記録範囲: `Engine/Source/EditorHost/Public/EditorHost/ImGuiPass.h` の `execute()`

## 低速だった再起動後Currentとの参考比較

対象はRebootのCurrent Trial2 /3 /6、SHAは同じ。合算1189.489FPS、30.001126秒。二時刻の全標本をそれぞれ合算した平均であり、各試行のp95を平均した値ではない。

| 区間 | 低速Current平均 ms | 今回平均 ms | 差 ms |
|---|---:|---:|---:|
| Main/Frame.Main | 0.246080 | 0.144884 | -0.101196 |
| Main/ImGui.Build | 0.200571 | 0.107854 | -0.092716 |
| Main/ImGui.PlatformNewFrame | 0.065431 | 0.021616 | -0.043815 |
| Main/ImGui.Callback | 0.085363 | 0.056222 | -0.029141 |
| Main/ImGui.Snapshot | 0.021713 | 0.010088 | -0.011625 |
| Render/Frame.Render | 0.828332 | 0.486039 | -0.342294 |
| Render/ImGui.RecordWait | 0.162496 | 0.079176 | -0.083321 |
| Render/Graph.Record | 0.245034 | 0.130322 | -0.114712 |
| Render/Queue.Submit | 0.096641 | 0.060719 | -0.035923 |
| Render/DXGI.Present | 0.277105 | 0.154301 | -0.122803 |
| Render/DXGI.Ready | 0.123042 | 0.003256 | -0.119786 |
| Fence.Wait / 全RenderFrame | 0.002163 | 0.081027 | +0.078864 |
| GPU PassEnvelope | 0.065627 | 0.059081 | -0.006546 |


今回Renderは約0.342ms短く、Present・Ready・UI/記録待ち・Queueの複数区間が短い。Readyは約0.123→0.003msに減る一方、全Frame換算Fenceは約0.002→0.081msへ増えるため、待機が全消滅した結果ではない。GPU平均の差は約0.0065msで、CPU Renderの差全量をGPU演算だけで説明しない。

### 表示環境差がある

高速測定直後23:18:18の `OverlayCause-20261025-230630/CurrentEnvironment.json` は、Screenが**1台、DISPLAY2 primary、1920×1080**と記録。22:30のMonitor対照はDISPLAY1 primaryと負座標側DISPLAY2の2台構成だった。Defenderは現在RealTimeProtectionEnabled=true。

一画面化と性能回復が同じ時期に観測されたことに加え、その後ユーザーからデュアル→シングルへ実際に変更した確認を得た。デュアルモニター時の表示経路・同期負荷を最有力の原因候補とする。ただし同条件で画面構成を反復切替したA/Bではなく、正確な変化時刻も未確定。低速Reboot側はPointerがアプリ内、今回は外で、時刻・Background負荷・外部CPU採取の有無も異なる。**この表をOverlay OFFや一画面化だけの因果的改善率にしない。** 直後の環境Snapshotを全Frameの接続構成履歴と同一視しない。2000FPS時の歴史記録も各FrameのMonitorを保存していない。

## 保存物・範囲

- `CausalPlan-CurrentTiming-Timing.json`: 今回全CPU Scope・全GPU Passの平均 / p50 / nearest-rank p95 / 最大 / 件数、品質、非重複Render配分。
- `CausalPlan-RebootCurrent-Timing.json`: 低速Current三試行の同形式集計。
- `CausalPlan-CurrentTiming-Comparison.json`: 主要CPU区間の比較。
- `CausalPlan-CurrentTiming-Breakdown.py`, `.log`: 再集計Scriptと結果。

標本数を維持し、通常の直近120標本だけで全期間を代用していない。Timing Trace / GPU query / NVIDIA採取の観測負荷を含む軽量Sceneの記録であり、製品Trace OFF、物理表示FPS、入力遅延、重いゲームSceneの性能は未検証。Engine 実装は変更していないため、Engine Build / CTest は実行していない。調査 Launcher は独立 Build に成功し、全 8 試行の終了 Code・寸法・前面・Pointer・Binary Hash・CSV 件数・欠落を検証した。計測アプリ・GPU Telemetry・記録・解析 Process は終了済み。
