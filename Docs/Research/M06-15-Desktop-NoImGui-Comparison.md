# M06-15 / Issue #144 — ImGui を初期化しない条件との比較

- 測定日: 2026-10-24（日本時間）
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 親 Commit: `ba1c485d895683cc50e7db390b3ac27a8f2a01a7`。計測 Probe の追加変更は未 Commit。
- 前回: [Desktop Performance Investigation](M06-15-Desktop-Performance-Investigation.md)

## 1. 結果

**同じ Development Binary で、通常 UI ありは合算 1,686.278 FPS、ImGui なしは 2,143.090 FPS。**
今回の条件では FPS が **27.09% 増加**し、1 Frame あたりの平均経過時間は約 **21.32% 短縮**した。
GPU のみの描画時間を比較した値ではなく、Main / Update / Render / Present を含む完了頻度である。

| 組 | 通常 UI あり FPS | ImGui なし FPS |
|---|---:|---:|
| 1 | 1,765.217 | 2,174.248 |
| 2 | 1,687.846 | 2,100.295 |
| 3 | 1,605.771 | 2,154.732 |
| **合算** | **1,686.278** | **2,143.090** |

通常 UI は 101,178 Frame / 60.000784 秒、ImGui なしは 128,592 Frame / 60.003090 秒。
FPS は完了 Frame 数 / 実経過秒であり、瞬間 FPS の平均ではない。

| 全区間の指標 | 通常 UI あり | ImGui なし |
|---|---:|---:|
| Frame 間隔 平均 ms | 0.593026 | 0.466621 |
| Frame 間隔 p95 ms | 1.2679 | 0.7714 |
| Frame 間隔 最大 ms | 6.2377 | 6.2891 |
| CPU Main 平均 ms | 0.153713 | 0.000673 |
| CPU Render 平均 ms（待機を含む） | 0.585015 | 0.458601 |
| Graph.Record 平均 ms | 0.147619 | 0.022901 |
| Present API 平均 ms | 0.174254 | 0.179683 |
| 表示準備待機 平均 ms | 0.130431 | 0.123351 |
| GPU Pass 全体 平均 ms | 0.066911 | 0.015249 |

包含区間や並列区間を足し合わせない。平均と p95 は改善したが、約 6.3 ms の最大間隔は残っている。
ImGui を除くだけで Present / OS / GPU 同期の揺れを解消した結果ではない。

## 2. ImGui なしの意味

`Engine/Tests/Performance/SpikeTimingProbe.cpp` だけに `ui=3` を追加した。
通常の Engine / Editor 起動設定や Production の機能は変更していない。

- 通常 UI は既存の EditorHost を使う。
- ImGui なしは WindowsHost を直接所有し、ImGui Context / Win32 Backend / DX12 UI Backend を初期化しない。
- UI 構築、Snapshot 作成・転送、ImGui 記録 Callback、ImGui Allocator Hook を使わない。
- 比較用表示 Pass は ImGuiPass と同じ BackBuffer Clear / RTV 設定 / Write・RenderTarget 宣言を保ち、UI Draw だけを行わない。
- ClearFinalColor と BackBufferClear の 2 Pass を維持する。既定の巨大三角形コピーへ切り替えて負荷を増やす比較ではない。
- GPU Query、FrameController、Queue / Fence、SwapChain / Present は継続する。

同一 Binary 内の切替なので ImGui Library 自体はリンクされているが、なし条件では実行基盤を生成しない。
ログの `imgui_initialized=0 current_context=0` と、全 CPU Trace に ImGui / Editor 区間がないことを確認した。
GPU Query も `ClearFinalColor / BackBufferClear` だけである。

## 3. 共通条件と検証

| 項目 | 条件 |
|---|---|
| PC | Ryzen 7 3700X / GeForce RTX 3060。前回と同じ PC |
| Build | Development x64、詳細 Trace Sink ON |
| UI 比較順 | あり → なし → なし → あり → あり → なし |
| 時間 | 各 Warmup 2 秒 + 20 秒、両条件とも 3 回 |
| Client Size | 1280 × 720 |
| CPU / GPU 枠 | 2 / 2、Worker ON、Update lead 1 |
| 未完了 GPU / SwapChain / Queued | 2 / 2 / 2 |
| 表示 | Waitable ON、VSync OFF、Tearing ON、FPS 無制限 |
| ETW / Trigger / FocusCycle | 全て無効 |
| Host の File / Debug Log | 両条件とも無効 |
| Layout / 性能設定の保存 | 両条件とも無効 |
| 配置 | 全 6 回とも `DISPLAY1`、Window 左上 `(196,273)`、Pointer `(844,930)` |
| Binary SHA256 | `7AA7BCE9AF186E335A9104F7EB46964C0B4CAA6D4E871C88EE6F1B33F93CF208` |
| Probe Source SHA256 | `CE4C5655852D4B43315E60497BA0114511EFDB9CF1200C6B0B17757E9F455B95` |

