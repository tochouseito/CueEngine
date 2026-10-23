# CueEngine 引き継ぎ書 — GPU 待機・統計・表示経路の性能調査

- 更新日: 2026-10-23（日本時間）
- Repository: <https://github.com/tochouseito/CueEngine>
- Workspace: `C:\Users\sinse\source\repos\CueEngine`
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- 作業 Branch: `m06/issue144/gpu-waits-and-telemetry`
- 統合先: `develop`。**本書を含む追加分は作業 Branch へ Commit / Push し、Merge は行わない**
- 追加分の親 Commit: `6effec98a6cf93d8f8e8eb54fed23251ce35e7ca`
- 本書を追加した Commit が今回の提出単位。正確な Hash は `git log -1 --format=fuller -- Docs/Research/CueEngine-Handoff-2026-10-23.md` で取得できる

本書は別の会話から調査を再開するための資料であり、新しい作業の実行指示ではない。次の会話のユーザー依頼と Repository の `AGENTS.md` を優先する。過去の調査書にある「未 Commit / 未 Push」「次は Intel 比較」などは、その調査時点の状態である。

## 1. 最初に把握すること

**目標は Development・Double Buffering・Waitable ON・通常 ImGui UI で平均 2,000 FPS。現在は未達。**

最新の同条件・各 20 秒 × 3 回の比較では、RTX 描画 → Intel 表示が **1,173.55 FPS**、Intel 描画 → Intel 表示が **1,438.67 FPS**。今回の観測では約 22.6% 高かった。

描画と表示の GPU をそろえる効果は観測した。一方、Intel 側では GPU Pass と GPU 開始前の時間が増える。**既定 GPU を Intel へ一律変更した実装ではない**。通常の高性能優先は継続し、計測 Probe が生成時だけ明示選択する。

スパイクの原因は一つではない。前面切替直後の Device 排他待ち、一件の ETW Stackwalk 自身によるページ読み込み、Hybrid GPU の表示経路を確認したが、通常時の 4～5 ms 級や過去の 30～50 ms 級を全て解決した状態ではない。

入口は本書。数値と根拠の詳細は [Intel 比較](M06-15-Intel-Presentation-Comparison.md)、過去の累積作業は [統合記録](M06-15-Work-Summary.md)を参照する。

## 2. ユーザーの合意と制約

- 「実装をお願いします」と言われるまでは質問として扱う、という継続方針がある。今回の Intel 比較に必要な計測経路の追加は、Intel で計測する依頼に伴って実施した。未検証の最適化を勝手に実装しない
- 最新の依頼は **Merge せず Push し、これまでの調査を含めた `.md` の引き継ぎ書を作ること**。次の実装・実機測定・Git 操作は次のユーザー依頼に従う
- 統計品質を落とさない。直近 120 標本の平均・nearest-rank p95・Max・件数を保持し、GPU Query や通常 UI の更新を間引いて FPS を稼がない
- 入力遅延は **Window の入力イベント到達 → それを反映した Frame の CPU 処理 / Present**。物理操作からの時間や Input-to-Photon ではない
- 再計測では Double Buffering / Worker ON / Waitable ON を維持する。Single や Waitable OFF の値で目標達成と扱わない
- 前面取得が Windows に拒否される場合はユーザーのクリックが必要。過去の「操作できる」という回答を、別の日時にも無期限で利用可能という意味にしない
- 日本語で回答・First-party コメントを記述する。コードを変更する場合は [コーディング規約](../../Engine/Documents/CODING_RULES.md)、[ADR-0001](../Decisions/0001-architecture-boundaries.md)、[ADR-0002](../Decisions/0002-build-system-source-of-truth.md)を確認する
- 作業開始時に Branch と未 Commit 変更を確認し、他の変更を保全する。コード変更後の必須確認は `pwsh -NoProfile -File scripts/codex_build.ps1`

## 3. Branch に含まれる変更

### 既に `6effec9` に含まれるもの

詳細は [統合記録の実装節](M06-15-Work-Summary.md)を参照する。

- `TimingSamples` の全標本を維持しつつ、統計取得時の全コピー / Sort を避ける変更。取得は定数時間、標本追加は最大 120 要素の移動を伴う
- GPU Fence / DXGI Event を使う待機、実処理と待機区間を分ける計測
- 入力 Event ID と反映 Frame の対応、および入力到達から CPU / Present までの統計
- CPU Span、GPU Query / Calibration、ETW の記録・解析基盤
- Present 後の入力計測で余分な ImGui Mutex 再取得を避ける変更。以前約 6.1 ms 待った一件の経路は修正済み

