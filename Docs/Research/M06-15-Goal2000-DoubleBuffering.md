# M06-15 / Issue #144 — Double Buffering で 2,000 FPS を目指す改善候補

- 調査日: 2026-10-22
- Branch: `m06/issue144/gpu-waits-and-telemetry`、HEAD: `6effec9`
- 今回は Code と保存済み記録の調査。Engine Code の変更、Commit / Push / Merge は行っていない。

> 続報: [Double Buffering 基準測定](M06-15-DoubleBuffering-Baseline.md)で、前面を維持した同条件の NoSink / CSV 各三回を取得した。NoSink 合算は約 1,200 FPS。平均表示待機が変動し、2,000 FPS はまだ達成していない。厳密な Compile Trace OFF の比較とは区別する。

## 1. 目標条件

ユーザー指定の目標を **Double Buffering で平均 2,000 FPS、平均 Frame 間隔 0.5 ms** とする。

- Development、通常 UI、Waitable ON。
- Buffering 2: CPU Snapshot 2、GPU 資源枠 2、Worker ON、Update 先行上限 1 Frame。
- 比較基準案は既存の通常計測と同じ 1280 × 720、SwapChain 2、GPU 未完了上限 2、Queued presents 2、VSync OFF、Tearing ON、FPS 上限なしとする。Queued presents は Buffering とは独立した値であり、実効値も記録する。
- 直近 120 標本の平均・正確な p95・Max・件数、GPU 計測、入力イベントから反映 Frame までの対応を維持する。
- FrameController の完了数 / 実測時間と Present 回数・間隔を確認する。Render Callback の短さだけで達成と判定しない。
- 平均 2,000 FPS と、全ての Frame が 0.5 ms 以内であることは別の条件。p95 / Max / 入力遅延は併記し、平均だけ上がって遅延が悪化する変更は分けて評価する。

## 2. 保存済み記録から分かる予算

2026-10-19 の `GpuTimeline144/NativeQ2` を再集計した。この記録は上記の比較基準案と同じ構成、前面維持、20 秒、ETW なし。ただし詳細 CPU / GPU Trace と Clock Calibration は有効だった。**新たに取得した通常 Build の基準値ではない。**

- FPS: 1,034.323、計測中完了 Frame 20,687。
- 完全に計測期間内へ収まる Frame.Render は 20,686 件。境界を跨ぐ Frame と欠落を混同しない。
- 平均 Frame 間隔は FPS から約 0.967 ms。0.5 ms に向け、約 0.47 ms を減らす必要がある参考値。

| 区間 | 平均 ms | 解釈 |
|---|---:|---|
| Frame.Render | 0.9603 | 待機を含む |
| Render 内の DXGI.Event 合計 | 0.6785 | 同じ Frame 内の全待機を合計 |
| Render 内の Fence Event 合計 | 0.00175 | 多くの Frame は 0、長い例も残る |
| 上記二待機以外の Render | 0.2800 | CPU 実行だけではなく、他の排他・再実行待ちも含む |
| Graph.Execute | 0.1407 | Render の内訳。ほかの行へ重ねて足さない |
| Graph.Record | 0.0373 | Graph.Execute の内訳 |
| Present API | 0.1248 | 表示受付待ちとは別の API 呼出区間 |
| Main | 0.1005 | Render と重なり得るので加算しない |
| UI Build | 0.0850 | Main の内訳 |
| 統計 Snapshot 取得 | 0.00525 | UI Build の外側、Main の内訳 |
| Main 終了 → Update 開始 | 0.00831 | 通知・進行情報更新・Scheduling 等の経過時間 |
| Update 処理 | 0.000753 | この軽量 Scene での値 |

Render 内の待機は Thread / Frame / 時刻包含で限定し、Native Wait の区間を合併して集計した。二種類の待機が重ならないことと、Render 区間の一意性を全件検証した。

この記録では表示受付待ちが Render の約 71% を占める。残り約 0.28 ms が仮に変わらなければ、表示待ち等へ使える予算は約 0.22 ms になる。ただし、待機を減らすと Queue や Scheduling の状態も変わるため、差分をそのまま実現可能な改善量とは見なさない。

2026-10-21 の `Signal144/Companion1` は Render 平均 0.7628 ms、DXGI Event 合計平均 0.3585 ms、残り約 0.4042 ms。こちらは 960 × 720、前面切替あり、Kernel Stackwalk / Sampling ありのため、上表との改善率は出さない。

## 3. 最優先: 表示経路と Adapter の対応

