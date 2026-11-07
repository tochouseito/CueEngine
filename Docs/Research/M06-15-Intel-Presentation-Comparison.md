# M06-15 / Issue #144 — Intel 描画・表示と RTX 描画の比較

- 測定日: 2026-10-23
- Branch: `m06/issue144/gpu-waits-and-telemetry`、HEAD: `6effec9`
- 前回: [描画 Adapter と表示経路](M06-15-Adapter-And-Presentation-Path.md)
- 要求: 両方を Intel にして計測する。Double Buffering・Waitable ON・通常 UI と統計品質を維持する
- Commit / Push / Merge は行っていない。開始時の未 Commit 変更を保全した

## 1. 結論

**Intel に描画・表示をそろえた測定を実施した。ETW なし各 20 秒・3 回の合算は Intel 1,438.67 FPS、RTX 1,173.55 FPS。今回の比較では Intel が約 22.6% 高いが、目標の 2,000 FPS は未達。**

**最後の GPU Pass Timestamp → IndependentFlip Event は Intel 平均 0.09297 ms。今回の RTX 対照記録は 1.21402 ms、前回の RTX 記録は 0.87740 msだった。** 前回の値を固定の普遍的なコストとは扱わず、今回の対照記録も併記する。

同一 GPU にそろえることは今回の軽い Scene では有効だった。一方、Intel は GPU Pass 全体区間が平均約 0.18～0.19 ms と、RTX の約 0.03～0.05 ms より長い。GPU 開始前の待ちも増えており、後段の時間短縮をそのまま FPS 改善量へ置き換えられない。重い 3D Scene でも Intel が有利という結論ではない。

## 2. 実装した計測経路

既定の Device 選択を高性能優先のまま、生成時の差し替えだけを用意した。

- `AdapterSelection::HardwareMinimumPower`: 省電力順の Hardware を試す。Hardware が作れない場合は失敗し、この指定では WARP へ Fallback しない
- `WindowsHostCallbacks::createBackend` と `EditorHostConfig::createBackend`: 初期化 Thread で一度だけ Factory を呼び、Backend の一意所有権を Host に渡す。公開型は抽象 `IBackend` / `IFileSystem` に限定する
- `CueSpikeTimingProbe` の末尾引数 `adapter`: `auto` / `intel` / `nvidia`。省略時は従来の経路。明示指定時は実 Device の Vendor と Software Flag を検査し、指定と違えば計測を開始しない
- 起動 Log に Vendor / Device / Adapter LUID / Software Flag を記録する
- 高 FPS でも全記録を保持するため、通常の長時間 Probe の CPU 容量を Thread 当たり 2,097,152 区間、GPU 容量を 524,288 Record に拡張した。計測前に確保し、間引きは行っていない

ImGui の表示内容、直近 120 標本の平均・p95・Max・件数、GPU Query、入力から反映 Frame への計測は保持した。無操作測定なので入力遅延の改善は主張しない。Factory Callback は Frame ごとの処理には入らない。

生成 Factory の失敗と空の成功値で初期化を拒否し、Window / UI を回収する Test を追加した。

## 3. 共通条件と同一性

| 条件 | 値 |
|---|---|
| 構成 / UI / Window | Development / 通常 UI / 1280 × 720 |
| CPU Snapshot / GPU 資源枠 | 2 / 2 |
| Worker / Update lead | ON / 1 |
| 未完了 GPU 上限 / SwapChain / Queued presents | 2 / 2 / 2 |
| 表示 | Waitable ON、VSync OFF、Tearing ON、FPS 無制限 |
| 時間 | 各回 Warmup 2 秒 + 計測 20 秒 |
| 詳細記録 | 全 CPU 区間 CSV / GPU 生 Query・Clock 対応 CSV。通常 6 試行は ETW なし |
| Binary | Trace 対応の同じ Development 実行 File、全 8 正式試行で SHA256 一致 |

SHA256: `018D28BE040A91BD0BB273E5F7B0C14F4900C978C4C21E38EB5D5A1E4F39C29F`。

起動順は Intel → RTX → Intel → RTX → Intel → RTX。OS / Driver / GPU Clock / 電源方針を変更せず、実行間の変動を含む比較として扱った。Compile 時から Trace OFF の性能比較ではない。

- Intel: Vendor `8086`、Device `9A60`、LUID `0x000000000000DCC8`、Software 0
- RTX: Vendor `10DE`、Device `2520`、LUID `0x000000000000DF8F`、Software 0
- 測定前後の Active Display Path は Source / Target とも Intel。Adapter 一覧と Active Path は一致した
- ETW の計測期間 10% / 30% / 50% / 70% / 90% にある 5 Frame の Graph.Execute 内を検査。Intel 試行の Native Command Packet 5 件は Intel、RTX 試行の 15 件は RTX の Device / Context に対応した
- 対象 Surface の IndependentFlip Context は、両試行とも Intel に対応した

