# Desktop: 追加の改善候補と全期間の CPU 区間計測

2026-10-24、Issue #144 調査 Branch。**小さい区間の削減は確認できたが、全体 FPS が確実に改善する候補の採用までは進めない**。以下は既存実装を基準にした今回の実験内の差であり、過去の develop 比較とは別の測定である。

## 条件

- PC: Ryzen 7 3700X / RTX 3060 / 32 GB
- Development x64、同じ共通統計 UI、1280×720、Worker ON、CPU Buffering 2、Update Lead 1
- GPU unfinished 2、SwapChain Buffer 2、Queued Present 2、Waitable ON、VSync OFF、Tearing ON、FPS 無制限
- DISPLAY1、Window (0, 125)、Pointer (545, 504)。前回とは位置が異なるため、前回の絶対 FPS と今回の絶対 FPS を直接比較しない
- 起動時だけ実験条件を固定し、同一 Binary で切り替える
- 各試行 Warmup 2 秒。詳細診断は各 8 秒×1 回、FPS 比較は各 10 秒×3 回
- FPS 比較の順序は 0→1→2→4→7、逆順、再び正順
- 詳細 Trace は Compile ON。FPS 比較時は Sink OFF とし、RAM への区間記録も停止。通常の Compile OFF の Binary と同じ性能である保証はしない
- Logger の File / Debug Output と ImGui Settings 保存は無効

計測用 Recorder は起動前に Thread ごとの固定 RAM を確保し、実行中は時刻と区間を保存する。集計・CSV・標準出力は Shutdown 後だけ実行する。GPU Trace Callback は未登録とし、追加の GPU Clock Calibration は発生させない。既存の GPU Query は有効のまま。

20 試行すべて正常終了、前面 / 可視 / Client Size 不一致 0、Pointer 移動 0、Window 移動 0、試行間の位置一致、Binary Hash 一致を確認。Warmup 後の UI Snapshot 追加確保も全試行 0。詳細記録の容量超過 0。各累積 Frame 数 CSV の時刻と Frame 数は単調だった。

## 何を試したか

| Option | 変更 |
| --- | --- |
| 0 | 基準。今回の最適化候補はすべて無効 |
| 1 | `WindowsHost::graph_performance()` の 5 個の `TimingSamples` 全体コピーを、小さい `TimingStatistics` の取得へ置換 |
| 2 | Queue ごとの成功した Timestamp Frequency を atomic 値に保持し、毎 Frame の Native 再照会を避ける |
| 4 | Owner 専用入力 Vector を移す `take_input_events()` だけ、ImGui Context Mutex の取得を省く |
| 7 | 1 / 2 / 4 をすべて適用 |

Option 1 は全 120 標本・平均・p95・最大・件数を維持し、同じ Mutex 内で集計値を取得する。Option 2 は成功値だけを Queue の寿命内に保持し、失敗を Cache しない。Option 4 は WrongThread / Lifecycle / Frame Open の検証を維持し、ImGui API、Renderer、Snapshot の共有排他には変更を加えない。

## FPS 比較: 詳細記録 OFF

FPS は 3 試行の完了 Render Frame 数の和を、実計測時間の和で割った値。

| 条件 | 平均 FPS | 基準との差 | 各試行の範囲 |
| --- | ---: | ---: | ---: |
| 基準 | 2,379.713 | — | 2,325.269–2,432.350 |
| 統計 Snapshot の複写削減 | 2,405.106 | +1.067% | 2,397.268–2,410.668 |
| Timestamp Frequency Cache | 2,404.504 | +1.042% | 2,384.949–2,423.269 |
| Owner 入力転送の Mutex 省略 | 2,409.792 | +1.264% | 2,386.524–2,439.177 |
| 3 つの組み合わせ | 2,332.049 | -2.003% | 2,307.292–2,352.686 |

基準自体の最小–最大幅は平均の約 4.5%。各単独変更の約 1% の差は、このばらつきより小さい。範囲比較を統計的有意性検定の代わりにはしないが、今回の 3 回だけで全体 FPS の改善確定とはしない。

組み合わせの利益も確認できなかった。個々の区間の削減時間を足し合わせて全体の利益と扱わない。

## 全 8 秒の区間内訳: 詳細記録 ON

以下は終了直前 120 標本ではなく、計測窓内に開始・終了が収まる全区間の平均。境界をまたぐ区間だけ除外する。詳細記録の観測負荷があるため、上表の FPS と混ぜて計算しない。

### 基準の CPU 経過時間