### 分かったこと

- `DX12RenderDevice::create` は Hardware を `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE` 順に選ぶ。Window の表示先 Adapter と同じであることを条件にしていない。
- 保存済み Stack には NVIDIA Kernel Driver を通る Context 優先度変更と、Probe 内の Intel User Mode Driver による Present 処理がある。
- GPU の Graph Pass が短いのに、Present / 表示枠待ちが長い。

これらは Hybrid GPU の表示経路を調べる根拠になる。ただし Module の存在だけで描画先・表示先の全対応やコピー回数を確定しない。**Adapter 間転送が平均 FPS の主因かは未確定。**

### 改善案

まず描画 Device の Adapter LUID と、Window の表示先 Output が属する Adapter LUID、対象 SwapChain / Context を照合する。異なる場合に、同じ表示先 Adapter で描く比較を行う。

差が大きければ、アプリケーション側で Adapter を明示指定できる選択方針を検討する。軽量 UI / Editor と重い 3D Scene の両方を、一律の固定 GPU に変更しない。比較結果によっては Engine の Code より表示接続経路が支配的な場合もある。

Microsoft は Hybrid GPU の Adapter 間資源共有・転送と CASO によるコピー削減を説明している。これは改善候補の根拠であり、この環境での削減時間を保証するものではない。[Hybrid 資源共有](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/using-cross-adapter-resources-in-a-hybrid-system)、[CASO の説明](https://devblogs.microsoft.com/directx/optimizing-hybrid-laptop-performance-with-cross-adapter-scan-out-caso/)

リスク: 表示先 GPU に合わせると重い描画では演算性能が下がる場合がある。Output の移動、Device 再生成、資源寿命も設計対象になる。比較前に既定方針を変えない。

## 4. 次点: FrameGraph / RHI の毎 Frame の CPU 処理

Graph.Execute と Present API を合わせて約 0.27 ms の参考値がある。Driver 内部の時間を Engine の関数呼出削減だけで全て消せるわけではないが、0.5 ms の予算では調べる価値がある。

候補:

1. Queue ごとに一定の Timestamp Frequency を生成時に保持する。現在は `DX12MainFrameGraph::execute_graphics` から毎 Frame `GetTimestampFrequency` を呼ぶ。Device / Queue 再生成時に無効化する。GPU Query 自体や標本数は減らさない。
2. FrameGraph の毎 Frame の `SharedCommandCompletion`、CommandPool の `DX12CommandCompletion` の確保を調べる。完了済み Object の再利用や固定 Slot を検討するが、外部が保持する Completion の値を後続 Frame で書き換えない。
3. CommandPool の共有 Lock が `queue->submit`、Driver の Execute / Signal を覆っている点を調べる。前面切替時には Signal 内の Device Lock 待機がある。貸出状態と停止処理を保護しつつ、Driver 呼出中に関係ない Pool 操作まで止める必要があるか検証する。

これらの個別効果は未測定。特に Lock 範囲変更と Completion 再利用は寿命・Error 回復に影響するので、小さな Cache 追加と同じ難度では扱わない。現在の単一 Graphics Queue / 二 Pass の Scene で、Cross-Queue Scheduler の改造を最優先にする根拠はない。

## 5. 通知と待機の改善

### Worker 通知

Main 投入、Update 完了、Render 完了、周期完了で同じ Waiter の `notify_all()` を使う。Double Buffering と Worker ON を維持し、必要な Consumer だけを通知する方式は候補になる。

ただし、保存記録の Main → Update の経過時間は平均約 8.31 µs。この全部を消せたとしても、目標まで不足する約 0.47 ms を単独で説明できない。Update → Render は平均約 0.85 ms だが、先行 Snapshot が前 Frame の描画を待つ期間を含むので、丸ごと無駄な Context Switch と扱わない。

リスク: 通知先分割による通知漏れ、停止・Drain・Error での解除漏れ。Thread 優先度や常時 Spin の変更を先に行う根拠はない。

### GPU / DXGI 待機

1 ms ごとの Timeout と再確認は、停止・Resize の通知を待機対象へ組み込むことで減らせる余地がある。ただし、表示 Event が通知されるまで待った区間も長いため、Timeout を縮めるだけでは平均 FPS が改善する説明にならない。

Waitable の許可を二重に消費しない、同じ完了済み Fence を再待機しないという契約を保つ。現在の `m_hasFramePermit` は既に許可を保持するため、二箇所から呼ぶだけで必ず二回待つと判断しない。資源再利用の Fence は削除しない。

## 6. ImGui と統計は優先度を分ける

### 通常時に大きな待機とは確認できなかった部分

`NativeQ2` の `ImGui.GpuUsage` は 20,687 回、平均約 0.265 µs、Max 10.5 µs。最新の `Companion1` でも平均約 0.277 µs。UI の表示だけから「毎 Frame 約 0.25 ms の GPU Fence 待ち」と解釈していたら修正が必要。

`DX12ImGuiBackend::wait_for_usage` は完了済みなら即 Return し、`gpuWaitTimes` には**実際に待った回だけ**を追加する。そのため UI の平均は待機発生時の条件付き平均であり、全 Frame 当たりの負担ではない。計測期間中に新しい待機がなければ、Warmup の待機標本が残ることもある。

改善案は「待機チェック全回の 0 を含む統計」と「実際に待った回の統計・回数」を区別すること。既存情報を失わず、誤読を防ぐ。これは統計表示の意味の改善であり、そのまま FPS の改善には数えない。

### 条件付きの排他改善

ImGui の公式 Backend 呼出し、GPU Ring 再利用、Texture 更新は Context Lock 内にある。GPU 待機が起きる条件では Main の UI 構築も止め得る。待機対象 Fence / Texture の確定と実際の待機を分け、可能なら Lock 外で待つ設計は候補。

ただし通常の今回記録では上記チェックは短い。最優先の FPS 対策とせず、Texture 更新・Resize・Skip を含めて検証する。公式 Backend は Context / Renderer 状態を参照するため、Snapshot があるという理由で Mutex を取り外さない。

統計 Snapshot 取得は平均約 5.25 µs。`WindowsHost::graph_performance` は `TimingSamples` を五つ複製してから `statistics()` を呼ぶが、現在の `statistics()` は定数時間なので、小さい集計結果だけを Lock 内で複写する候補がある。標本更新・表示を間引く必要はない。一方、既に全標本 Sort を避けており、この変更だけで FPS が倍になるとは見込まない。

## 7. 改善の判定手順

1. 同じ Source / 実行 File / Size / 前面状態で、通常の Trace OFF を基準にする。次に詳細 Trace ON / Sink なし、Trace ON / CSV のみを比較し、計測の影響を分離する。既存の通常 120 標本の統計と GPU Query は維持する。
2. ETW が必要な回でも、まず Stackwalk / CPU Sampling を外す。前回は Stack 展開中の Hard Fault / I/O が約 8.76 ms の停止を追加した。詳細 Stack は再現した対象の説明用に分ける。
3. 比較案は Warmup 後 20 秒を三回ずつ、変更前後を交互に実行。前面・Client Size・設定実効値・欠落を検査する。未完了の部分や計測境界を勝手に 0 としない。
4. FPS、Present 間隔、全区間の p95 / Max、通常 UI の直近 120 標本、CPU 消費、入力イベントから反映 Frame までの値を保存する。無入力の比較で入力遅延が改善したとは主張しない。
5. 平均 FPS の達成確認と、前面切替等によるスパイクの原因調査を別々に評価する。特に最新の 960 × 720 / 前面切替 / Stack 有効の記録と、1280 × 720 / 前面維持の記録から改善率を作らない。

順序は **基準測定 → Adapter / 表示経路 → FrameGraph / RHI の CPU 負荷 → 通知・条件付きの排他**。GPU Pass が約 0.03～0.05 ms の現状では Shader の小変更、PSO Cache、Build 時処理の Cache を最優先にする根拠はない。現在も FLIP_DISCARD / Tearing 対応なので、Flip Model 導入を未実装の改善として挙げない。[Flip Model の説明](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)

## 8. 今回の成果物と制限

`out/validation/Performance20261022/Goal2000/` に個別集計 Script と結果を保存した。

- `SavedScopeAnalysis.json` / `.log`: 二記録の区間別統計、Render 内の待機合併・差引き。
- `AnalyzeSaved.py`、`VerifySaved.py`: 集計と検証。完全な Render 区間 20,686 / 25,595 件について一意性と待機の非重複を確認した。
- `HandoffsAndScopes.json`: Main → Update / Update → Render の経過時間の参考集計。

Code 調査で候補と優先度を絞った段階で、改善を実装・比較検証した結果ではない。本報告後にユーザーから操作可能の回答を受け、上記続報の基準測定を実施した。Engine / Build 設定は変更しておらず、Build は再実施していない。既存の未 Commit 変更を保全した。
