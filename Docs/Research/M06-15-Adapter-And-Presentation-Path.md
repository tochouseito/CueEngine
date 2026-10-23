# M06-15 / Issue #144 — 描画 Adapter と表示経路の照合

> 続報: [Intel 描画・表示と RTX 描画の比較](M06-15-Intel-Presentation-Comparison.md)で、Intel への生成時選択と比較計測を実施した。以下は比較前の経路確認記録。

- 調査日: 2026-10-23
- Branch: `m06/issue144/gpu-waits-and-telemetry`、HEAD: `6effec9`
- 目標: Double Buffering、Development、通常 UI、Waitable ON で平均 2,000 FPS
- 前回: [Double Buffering 基準測定](M06-15-DoubleBuffering-Baseline.md)
- Engine Code / Build 設定、OS の GPU 選択・表示設定は変更していない。Commit / Push / Merge も行っていない。

## 1. 結論

**描画と表示処理が別 Adapter を通ることを確認した。** 新しい記録では、Graph の Command 提出は NVIDIA RTX 3060 Laptop GPU、対象 Surface の IndependentFlip Context は Intel UHD Graphics に属していた。現在の Windows の Active Display Path も Intel の LUID を返した。

計測対象 24,669 Frame 全てについて CPU Present、PresentHistory、IndependentFlip、GPU Pass Query を対応付けた。最後の GPU Pass Timestamp から IndependentFlip Event までは、平均 **0.8774 ms**、p95 **1.7601 ms**、最大 **4.5044 ms**。同じ記録の GPU Pass 全体区間は平均 **0.0407 ms**だった。

この差は転送・同期・Driver / Scheduler / 表示側処理を含む後段の経過時間であり、**全てが GPU 間の純粋なコピー時間という結論ではない**。Double Buffering では Frame が重なるため、この遅延をそのまま Frame 間隔へ加算したり、0.5 ms を超えるだけで 2,000 FPS が不可能と判断したりしない。

有力な次の比較は、同じ Editor / UI / Buffering / 表示設定を維持した **Intel 側での描画**。Adapter 間の経路を減らす効果と、Intel 側の描画性能の差を同時に測る。今回この比較は実行しておらず、達成 FPS や削減時間は未保証。

## 2. Adapter の同一性と表示構成

DXGI の Description と ETW の `DpiReportAdapter` の Vendor / Device ID / LUID が一致した。

| GPU | Vendor / Device | Adapter LUID |
|---|---|---|
| NVIDIA GeForce RTX 3060 Laptop GPU | `10DE / 2520` | `0x000000000000DF8F` |
| Intel UHD Graphics | `8086 / 9A60` | `0x000000000000DCC8` |
| Microsoft Basic Render Driver | `1414 / 008C` | `0x000000000000DF56` |

Engine と同じ `HIGH_PERFORMANCE` 順では NVIDIA、`MINIMUM_POWER` 順では Intel が先頭だった。これは列挙順の照会であり、Engine の設定を変更したものではない。[EnumAdapterByGpuPreference](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgifactory6-enumadapterbygpupreference)

表示構成は二つの API の結果を保持した。

- `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` は `DISPLAY1` の Source / Target とも Intel の LUID を返した。Target Device Path は `DISPLAY#CMN15E7...`、出力 Technology は Internal、Refresh は約 60 Hz。
- 一方、DXGI の `EnumOutputs / GetDesc` は NVIDIA の Output として `DISPLAY1` を返し、Intel の Output 列挙は空だった。

DXGI の Output 名だけで Intel の表示経路を否定せず、Active Path と ETW の表示 Context を照合した。API の見え方が異なる Driver 内部の理由は未特定。測定前後の二回の照会で Adapter 一覧と Active Path が同一であることも確認した。[QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig)

## 3. Context / Queue での確認

### 保存済み 2026-10-21 / Companion1

- Probe PID 18940 の Device `0xffffa90d21990810` は NVIDIA Adapter `0xffff8383f6b7f000` に対応。
- 既に確認していた描画側 HW Queue と Node 4 の Copy Engine Queue がこの Device に属する。
- 対象 IndependentFlip Context `0xffffa90d2910ede0` は別 Device `0xffffa90d2af57810`、Intel Adapter `0xffff8383f51a5000` に対応。
- NodeMetadata で NVIDIA の Node 4 は Copy（EngineType 6）、Intel の Node 0 は 3D（EngineType 1）。