したがって比較は **Intel 描画 → Intel 表示** と **RTX 描画 → Intel 表示**。RTX 描画 → RTX 直結表示は今回測定していない。

## 4. ETW なしの交互 3 組

Frame 間隔は、計測中に保持した連続 Render 完了点から算出した。FPS は完了 Frame 数 / 実経過時間であり、瞬間 FPS の平均ではない。

| 試行 | GPU | Frame 数 | 秒 | FPS | Frame 間隔 平均 / p95 / Max ms |
|---|---|---:|---:|---:|---|
| Intel2 | Intel | 27,216 | 20.000438 | 1,360.770 | 0.7349 / 0.9031 / 4.8729 |
| Nvidia1 | RTX | 26,371 | 20.000463 | 1,318.519 | 0.7584 / 1.0377 / 6.1977 |
| Intel3 | Intel | 31,787 | 20.000379 | 1,589.320 | 0.6292 / 0.7944 / 3.4559 |
| Nvidia2 | RTX | 21,835 | 20.000590 | 1,091.718 | 0.9160 / 3.2689 / 5.3307 |
| Intel4 | Intel | 27,319 | 20.000431 | 1,365.921 | 0.7321 / 0.9313 / 5.3642 |
| Nvidia3 | RTX | 22,209 | 20.000762 | 1,110.408 | 0.9006 / 4.0434 / 5.5772 |

各回の平均・p95・Max・件数、待機区間、全 CPU 連番検査は `TrialAnalysis.json`、GPU Pass 全件統計は `GpuComparison.json` に保存した。

| 合算 | Intel | RTX |
|---|---:|---:|
| 総 Frame 数 | 86,322 | 70,415 |
| 総秒数 | 60.001248 | 60.001815 |
| FPS | **1,438.670** | **1,173.548** |
| 平均 Frame 間隔（総時間 / 総数）ms | **0.69509** | **0.85212** |

今回の観測上の差は FPS +22.6%、平均 Frame 間隔 -18.4%。各試行の変動があるため、一般的な改善率として保証しない。Intel にそろえても約 5 ms のスパイクは残った。

GPU Pass 全体区間の各回平均は、Intel 0.1853 / 0.1854 / 0.1826 ms、RTX 0.0273 / 0.0464 / 0.0371 ms。Intel では Render 内の Fence Event 待機も平均約 0.14 ms 生じ、RTX の 3 試行では同じ待機区間は 0 だった。これは今回捕捉した区間の比較で、全ての GPU 待機が恒久的に不要という意味ではない。

## 5. 表示経路の ETW 比較

追加の各 20 秒は Stackwalk / Sampling なし、Kernel Scheduling / GPU / 表示 Event を採取した。通常 6 試行の合算 FPS へ混ぜない。

| 条件 / 計測区間 | IntelEtw2 | NvidiaEtw1 |
|---|---:|---:|
| PID | 20084 | 21924 |
| FPS / Frame 数 | 1,427.383 / 28,548 | 909.894 / 18,198 |
| Present API 平均 ms | 0.04198 | 0.13365 |
| Present Start → IndependentFlip 平均 ms | 0.89612 | 1.36061 |
| CPU 提出区間終了 → 最初の GPU Pass Timestamp 平均 ms | 0.62489 | 0.11530 |
| GPU Pass 全体区間 平均 ms | 0.19200 | 0.04354 |
| GPU 最終 Timestamp → IndependentFlip 平均 ms | **0.09297** | **1.21402** |
| 同 p95 / Max ms | **0.10956 / 1.67852** | **4.52210 / 6.15542** |
| Present Start → History Start 平均 ms | 0.03577 | 0.17955 |
| History Start → IndependentFlip 平均 ms | 0.86035 | 1.18105 |

区間の一部は重なるので表を加算しない。IndependentFlip Event は光学的な表示完了ではなく、Input-to-Photon でもない。CPU 提出から最初の GPU Pass までには Queue 内の待ちや Pass 前の処理などが含まれ、その内訳は未特定。

**後段は短くなったが、Intel 側の GPU 開始前と描画に時間がかかる。** 後段だけを見ると大幅な短縮でも、Frame 全体ではその全てが短縮されるわけではない。Double Buffering の Frame は重なるため、これらの遅延から 1 / 時間で FPS 上限を直接求めない。

両試行とも計測対象 Present 全件を一意対応し、全件 `Hardware Composed: Independent Flip`、Dropped 0。CPU Present → Native Present → PresentCount → 対象 Surface の IndependentFlip → GPU Query を照合した。起動 / Warmup の IndependentFlip 系列に入らない呼出は明示的に計測対象外とした。

GPU Query と CPU Clock 対応の感度は全件保持した。最大感度は Intel 約 15.77 µs、RTX 約 230.82 µsであり、厳密な変換誤差の保証上限とは扱わない。Clock offset drift はそれぞれ 100 ns / 50 ns。