### 今回追加して Push するもの

| 範囲 | 内容 |
|---|---|
| `DX12CommandPool.cpp` / `DX12QueuePool.cpp` / `DX12MainFrameGraph.cpp` | Submit 全体・Pool Lock・Queue Lock・Signal API・提出後登録を分ける Trace |
| `FocusExperiment.h` / `SpikeTimingProbe.cpp` | 補助 Window と前面切替、実測前面 PID / Size の検査、必要に応じた 960 × 720 比較 |
| `capture_spike_etw.ps1` | FocusCycle、CPU Sampling、比較 Window / Size、FocusCycle 時の Marker Session 分離・Buffer 拡張 |
| `analyze_focus_cycle.py` / `analyze_present_modes.py` と Test | 前面切替の条件成立、巨大な QPC 値、曖昧な Present 対応を成功扱いしない検査 |
| `analyze_spike_etw.py` | 追加した Submit / Signal 等の区間を解析対象へ追加 |
| `DX12RenderDevice` | `HardwareMinimumPower` を追加。省電力順で Hardware を試し、この指定では WARP へ Fallback しない |
| `WindowsHostCallbacks::createBackend` / `EditorHostConfig::createBackend` | 初期化時だけ Backend Factory を差し替え、一意所有権を Host へ移す。公開契約は抽象型を使う |
| Probe の `adapter` 引数 | `auto` / `intel` / `nvidia`。明示指定では実 Device の Vendor / Software Flag を検査し、LUID とともに記録する |
| Probe の記録容量 | 長時間時の CPU 区間は Thread 当たり 2,097,152、GPU Record は 524,288。事前確保し、標本を間引かない |
| `EditorHostTests.cpp` | Factory 失敗と空の成功値でも初期化を拒否し、Window / UI を回収する検証 |
| `Docs/Research/M06-15-*.md` | 追加の前面切替・Stack・Adapter・2,000 FPS 条件・Intel 比較の調査書 |

FrameGraph / RHI の Timestamp Frequency Cache、Completion 再利用、Lock 範囲変更、通知先分割などの性能改善候補は **未実装**。

## 4. 固定した測定条件

| 項目 | 条件 |
|---|---|
| Build | Development、通常 UI、ClearFinalColor と ImGui の軽い Scene |
| Window | 原則 1280 × 720、前面・可視・非最小化を維持 |
| CPU Snapshot / GPU Resource Frame | 2 / 2 |
| Worker / Update lead | ON / 1 |
| 未完了 GPU 上限 / SwapChain Buffer / Queued presents | 2 / 2 / 2 |
| 表示 | Waitable ON、VSync OFF、Tearing ON、FPS 上限なし |
| 通常試行 | Warmup 2 秒 + 測定 20 秒 |
| 指標 | 完了 Frame 数 / 実経過時間。瞬間 FPS の平均ではない |
| UI / 統計 | 通常 UI と 120 標本の情報品質を維持 |

960 × 720、FocusCycle、Stackwalk / Sampling の記録は原因を調べる別条件。通常時の FPS 比較へ混ぜない。最初の約 650 FPS と最新値は測定条件が異なるため、差の全てをコード改善率として報告しない。

## 5. これまでに確認した原因と制限

| 調査 | 確認できたこと | 未確定 / 注意 |
|---|---|---|
| 前面切替の制御比較 | 安定した非前面だけで継続的に大幅悪化する結果は得られず、切替直後に約 25～31 ms を再現 | 非前面化だけで過去の全スパイクを説明しない |
| DWM CPU Stack | DWM → D3D11 の Shader 作成 / DrawIndexed → Intel `igc64.dll` Compiler の実 Stack を確認 | Cache miss、Compiler 内部、Driver 不具合は断定できない |
| Device 排他 | 補助 Window / メモ帳の計八切替で、七件が Engine の Signal、残る一件が Present 内部の Signal 提出。Device 排他を待ち、System の ProcessStatusChange から解放される | Driver 内部で排他保持が長くなる根本理由は未解決 |
| Stackwalk 由来の I/O | Frame 27222 の約 9.4 ms 表示準備待ちに、Present worker の ETW Stack 展開中の Hard Fault / NTFS I/O 待ち約 8.76 ms が含まれた | この一件を通常実行の性能値や全スパイクの原因にしない。Backing File は未特定 |
| Stack なし基準 | Stackwalk を外しても通常時に約 5 ms が残る | 全てが ETW の影響という結論ではない |
| Adapter 照合 | Graph の描画は RTX、対象 IndependentFlip と Active Display Path は Intel | 純粋な GPU 間コピー時間・コピー回数・CASO の実効状態は未確定 |
| Intel 比較 | 同一 GPU にそろえた軽い Scene で FPS と後段遅延が改善 | 2,000 FPS は未達。重い Scene や RTX 直結表示は未測定 |