全 6 回で Exit 0、Binary Hash 一致、前面 / 可視 / 非最小化 / Client Size の不一致 0、
Window 移動 0、Pointer 変化 0、Snapshot 追加確保 0。なし条件の零件は Snapshot 自体がない意味である。
配置は Pointer 座標を基準に共通化したが、Frame ごとの Hover 対象 HWND を記録した検証ではない。

CPU / GPU Record の欠落・GPU 上書き・エラー Status は全て 0。CSV 行数と宣言件数の一致、
CPU の Thread 内連番、Render Frame の連続性を検証した。
計測中の提出は各 1 回で、対応する 2 Pass の完了 GPU Query が全件揃った。
CPU 完了、CPU Trace、非同期 GPU の測定境界による件数差を同じ標本数とみなさない。

## 4. RAM 保持と終了後の保存

- CPU / GPU 記録配列は初期化時に確保し、測定中に File へ流さない。
- `Recorder::write` と `write_gpu` は、CPU または GPU の記録中なら `fopen` より前に拒否する。
- Worker / GPU の Shutdown と記録停止後にだけ CSV を出力する。
- Runner も標準出力 / 標準エラーを非同期で RAM に保持し、Process 終了後に `.log` / `.err` / 起動・終了情報を保存する。
- 全試行で `recording_output guarded=1 early_file_write_attempts=0` を確認した。

ただし、Process 全体の I/O カウンターには各試行 1 回、112～113 bytes の書込みが残った。
これは全 I/O の会計値であり、保存先や File / Pipe 等の種別を特定する情報ではない。
計測結果の早期 File 保存はないことを確認したが、OS / Driver を含む全 File / Device I/O がゼロとは主張しない。
この 1 件の発生元の特定は今回行っていない。
[GetProcessIoCounters](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessiocounters)

`NoImGui-20261024-190230` と `NoImGui-20261024-190419` は、この会計値を File 保存と同一視した
旧判定で中断した予備試行。正式な比較には混ぜず、記録を残した。

## 5. 保存先と再現方法

Git 管理外の Root:
`out/validation/Performance20261024/Desktop144/NoImGui-20261024-190622/`

- `Comparison.csv`: 6 試行の FPS、Frame 間隔、CPU Render の一覧。
- `Comparison.json`: 全条件検査、CPU / GPU の詳細統計、合算結果。
- `Pair*-Ui*.log` / `.err` / `-run.json`: 終了時に保存した標準出力、引数、PID、Hash、終了コード。
- `Pair*-Ui*-spans.csv` / `-gpu.csv`: RAM に保持した全 CPU / GPU 記録。
- `MeasuredProbe.exe` / `SpikeTimingProbe.cpp`: 使用 Binary と Source の控え。

Runner / 解析処理は同じ上位 Folder の `RunNoImGuiComparison.ps1` / `AnalyzeNoImGui.py`。
Probe の第 4 引数 `ui` は 0=通常統計 UI、1=TEST のみ、2=空の UI Callback、3=ImGui 未初期化。
末尾の追加引数 `fixedPlacement=1` は共通配置と Pointer / Window 位置の検査を有効にする。

## 6. 検証範囲と残る制限

- 規約指定の Debug Build と最終 Development Probe Build 成功。全 CTest は今回再実行していない。
- 計測アプリは終了済み。Configure Cache の `CUE_ENABLE_TIMING_TRACE` は OFF に戻した。保存 Binary は ON のまま。
- 詳細 Trace ON の比較なので、ImGui 除去により不要になった観測処理の負担も減る。通常製品構成の改善率とは同一視しない。
- Clear と通常 UI の軽い Scene の比較であり、重いゲーム Scene、入力遅延、長時間の安定性は未測定。
- 今回は配置・Log 設定・Probe Source も揃え直したため、旧 2,073 FPS との単純差を ImGui の効果にしない。
- Commit / Push / Merge は行っていない。既存の未追跡資料・調査 Code は保全した。
