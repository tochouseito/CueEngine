# デュアルモニターでの排他 Fullscreen / Borderless 比較

2026-10-26。ユーザーがデュアルモニターへ戻した状態で、排他的 Fullscreen と Borderless 全画面を比較した。Engine 正本は変更せず、`out/validation/Performance20261026/DisplayModes/` に調査用 Source コピーと実行 File を生成した。Commit / Push / Merge は行っていない。

## 結果

**この条件では Borderless が有利で、排他 Fullscreen による高速化は確認できなかった。** Borderless は合算 1004.946 FPS、排他は 840.559 FPS。Borderless を基準に排他の FPS は 16.36% 低い。これは今回の短い反復計測の差で、他の PC / Driver / Scene に一般化しない。

| 項目 | Borderless | 排他 Fullscreen |
|---|---:|---:|
| 各試行 FPS | 1025.395 / 1003.367 / 986.076 | 789.492 / 801.010 / 931.174 |
| 合算 FPS | 1004.946 | 840.559 |
| 測定時間合計 | 30.002595 s | 30.003826 s |
| Render 完了間隔 平均 | 0.995044 ms | 1.189545 ms |
| Render 完了間隔 p95 | 2.094700 ms | 2.755500 ms |
| Render 完了間隔 p99 | 4.307200 ms | 5.648900 ms |
| Render 完了間隔 最大 | 14.376900 ms | 85.886500 ms |
| 3 ms 超の完了間隔 | 710 / 30145 間隔 | 1026 / 25214 間隔 |
| CPU Frame.Main 平均 | 0.369560 ms | 0.351567 ms |
| CPU Frame.Update 平均 | 0.001798 ms | 0.001896 ms |
| CPU Frame.Render 平均 | 0.902863 ms | 1.118813 ms |
| Present 平均 | 0.365963 ms | 0.615532 ms |
| Present p95 | 0.648900 ms | 1.752500 ms |
| DXGI 受付待ち平均 | 0.016545 ms | 0.018475 ms |
| Graph.Record 平均 | 0.303711 ms | 0.267747 ms |
| ImGui.RecordWait 平均 | 0.216981 ms | 0.182202 ms |
| Queue.Submit 平均 | 0.101461 ms | 0.098346 ms |
| Fence 待ち / 全 Render 平均 | 0.003155 ms | 0.002536 ms |
| GPU ClearFinalColor 平均 | 0.004171 ms | 0.004530 ms |
| GPU ImGui Pass 平均 | 0.048245 ms | 0.053825 ms |
| GPU 2 Pass 全体 平均 | 0.056648 ms | 0.062867 ms |

CPU Render の差は約 +0.216 ms、Present は約 +0.250 ms。Graph.Record などは排他側で短いため、Present の増大が今回の全体差と整合する。GPU 2 Pass の差は約 +0.0062 ms で、描画処理そのものの増大では CPU Render の差を説明できない。

Frame.Main / Update / Render / GPU は並行する経過時間。Graph.Record は ImGui.RecordWait を含む。表全体を合計して Frame 時間にしない。Present は API の経過時間であり、CPU の演算時間や物理的な画面表示までの遅延ではない。最大完了間隔 85.887 ms も Present 単独の最大値ではなく、これをすべて Present に帰属させない。

## 共通条件

- NVIDIA GeForce RTX 3060、Driver 617.42、Development、通常 Test UI と性能統計を表示
- 有効画面は 2 枚。開始時の `DISPLAY1` は Primary / 1920×1080 / 75 Hz、`DISPLAY2` は 1920×1080 / 60 Hz
- 両条件とも `DISPLAY1` の `(0,0)` に Client / SwapChain とも 1920×1080 で配置。Window Style は同じ Borderless 形式で、Native 排他状態を切り替える
- ClearFinalColor と ImGui の 2 Pass。ImGui Pass は BackBuffer Clear と UI 描画を含む
- Worker ON、Buffering 2、未完了 GPU 2、SwapChain Buffer 2、Queued Presents 2、Waitable ON
- VSync OFF、FPS 上限なし。Borderless の Present は ALLOW_TEARING、排他の Present は仕様に従い Flag 0
- SwapChain は両条件とも FLIP_DISCARD / R8G8B8A8_UNORM、生成 Flag は 2112（ALLOW_TEARING と FRAME_LATENCY_WAITABLE_OBJECT）
- 外部 CPU 周期監視と NVIDIA 周期 telemetry は OFF。PIX / Nsight / WPR / PresentMon は併用しない
- NVIDIA 設定は変更していない。画面上で NVIDIA 性能 Overlay が表示され、全 6 Process で `nvspcap64.dll` の Load を確認した。注入なしの比較ではない
- 本計測は全 6 回とも通常のデスクトップ権限で実行し、実行環境の権限差を比較条件へ混ぜない

順序は Trial9 Borderless → Trial10 排他 → Trial11 排他 → Trial12 Borderless → Trial13 Borderless → Trial14 排他。各試行は前面取得後に 2 秒 Warmup、その後 10 秒測定した。初回 Resize は Warmup に含め、測定中の Client Size は全区間で安定している。