過去の CPU 提出終了 → GPU 最初の Timestamp 約 8.8 / 30.4 ms、約 49.6 ms のスパイクにも未解決の事例がある。最新の 20 秒で再現しなかったことは、その原因解消の証明ではない。

## 6. 最新の比較結果

### ETW なし、同じ Binary で交互に各 3 回

| 描画 → 表示 | 各回 FPS | 合算 Frame / 秒 | 合算 FPS |
|---|---|---|---:|
| Intel → Intel | 1,360.770 / 1,589.320 / 1,365.921 | 86,322 / 60.001248 | **1,438.670** |
| RTX → Intel | 1,318.519 / 1,091.718 / 1,110.408 | 70,415 / 60.001815 | **1,173.548** |

今回の比較では FPS +22.6%、平均 Frame 間隔は 0.85212 → 0.69509 ms（約 18.4% 短縮）。実行間変動を含む観測であり、保証値ではない。Intel の最悪 Frame 間隔は今回の三回中で約 5.36 ms、RTX は約 6.20 msだった。

### 追加 ETW の対応区間

| 平均 ms | Intel → Intel | RTX → Intel |
|---|---:|---:|
| Present API | 0.04198 | 0.13365 |
| CPU 提出区間終了 → GPU 最初の Pass Timestamp | 0.62489 | 0.11530 |
| GPU Pass 全体区間 | 0.19200 | 0.04354 |
| GPU 最終 Timestamp → IndependentFlip Event | **0.09297** | **1.21402** |
| Present Start → IndependentFlip Event | 0.89612 | 1.36061 |

前回の RTX 記録では後段は平均 0.87740 msだった。前回と今回の RTX 値が異なることも保持し、0.877 ms を固定の Flip コストと扱わない。

区間の一部は重なるので表を加算しない。GPU 最終 Pass の後には転送・同期・Driver / 表示側処理などが含まれる。IndependentFlip Event は光学的な表示完了ではなく、遅延から単純に `1 / 時間` で FPS 上限を求めない。Intel では後段が短くても、GPU 開始前と描画に時間がかかっている。

ETW 試行は Intel 1,427.383 FPS / 28,548 Frame、RTX 909.894 FPS / 18,198 Frame。上の ETW なし 3 回合算へ混ぜない。対象 Present は全件一意対応、両方とも `Hardware Composed: Independent Flip`、Dropped 0。

## 7. Hardware と表示の意味

- RTX: NVIDIA GeForce RTX 3060 Laptop GPU、Vendor / Device `10DE / 2520`、当日の LUID `0x000000000000DF8F`
- Intel: Intel UHD Graphics、`8086 / 9A60`、当日の LUID `0x000000000000DCC8`
- Intel は Hardware GPU。WARP の Software 描画ではない
- `QueryDisplayConfig` の Active Source / Target と ETW の IndependentFlip Context は Intel。DXGI `EnumOutputs` は NVIDIA の Output として `DISPLAY1` を返し、見え方が異なる。Output 名だけで接続先を断定しない
- Intel 比較では、Device LUID に加え五つの Frame の Native Command Packet と表示 Context を照合し、両方 Intel であることを検証した
- LUID は現在の照会値で再検証する。別環境や再起動後まで値を固定しない
- RTX が描画と表示の両方を担当できるかは、機種の MUX / Advanced Optimus、内蔵画面の接続、外部映像端子の配線に依存する。この PC の対応は未確認
- Device / SwapChain を RTX で作るだけでは、物理的な表示接続先を変更できない

## 8. 記録品質で外せない注意