| 区間 | 平均 | p95 | 意味 |
| --- | ---: | ---: | --- |
| Present | 173.660 µs | 240.400 µs | Native Present 呼出区間 |
| Queue Submit | 55.866 µs | 109.300 µs | Queue 提出処理。内部 Signal 等を含む |
| DXGI Ready | 49.146 µs | 100.900 µs | 次の表示枠の待機 |
| Graph Record | 82.253 µs | 160.000 µs | ImGui 記録待ちと実記録を含む |
| ImGui Record Mutex Wait | 38.652 µs | 103.000 µs | Render が Context Mutex を取得するまで |
| ImGui Record Body | 18.626 µs | 26.300 µs | Mutex 取得後の検証・統計追加・Backend 記録 |
| ImGui Platform NewFrame | 24.759 µs | — | Win32 Backend の NewFrame |
| Timestamp Frequency | 5.274 µs | 7.100 µs | Frequency 取得区間 |

これらは CPU の経過時間であり、CPU Cycle や GPU 実行時間そのものではない。Preemption / 待機を含む。親子区間は重なるため加算しない。例えば Graph Record の中に ImGui Record Wait / Body があり、Queue Submit には Signal が含まれる。

### 候補で短くなった区間

| 区間 | 基準 | 単独候補 | 差 |
| --- | ---: | ---: | ---: |
| Editor Statistics（Option 1） | 3.652 µs | 3.152 µs | 約 -0.500 µs |
| Timestamp Frequency（Option 2） | 5.274 µs | 約 0.039 µs | 約 -5.235 µs |
| Owner Input Transfer Wait（Option 4） | 1.961 µs | 約 0.048 µs | 約 -1.913 µs |

局所的な削減は見える。一方で ImGui Record Wait は基準 38.652 µs に対し、Option 2 では約 42.99 µs、Option 4 では約 40.77 µs、組み合わせでは約 42.98 µs だった。進行タイミングの変化で別の場所の待ちが変わる可能性がある。

これを単独変更の回帰原因と断定はしない。各診断条件は 1 回ずつで、Main / Render の保持区間の重なりや OS Scheduling を直接対応付けた解析はまだ行っていない。

## 判断と次の優先対象

1. **ImGui Context の Main / Render 競合**: 約 39 µs の取得待ちが約 19 µs の実記録より大きい。描画コマンド生成自体の高速化より、Main の Context 保持と Render 記録の重なりを調べる優先度が高い。Frame 全体を直列化したり Context Mutex を無条件に外したりしない
2. **Frequency Cache**: 約 5 µs の局所削減が明確な候補。ただし Native 周波数との長時間一致 / Queue 再生成 / Device Lost / Copy Timestamp 非対応を含む検証は残る。今回の全体 FPS の差だけで採用しない
3. **Owner 入力 / 統計 Snapshot**: 所有境界や不要な複写を整理する余地はあるが、大きな FPS 改善策とは位置付けない
4. **Present / Queue Submit**: 依然として大きい区間。今回これらの内部待機や Driver 処理を分離しておらず、Native 呼出自体を高速化できると断定しない

ImGui の競合を減らす設計が次の大きな調査対象。CPU 経過時間から削減可能な GPU 時間や入力遅延を推定しない。調査候補の正式実装は、別 Issue / develop 起点 Branch で行う既存の方針に従う。

## 変更と検証

- 調査専用 Header と、3 候補の `CUE_DESKTOP_INVESTIGATION` 条件分岐、区間名の識別、専用 Harness / Recorder / Runner を追加
- 通常 Build では候補の条件分岐と Queue の Cache Member は無効。調査コードの切替であり、通常実装へ採用していない
- `pwsh -NoProfile -File scripts/codex_build.ps1`: Debug Build 成功
- Debug の関連 CTest 6 件成功: TimingTrace、TimingSamples、InputLatency、DX12 QueuePool、FramePacing、ImGuiManager
- 実験用 Development Build 成功、上記 20 試行すべて正常終了
- 既存の未 Commit `SpikeTimingProbe.cpp` は SHA-256 で変更されていないことを確認
- Commit / Push / Merge / 新規 Branch / Worktree は実行していない

未実施: 通常 Trace Compile OFF における同一候補の長時間反復、実験条件での実入力・Resize・停止失敗経路、Frequency Cache の長時間 Native 値一致、GPU の全期間分布、全 Frame の FPS p95 / Hitch 頻度。今回の短時間・小負荷の結果をゲーム全体へ一般化しない。

## 成果物

Root: `out/validation/Performance20261024/HotspotStudy/`

- `OriginalFiles.json` / `Original/`: 作業前の 4 File の完全コピーと Hash
- `Harness/`: 共通 UI、固定 RAM Recorder、実験用 CMake
- `RunStudy.ps1`: 同条件の Profile / Throughput Runner
- `Runs-20261024-212236/Summary.json`: 全 20 試行と平均値、各区間の全期間集計
- 同 Folder の `.log` / `.csv` / `-metrics.csv` / `.json`: 終了後保存した個別結果、Thread ごとの区間分布、Hash と条件