同一実行 File の SHA256 は `A54DEB808C8F3E357E60368DAA1BED32CD5B9EC048B886E32A00FE6CBAFF3A91`。各試行の起動前・終了後 Hash は一致している。

## 成立確認と保存方式

- `SetFullscreenState(TRUE, output)` と切替後の明示 `ResizeBuffers` が S_OK であることを排他試行で確認した
- `GetFullscreenState` を生成時・最初の Present・2048 Present ごと・停止時に確認した。Borderless は各 8 回、排他は各 7 回の確認で、全試行不一致 0 / Query 失敗 0
- 全 6 回の Window 状態は可視・前面・非最小化・Client 1920×1080 が成立。状態不一致 0、状態変化 0
- 外部 Observer の Pointer 変化 0、Window 移動 0。初期 Pointer は全 6 回 `(966,530)`。初期 Hover 観測は前面取得前なので、測定中 Hover の証明とは扱わない
- CPU Trace 欠落 0、GPU 記録欠落 / 上書き 0、Snapshot 追加確保 0。Module 一覧取得も全 6 回成功 / 打ち切りなし
- 別の読み取り再集計でも Render / Present 平均が一致。採用した全 Frame.Render に両 GPU Pass の記録が対応し、GPU Status 異常 / 重複は 0。各試行の Render 標本数は完了 Frame 数より開始・終了境界の 1 件少なく、間隔数はさらに 1 件少ない
- 調査 Recorder は RAM に保存し、測定終了後に CSV へ出力。Export Guard の早期 File 出力要求は全試行 0
- 終了時 `SetFullscreenState(FALSE,nullptr)` と Windowed 復帰確認が全試行成功。計測 Process / Launcher は終了済みで、デュアル画面構成を維持している

ただし Process 全体の書込 I/O は各 10 秒で約 63～74 KB（885～1037 操作）記録されている。これは調査 Export の早期出力 0 と別の観測であり、発生元は未特定。`GetProcessIoCounters` の Write は Disk File だけを指すものではなく、Pipe / Device なども含み得るため、「計測中の File I/O が完全に 0」「全量が Disk 書込」とはどちらも断定しない。

## 調査用実装と初期試行

`GenerateProbe.ps1` が現行 `SpikeTimingProbe.cpp` と `DX12SwapChain.cpp` を調査 Directory へコピーし、その TU だけを既存 Development Library と組み合わせて生成する。正本の 2 Source は生成前後の Hash 一致を確認した。通常 Build へ Fullscreen 切替 API は追加していない。

Window は通常生成時に装飾枠によって初期 Client 高さが 1061 に制限される。この制限を SwapChain 生成前に解き、1920×1080 へ統一した。FrameGraph は Native BackBuffer の Desc から初期寸法を作るため、FinalColor も FHD となる。Host に残る初回 Resize 通知は Warmup で処理される。

Trial1～8 は起動診断であり、本比較に含めない。初期の制限付き実行では SetFullscreenState が E_FAIL となったが、同じ Binary を通常のデスクトップ権限で起動すると成功した。Trial7 は起動後の host.step が終了し、Trial8 は表示確認を含む診断用。比較には同一 Binary / 同一権限の Trial9～14 だけを使った。

## 限界と判断

- `GetFullscreenState(TRUE)` は DXGI 上の論理排他状態を示す。Windows の Fullscreen Optimizations を無効化しておらず、実際に Independent Flip / Composed のどの経路になったかは今回未計測
- 背景 CPU 負荷・GPU Clock の全期間記録は今回行っていない。実行順を ABBAAB としたが、各試行の揺れは残る
- モード確認は全 Present ではなく疎な Sampling。短時間の解除と復帰まで完全に除外する証拠ではない
- 通常の枠付き Window と同じ条件での比較、シングル画面との同時比較、物理表示間隔 / 入力遅延の測定は行っていない
- 過去の約 2000 FPS の主条件は Client1280×720。今回 FHD は 2.25 倍の Pixel 数であり、これをそのまま単画面 / 複数画面の差としない
- 各 3 回 / 合計 30 秒の軽量 Scene。現状では排他を優先する根拠は得られず、Borderless を維持する判断が妥当

## 検証と出力

- `pwsh -NoProfile -File scripts/codex_build.ps1` の通常 Debug Build 成功。Log は調査 Directory の `DefaultDebugBuild.log`
- 調査用 Development 実行 File の MSBuild 成功。既存 Library の Build 条件は `OptimizedProbe/ProbeInputs.cmake` を再利用
- `git diff --check` 成功。今回の Engine Source 変更なし。自動 Test Suite の追加実行は行っていない
- 再現手順: `GenerateProbe.ps1` → 調査 CMake / MSBuild → DLL と EngineResources 配置 → 通常のデスクトップ権限で `RunComparison.ps1` → `Analyze.py`
- 生記録: `out/validation/Performance20261026/DisplayModes/Results/Trial9-*` ～ `Trial14-*`
- 集計: `out/validation/Performance20261026/DisplayModes/Summary.json`

公式資料: [SetFullscreenState と ResizeBuffers](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate)、[D3D12 の Fullscreen Waitable 対応](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag)、[Flip Model と Independent Flip](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)。