1. 前面 / Size / Exit / Snapshot 追加確保 / CPU 連番 / 保持件数 / GPU status・欠落・上書き / ETW Lost・Decode Error を確認する。条件不成立の試行を成功値にしない
2. GPU Query のみ先に捕捉された Warmup と、計測対象の提出 Frame を区別する。開始境界をまたぐ `Frame.Render` は Present / 提出件数と 1 件異なる場合がある
3. GPU / CPU Clock の対応と感度を保持する。最新の最大感度は Intel 約 15.77 µs、RTX 約 230.82 µs。誤差の保証上限とは扱わない
4. UI の `ImGui GPU fence wait` は **実際に待った回だけの条件付き平均**。全 Frame の負担ではなく、Warmup の標本が残る場合もある。RTX の保存記録では待機チェック全体は平均約 0.265～0.277 µsだった。Intel でも同じ負担と推定しない
5. Stackwalk / Sampling は原因を絞る手段で、通常実行の基準値とは分ける。事実を隠すために計測を削除せず、同条件で比較する

### 最新の Intel ETW の欠落

`IntelEtw2/Merged.etl` は、終了後に大量出力した **CPU Marker Session に 497,293 Event の欠落**があり、Exporter は Exit 4。この統合 ETL を正常な全記録とは扱わない。

元の `Kernel.etl` / `UserGpu.etl` は別 Session で欠落がなかったため、この二つだけを `GpuOnly.etl` へ統合して再検証した。ProcessTrace 0、Decode Error / Event Lost / Buffer Lost は全て 0。表示経路と PresentMon はこの欠落のない ETL から生成し、CPU 区間には全件保持された元 CSV と Clock Log を使用した。元の欠落記録も保存した。

Intel の欠落した CPU Marker を使う Stack / Scheduling 解析が必要なら再収集が必要。欠落を無視する `xperf -tle` で正常扱いにしない。RTX の最新統合 ETL は Lost / Decode Error 0。

## 9. 検証済みの状態と再開コマンド

### Build / Test

- 10/23 の最終 Debug Build 成功。最初の Sandbox 内 Build は MSVC PDB 通信の C1902 で失敗し、通常環境で同じ Build を再実行して成功した
- Development の Probe / ETW Exporter は Trace ON で Build 成功
- Debug の `Cue.Renderer.DX12.RenderDevice`、`Cue.WindowsHost.Lifecycle`、`Cue.EditorHost.Lifecycle` の **3 Test 成功**。全 CTest を今回再実行したわけではない
- 引き継ぎ作成時に `python -B -m unittest discover -s scripts -p 'test_analyze_*.py'` の **14 Test 成功**
- 最終 Source の Build 後、この引き継ぎ作成で C++ / HLSL / CMake は変更していない
- Configure Cache は現在 `CUE_ENABLE_TIMING_TRACE=OFF`。既存の Development Probe は Trace ON の Binary。Cache と Binary の状態を混同しない
- 計測アプリ / Collector は終了済み。OS / Driver / 表示設定は変更していない

```powershell
Set-Location C:/Users/sinse/source/repos/CueEngine
git status --short --branch
git log -3 --oneline

# コード変更後の必須確認
pwsh -NoProfile -File scripts/codex_build.ps1

# 計測 Binary を作り直す場合だけ Trace を有効にする
. scripts/vcpkg_environment.ps1
$env:VCPKG_ROOT = get_vcpkg_root
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=ON
cmake --build --preset windows-vs2026-development --target CueSpikeTimingProbe CueSpikeEtwExport

# 計測を終えたら通常設定へ戻す。これだけでは既存 Binary は作り直されない
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=OFF
```

測定 Binary の SHA256 は `018D28BE040A91BD0BB273E5F7B0C14F4900C978C4C21E38EB5D5A1E4F39C29F`。`out/validation/Performance20261023/Intel144/MeasuredProbe.exe` に保存済み。再 Build 後に Hash が変わる場合は新しい比較単位にし、別 Binary の差をそのまま Adapter の効果にしない。

### Probe の引数

```text
buffers waitable queued ui seconds prefix traceEnabled
[emitEtw [requireForeground [deferEtw [triggered [renderMs [gpuMs
[focusCycle [focusWindow [focusWidth [adapter]]]]]]]]]]
```

通常 UI は `ui=0`。`adapter` は `auto` / `intel` / `nvidia`。全前段の省略可能引数を埋めて末尾へ渡す。