Module 名の存在だけによる推定から、Adapter → Device → Context → HW Queue の対応へ進められた。

### 新しい 2026-10-23 / NoStack2

正式記録は Probe PID 14012、Render TID 7712。

- Frame 18019 の `Graph.Execute` は ETL 時刻 18,375,105.85～18,375,228.15 µs。
- 同じ Thread / 区間内の Native Command Packet 三件は、Context / Device を介して全て NVIDIA の LUID に対応。
- 対象 Surface `0x0000000033a6c15b` の IndependentFlip Context `0xffffa90d1cb73de0` は Intel の LUID に対応。
- NVIDIA の Copy Engine Context / Queue も記録されていた。

Copy Engine 上の全 Packet を個別 Frame の画素転送へ完全対応させたわけではない。異なる同期 Object の Fence 数値が同じという理由だけで、生産側 / 消費側を確定していない。

## 4. 正式記録の条件と結果

| 項目 | NoStack2 |
|---|---|
| 実行 File | 既存 Development Probe、前回と同じ Hash |
| Buffering / GPU 上限 / SwapChain / Queued presents | 2 / 2 / 2 / 2 |
| 表示 | Waitable ON、VSync OFF、Tearing ON、FPS 無制限 |
| UI / Window | 通常 UI、1280 × 720、前面・可視・非最小化を維持 |
| 期間 | Warmup 2 秒 + 20 秒 |
| 記録 | CPU / GPU CSV、Kernel / GPU / 表示 ETW。Stackwalk / Sampling はなし |
| FPS | 1,233.413、24,669 Frame / 20.000606 秒 |
| CPU Render 平均 / p95 / Max | 0.8038 / 1.6395 / 4.2626 ms |
| Present API 平均 / p95 / Max | 0.1342 / 0.1986 / 0.4412 ms |
| GPU Pass 全体区間 平均 / Max | 0.0407 / 0.0809 ms |

前回の約 1,200 FPS に対する改善を実装した回ではない。ETW Buffer 条件も異なる一回の測定なので、差を改善率や ETW 負荷の確定値にしない。Trace 対応 Binary であり、Compile 時から Trace OFF の比較ではない。通常 120 標本の統計、GPU Query、入力 Frame 対応は維持した。無操作測定から入力遅延の改善も主張しない。

## 5. 全 Frame の Present / GPU / 表示 Event 対応

Native DXGI Present Start と PresentHistoryDetailed Start は起動・Warmup 込みで各 27,071 件、SwapChain は一つ。順序を対応付け、全 History Start が対応する Present の後であることを検査した。

対象 Surface の IndependentFlip は 26,906 件、PresentCount 166～27,071 が連続。最初の 165 呼出はこの Event 系列に入らないが計測対象外であり、欠落や成功値に置き換えなかった。

計測対象 24,669 Frame は全件一意対応。PresentMon の QPC → Native Present → PresentCount → 同 Surface の IndependentFlip を照合した。全件 `Hardware Composed: Independent Flip`、Dropped 0。Frame ID と PresentCount は別の識別子として保持した。

Clock 原点は PresentMon の生 QPC / ETL 相対時刻から求め、独立した Native ClockSync と Exporter の Raw QPC で検証。整数 µs 表示との差は 100 ns。

| 後段の計測区間 | 平均 ms | p95 ms | Max ms |
|---|---:|---:|---:|
| 最後の GPU Pass Timestamp → IndependentFlip Event | 0.8774 | 1.7601 | 4.5044 |
| CPU Present Start → PresentHistoryDetailed Start | 0.1772 | 0.2474 | 0.8852 |
| PresentHistoryDetailed Start → IndependentFlip Event | 0.8406 | 1.7330 | 4.4770 |

行は重なるので加算しない。IndependentFlip Event は光学的な表示完了ではなく、Input-to-Photon の遅延として扱わない。