## 6. 記録品質と不採用試行

- `Intel1`: Warmup 中に前面を失ったため除外。Exit 11
- `IntelEtw1`: Windows の前面取得制限で開始できず除外。Exit 11
- 正式な通常 6 試行と追加 ETW 2 試行は、前面・可視・非最小化を維持。Window 状態 11、Size 不一致 / 状態変化 / Snapshot 追加確保は全て 0
- 全 8 試行で CPU 連番が 0 から連続し、CSV 件数と保持件数が一致。CPU / GPU CSV の Dropped / GPU 上書きは 0。GPU 生 Record の status は全件正常、全提出 Frame に二つの Pass が対応した
- GPU Query だけ先に記録された Warmup は測定対象の提出 Frame と区別。CPU Frame.Render の区間全体が計測開始をまたぐ境界では Present / 提出の件数と 1 件異なるため、集計区間の定義を保持した

### Intel の Deferred Marker 欠落の扱い

`IntelEtw2/Merged.etl` では、終了後に全 CPU 区間をまとめて出す **Marker Session に 497,293 Event の欠落**があった。元の統合 ETL の Exporter は Exit 4、xperf も欠落を検出し拒否した。この状態を正常な全 ETW として扱っていない。

一方、元の `Kernel.etl` / `UserGpu.etl` は別 Session で保存され、Stop Log に欠落はなかった。この二つだけを `GpuOnly.etl` へ統合して再検証した。

- `GpuOnly.etl`: ProcessTrace 0、Decode Error / Event Lost / Buffer Lost 全て 0、CSwitch 932,786、ReadyThread 567,697
- GPU / 表示の抽出と PresentMon を `GpuOnly.etl` から再生成して時刻原点を統一
- CPU 区間は欠落した ETW Marker から復元せず、元の全件 CSV と Clock 対応 Log を使用
- 元の欠落 ETL、Marker Stop Log、拒否した Export Log も保存

これにより表示経路の対応付けは欠落のない Native 記録を使用し、CPU 区間情報は全件 CSV で保全した。Intel の全 CPU Marker ETW を使う Stack / Scheduling 解析が必要なら別途再記録が必要であり、今回その解析は行っていない。

RTX の統合 ETL は Decode Error / Event Lost / Buffer Lost 全て 0、Marker 908,628（CPU 区間 908,626 + ClockSync 2）、CSwitch 1,308,182、ReadyThread 756,750。

## 7. Build / Test / 成果物

- 必須 Debug Build: `pwsh -NoProfile -File scripts/codex_build.ps1`。最初の Sandbox 実行は MSVC PDB 通信の C1902 で失敗。通常環境での再実行成功
- Development の `CueSpikeTimingProbe` / `CueSpikeEtwExport`: Trace ON で Build 成功
- Debug の Device / WindowsHost / EditorHost Lifecycle Test: **3 件成功**。Factory 失敗 / 空成功の回収を含む
- 計測終了後は Configure Cache を Trace OFF に戻した。測定 Binary 自体は Trace ON のまま保存
- 最後の Header 依存明示は `<functional>` の Include 追加のみ。測定時 Source Hash と測定 Binary を保存し、最終 Source は Debug で再 Build 確認
- 全アプリ・Collector は停止・Merge 済み。OS の GPU / 表示設定は変更していない

Root: `out/validation/Performance20261023/Intel144/`。

- `RunTrial.ps1` / `CaptureCompare.ps1`: 再現用の起動と Collector。Collector の変更は `out/` のコピーだけ
- `Intel2` / `Nvidia1` / `Intel3` / `Nvidia2` / `Intel4` / `Nvidia3`: Log、全 CPU / GPU CSV、起動引数 / Hash / Exit
- `TrialAnalysis.json` / `EtwTrialAnalysis.json`: 全区間の統計と連番 / 件数検証
- `GpuComparison.json`: 全 8 試行の GPU Pass / 全体区間統計、Adapter、表示構成、Hash の検証
- `IntelEtw2` / `NvidiaEtw1`: 生 ETL、PresentMon、NativeGpu、AdapterPresentJoin、全 FrameDisplayJoin、GraphQueueProof
- `CurrentDisplayBefore.json` / `CurrentDisplayAfter.json`: 読み取り専用の表示構成照会
- `MeasuredProbe.exe` / `MeasuredSourceHashes.json` / Build・Test Log: 測定 Binary と検証の追跡情報

## 8. 次に残ること

今回の軽い描画では GPU 間の経路を避ける効果を観測したが、Intel への一律変更を最終方針にはしていない。RTX の描画性能を保って表示経路もそろえる RTX 直結表示の比較、Intel 側の GPU 開始待ち・Fence 待機の内訳、残る 4～5 ms 級スパイクは未解決。重い Scene の評価も未実施。