**測定を依頼された場合の例**（前面操作を要するため、引き継ぎを読んだだけで起動しない）:

```powershell
$repoRoot = 'C:/Users/sinse/source/repos/CueEngine'
$probe = "$repoRoot/out/build/windows-vs2026/Engine/Tests/Development/CueSpikeTimingProbe.exe"
$runRoot = "$repoRoot/out/validation/IntelComparisonNew"
New-Item -ItemType Directory -Path $runRoot -ErrorAction Stop | Out-Null
$runPrefix = "$runRoot/Intel1"
$probeArgs = @('2','1','2','0','20',$runPrefix,'1','0','1','0','0','20','5','0','0','1280','intel')
$hash = (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash
$process = Start-Process -FilePath $probe -ArgumentList $probeArgs -WorkingDirectory $repoRoot -RedirectStandardOutput ($runPrefix+'.log') -RedirectStandardError ($runPrefix+'.err') -PassThru
@{pid=$process.Id;sha256=$hash;args=$probeArgs;started=(Get-Date -Format o)} | ConvertTo-Json -Depth 4 | Set-Content ($runPrefix+'-launch.json') -Encoding utf8
$process.WaitForExit()
@{pid=$process.Id;exit_code=$process.ExitCode;completed=(Get-Date -Format o)} | ConvertTo-Json | Set-Content ($runPrefix+'-done.json') -Encoding utf8
```

この例の固定 Path は空白を含まない。別 Path へ変更する際は引数の Quote も確認する。RTX は新しい Prefix で末尾を `nvidia` にする。前面取得は最大 45 秒待ち、拒否 / Warmup 中の前面喪失は Exit 11。ユーザーに対象 Window を一度クリックしてもらい、約 22 秒は操作を止めて測る。成功した `Start-Process` 経路を使い、別 Window の入力を疑似送信して前面制限を回避しない。

### ETW / 解析 Tool

- `scripts/capture_spike_etw.ps1` は通常権限で Probe、UAC で Collector を起動する。既存 Kernel Session がある場合は保全のため停止する設計
- **Repository 版 Collector に `-Adapter` はない**。最新 Intel 比較では `out/.../Intel144/CaptureCompare.ps1` のローカルコピーを使った。単純に Repository 版へ `-Adapter intel` を渡さない
- `CaptureCompare.ps1` は Deferred Marker を独立 Session に分け、Buffer 拡張と `Start-Process` 起動を追加したコピー。ただし上記 Marker 欠落は残るため、完全な CPU Marker ETW が必要なら再収集方法の改善が必要
- `C:/Program Files (x86)/Windows Kits/10/Windows Performance Toolkit/xperf.exe`
- PresentMon 2.6: `out/validation/Performance20261020/Present144/Tools/PresentMon-2.6.0-x64.exe`。`--etl_file ... --process_id ... --v1_metrics --qpc_time --no_console_stats --output_file ...` を使用
- `CueSpikeEtwExport.exe ETL Events.csv` は Lost / Decode Error で失敗する。結果と Exit を保存する
- `python -B scripts/analyze_gpu_timeline.py PREFIX`
- `python -B scripts/analyze_present_modes.py PREFIX PresentMon.csv OUTPUT_PREFIX`
- `python -B scripts/analyze_focus_cycle.py --help` / `scripts/analyze_spike_etw.py` は対応する調査書の引数・対象記録を参照
- GPU Timeline 解析は全 CPU CSV を読み込みメモリを使うため、複数の大きな記録は順に解析する

## 10. Git に含まれる資料とローカル証跡

### Push 対象の調査書