最大の Frame 18019 は、GPU 最終 Timestamp が 18,375,371.648 µs、History Start が 18,375,399 µs、Intel の IndependentFlip が 18,379,876 µs。当該 Frame の Calibration 感度は 3.256 µs。GPU Clock 投影感度は全標本で保持し、平均約 1.10 µs、最大 451.648 µsだった。感度は変換誤差の保証上限ではなく、個別標本の制約として残す。

## 6. 最初の記録の失敗と再計測

`NoStack1` は CPU / GPU CSV と前面・Size が正常だったが、User ETW Session に **1,061,095 Event の欠落**があり Exporter も失敗。この ETW は正式な対応付けに採用しなかった。

最初は終了後の大量の CPU Marker と GPU Event が同じ Session だった。再計測では `out/` の Collector コピーだけを変更し、Deferred Marker を別 Session に分離、Buffer を拡張した。Repository の `scripts/capture_spike_etw.ps1` は今回変更していない。

`NoStack2` は Event Lost / Buffer Lost / Decode Error が全て 0。ClockSync 二件込みの Marker 1,233,769 件と、生 CSV の 1,233,767 区間が整合した。正式な結論はこの記録と、欠落のない保存済み記録から出した。

## 7. 改善案と残る調査

次は、**描画 Adapter を明示して Intel / NVIDIA を同条件比較できるようにする案**が有力。既定の高性能優先を一律変更する案にはしない。

- UI と軽い描画では Adapter 間の経路を減らす効果が期待できるが、効果量は未測定。
- 重い 3D Scene では Intel の演算性能が不利になる可能性がある。
- Output 移動や再生成に備え、Adapter LUID、表示先、選択方針を診断情報へ残す。
- 今回 Intel を指定する Engine 機能は追加していない。NVIDIA へ直接接続する表示経路の比較も未実施。

Microsoft は Hybrid 構成の Adapter 間資源共有と CASO による転送削減を説明している。今回のコピー回数・転送帯域・Driver 内部の理由・CASO の実効状態までは未確定。[Hybrid 資源](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/using-cross-adapter-resources-in-a-hybrid-system)、[CASO](https://devblogs.microsoft.com/directx/optimizing-hybrid-laptop-performance-with-cross-adapter-scan-out-caso/)

## 8. 成果物と検証

Root: `out/validation/Performance20261023/Adapter144/`。

- `Inventory/`、`CurrentDisplay.json` / `CurrentDisplayAfter.json`: 読み取り専用 C# 診断。Build の警告 / Error 0、前後の表示構成一致。
- `StartDxg.txt`、`StartMetadata.jsonl`、`QueueAdapterProof.json`: 保存済み 10/21 記録の対応。
- `NoStack1/`: 欠落のため正式不採用の ETW と生 CSV。
- `CaptureSeparated.ps1`: Marker 分離と Buffer 変更を施した Collector コピー。
- `NoStack2/`: 正式 ETL、CPU / GPU CSV、Events.csv、NativeGpu.txt、PresentMon.csv、各 Log。
- `NoStack2/AdapterPresentJoin.json`、`FrameDisplayJoin.jsonl`: 全 Frame の後段時刻と対応検査。
- `NoStack2/GraphQueueProof.json`、`PipelineWindows.json`: Graph 区間内の提出と選択 Frame の Native Event。
- `NoStack2/Verification.json`: 表示構成、ClockSync、CPU 全連番・保持件数、統計。
- `AnalyzeAdapterPresent.py` / `JoinSelected.py` / `InspectPipeline.py` / `VerifyCurrent.py` / `VerifyGraphQueue.py`: 個別解析。

正式記録の前面・Size 不一致 / 状態変化 / Snapshot 追加確保は 0。CPU 連番と保持件数が全件一致し、CPU / GPU 欠落・GPU 上書きは 0。GPU 提出 24,669 件と対応 Frame 数が一致し、不正記録 0。提出未記録の Warmup 二 Frame は別扱い。Native Header の曖昧対応も 0。

Present 対応 Test 三件、CPU 状態分類 Test 三件成功。計測アプリと Collector は停止・Merge 済み。Engine Code 未変更のため Engine Build は再実施していない。Repository の今回の変更は報告と参照だけで、既存の未 Commit 変更を保全した。生記録と個別 Tool は Git 管理外。