| 資料 | 読む目的 |
|---|---|
| [Work-Summary](M06-15-Work-Summary.md) | 元の改善・入力計測・累積調査の入口 |
| [Controlled-Focus-Comparison](M06-15-Controlled-Focus-Comparison.md) | 前面切替の制御・品質 Gate |
| [Dwm-Present-History](M06-15-Dwm-Present-History.md) / [Dwm-Cpu-Stacks](M06-15-Dwm-Cpu-Stacks.md) / [Normal-App-Focus](M06-15-Normal-App-Focus.md) | DWM と Intel Compiler の根拠と対照条件 |
| [Gpu-Priority-And-Signal](M06-15-Gpu-Priority-And-Signal.md) / [Signal-Wait-Confirmation](M06-15-Signal-Wait-Confirmation.md) | Device 排他の待機・解放 Stack |
| [Presentation-Stackwalk-Fault](M06-15-Presentation-Stackwalk-Fault.md) | 計測自身による I/O の一件 |
| [Goal2000-DoubleBuffering](M06-15-Goal2000-DoubleBuffering.md) | 2,000 FPS の条件と未実装の改善候補 |
| [DoubleBuffering-Baseline](M06-15-DoubleBuffering-Baseline.md) | ETW なし、Sink 有無の基準測定 |
| [Adapter-And-Presentation-Path](M06-15-Adapter-And-Presentation-Path.md) | RTX 描画 / Intel 表示の照合、前回の 0.877 ms |
| [Intel-Presentation-Comparison](M06-15-Intel-Presentation-Comparison.md) | 最新の Intel 比較・全試行の数値・欠落の扱い |

### Git 管理外の証跡

**`out/` の ETL / CSV / Binary / 個別解析コピーはローカルのみで、今回の Push には含めない。別会話が同じ Workspace を使う場合は直接読めるが、別 PC の Clone だけでは存在しない。** 詳細報告と再計測できる Source / 標準 Tool は Repository にある。

主な保存先（全て Workspace 相対）:

- `out/validation/Performance20261019/Focus144/`: 制御した前面比較
- `out/validation/Performance20261020/CpuStack144/Run1/`: DWM CPU Stack
- `out/validation/Performance20261021/Signal144/Companion1/` / `Notepad1/`: Device 排他の正式二条件
- `out/validation/Performance20261021/Presentation144/`: Stackwalk / Hard Fault の個別解析
- `out/validation/Performance20261022/Goal2000/`: 基準測定と再現用 Script
- `out/validation/Performance20261023/Adapter144/`: Adapter 診断 C#、表示構成、前回の正式 `NoStack2`。`NoStack1` は ETW 欠落で不採用
- `out/validation/Performance20261023/Intel144/`: 最新比較の全件 CSV、ETL、Log、Binary、再現用 `RunTrial.ps1` / `CaptureCompare.ps1`、個別解析 Script
- `out/validation/Performance20261023/Handoff144/`: 今回の提出確認・Python Test Log

最新比較の入口は `Intel144/TrialAnalysis.json` / `EtwTrialAnalysis.json` / `GpuComparison.json`。`IntelEtw2/AdapterPresentJoin.json` / `FrameDisplayJoin.jsonl` / `GraphQueueProof.json` と RTX 側の同名 File で個別 Frame まで追える。失敗試行・元の欠落 ETL も削除せず保持している。

## 11. 次の調査候補

以下は候補であり、次の作業依頼に応じて選ぶ。

1. **RTX の描画性能を保ち、表示も RTX にそろえる比較**。まず PC の正確な機種・MUX / Advanced Optimus・外部端子接続を読み取りで確認する。今回は対応未確認。比較では UI / Buffering / Waitable / Size / 前面をそろえる
2. **Intel の GPU 開始前約 0.625 ms と Fence 待機約 0.14 ms の内訳**。GPU Queue、既提出 Frame、OS / 表示作業と照合し、純粋な GPU 演算時間や Worker 切替時間に誤分類しない
3. **同一条件で残る通常時のスパイク**。Stack なしを基準にし、必要な Frame に限って詳細原因を調べる。失敗試行や前面切替を通常実行へ混ぜない
4. **FrameGraph / RHI の毎 Frame の CPU コスト**。Timestamp Frequency の保持、Completion の確保、Pool の Lock 範囲が候補。Completion 再利用は古い Token の値・寿命を変えず、Lock 変更は停止 / 取消 / 失敗回復まで検証する
5. Worker の通知先分割、ImGui GPU 待機の排他範囲、統計 Snapshot の複写量は次点。Main → Update の既存平均約 8.3 µsだけで不足分を説明できず、現在の二 Pass / 単一 Graphics Queue に Cross-Queue Scheduler 改造を優先する根拠もない

Waitable 許可の `m_hasFramePermit` は既に再利用を管理する。呼出箇所が二つあるだけで二重待機と断定しない。Resource 再利用の Fence を削除したり、待機を計測区間外へ移したりして改善扱いにしない。

次の会話は、まず Branch / Dirty 状態と本書・最新比較を読み、ユーザーの次の依頼を確認するところから再開できる。
