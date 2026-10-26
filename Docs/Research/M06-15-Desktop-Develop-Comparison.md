# Desktop: develop と Issue #144 の同条件比較

2026-10-24。操作停止後の正式比較は **UI なしで +7.115%、共通統計 UI ありで -4.107%**。各条件 3 Pair、合計 12 試行をすべて同じ前面・Window・Pointer 条件で完了した。以前の 20 秒 1 Pair の暫定値は今回の集計に混ぜない。

## 比較対象と共通条件

- develop: `752d7fd9d7a12c4768acaf7620b84516e5244cca`。作業時に origin/develop と一致を確認
- Issue #144: `ba1c485d895683cc50e7db390b3ac27a8f2a01a7`
- 同一 PC: Ryzen 7 3700X / RTX 3060 / 32 GB。Development x64、同一の共通 Harness を各 Branch の実装へ Link
- CPU Buffering 2、Worker ON、Update Lead 1、GPU unfinished 上限 2
- SwapChain Buffer 2、Queued Present 2、Waitable ON、VSync OFF、Tearing ON、FPS 上限なし
- Client 1280×720、DISPLAY1、Window (570, 273)、Pointer (1218, 803)
- 各試行 Warmup 2 秒、計測 15 秒。操作停止時間内に収めるため両側とも同じ 15 秒へ変更。結果は RAM に保持し、Shutdown 後に CSV と標準出力を保存
- 各 UI 条件で develop→Issue、Issue→develop、develop→Issue の順。各 Branch の計測時間は合計約 45 秒
- 両方とも詳細 Trace は Compile 時に OFF。File Log と Debug Output は無効
- UI なしでは ImGui を生成せず、共通の ClearFinalColor + BackBufferClear を実行
- UI ありは両方とも Test Window に同じ Graph / Present / GPU 統計 Panel を表示。Branch ごとの既定 UI は表示内容が異なるため、そのまま比較しない

## 有効な計測結果

| 条件 / 指標 | develop | Issue #144 | 差 |
| --- | ---: | ---: | ---: |
| UI なし: 合計時間 | 45.0034100 s | 45.0001151 s | — |
| UI なし: 完了 Render Frame | 96,964 | 103,855 | — |
| UI なし: 全期間平均 FPS | 2,154.592 | 2,307.883 | +7.115% |
| UI なし: 平均 Frame 間隔相当 | 0.464125 ms | 0.433298 ms | -6.642% / -0.030827 ms |
| 共通 UI あり: 合計時間 | 45.0017268 s | 45.0002151 s | — |
| 共通 UI あり: 完了 Render Frame | 92,384 | 88,587 | — |
| 共通 UI あり: 全期間平均 FPS | 2,052.899 | 1,968.591 | -4.107% |
| 共通 UI あり: 平均 Frame 間隔相当 | 0.487116 ms | 0.507978 ms | +4.283% / +0.020862 ms |

FPS は `3 試行の完了 Render Frame 数の和 / 計測時間の和`。Frame 間隔相当はその逆数であり、CPU Render Callback の時間そのものではない。

12 試行すべて終了 Code 0、前面・可視・Client Size 不一致 0、Pointer 移動 0、Window 移動 0。試行間でも Monitor、Window 位置、Pointer 位置が一致。Binary Hash は各実行の前後および同じ Branch の全試行で一致した。各 CSV の累積時刻 / Frame 数は単調、100 ms 標本は各 150 個。

| 条件 / Pair | develop FPS | Issue #144 FPS |
| --- | ---: | ---: |
| UI なし 1 | 2,178.400 | 2,309.657 |
| UI なし 2 | 2,195.815 | 2,290.861 |
| UI なし 3 | 2,089.555 | 2,323.132 |
| 共通 UI あり 1 | 2,022.189 | 1,859.066 |
| 共通 UI あり 2 | 2,074.799 | 2,019.506 |
| 共通 UI あり 3 | 2,061.712 | 2,027.199 |

UI なしは全 Pair で Issue #144 が上回り、共通 UI ありは全 Pair で下回った。ただし 3 回の短時間試行であり、全ゲーム負荷や長時間動作への一般化、統計的有意性の主張はしない。UI あり 1 回目の Issue 値が他の 2 回より低い点も残す。

### 補助値: 終了直前 120 Sample

各試行終了直前 120 Sample の平均を 3 試行で平均した補助値。45 秒全体の区間平均ではない。

| 条件 / CPU 区間 | develop | Issue #144 |
| --- | ---: | ---: |
| UI なし: Render | 0.453806 ms | 0.405282 ms |
| UI なし: Graph Record | 0.020402 ms | 0.020266 ms |
| UI なし: GPU Queue Limit Wait | 0.034926 ms | 0.004598 ms |
| UI なし: Presentation Ready Wait | 0.123083 ms | 0.108133 ms |
| UI なし: Present | 0.173345 ms | 0.167446 ms |
| 共通 UI あり: Main | 0.152204 ms | 0.098208 ms |
| 共通 UI あり: Render | 0.482075 ms | 0.532487 ms |
| 共通 UI あり: Graph Record | 0.040253 ms | 0.092395 ms |
| 共通 UI あり: GPU Queue Limit Wait | 0.030958 ms | 0.014570 ms |
| 共通 UI あり: Presentation Ready Wait | 0.107397 ms | 0.131777 ms |
| 共通 UI あり: Present | 0.169200 ms | 0.183420 ms |

GPU ImGui Pass の末尾平均は 0.050372→0.048566 ms。GPU の ImGui 処理が増大した証拠はない。

これらは末尾 120 Sample だけの値であり、45 秒全体の平均ではない。特に Present の差を Present API 自体の最適化の証明に使わない。100 ms ごとの累積 Frame 数 CSV から、全 Frame の p95、最大、Hitch 頻度は算出できない。

## 改善に関係する実装差

### 1. GPU 完了待機

develop の Worker Render は GPU Frame 上限で詰まると `sleep_for(1ms)` 後に再確認する。Issue #144 は最古の Frame の Fence 完了通知を最大 1 ms 待ち、GPU が早く完了すればその時点で復帰できる。

これは全 Frame から必ず 1 ms を削る変更ではない。GPU Frame 上限に到達した場合に、既に完了した GPU の再確認まで寝過ごす時間を減らす。末尾の Queue Limit Wait の減少はこの方向と整合するが、単独の寄与率は未測定。

### 2. 統計処理の整列・コピー削減

develop の `TimingSamples::statistics()` は 120 Sample をコピーし、平均を集計して整列する。Issue #144 は `add()` 時に整列済み配列と整数和を維持し、取得時は小さな集計結果を返す。`FrameController::timing_info()` も 9 個の Sample 配列のコピーから小さな統計値のコピーへ変わった。

同じ入力列で「1 回追加 + 1 回統計取得」を 100 万 Pair、初回 Warmup と本計測 3 回ずつ実施した。

| 指標 | develop | Issue #144 |
| --- | ---: | ---: |
| 1 Pair の平均時間 | 3,006.244 ns | 98.354 ns |
| 統計 API の処理時間比 | 1 | 約 1/30.57 |
| 各実行の結果 Checksum | 2,437,638,396,119 | 2,437,638,396,119 |

結果は全実行で一致した。ただし `add()` は最大 120 要素の移動が増えており、全処理が定数時間になったわけではない。UI ありは毎 Frame 複数の統計を読む。Main 短縮という補助値とは整合するが、共通 UI ありの全体 FPS は今回低下している。約 30 倍という数値をアプリ全体の FPS 倍率へ置き換えない。

### 共通 UI ありの低下について

Main の末尾平均は約 54 µs 短くなった一方で、Render の Graph Record 区間は約 52 µs 長くなった。CPU 区間の改善が Render 側の待機や競合で相殺されている可能性がある。

ただし Main と Render は並列で、値も末尾窓であるため、この差を単純加算して全体 FPS の原因と決めない。今回の共通 Probe は ImGui Context Lock 待ちを単独出力しておらず、Graph Record 増大の内訳は特定できていない。

次の切り分け候補は、ImGui Context の待機 / 保持時間、UI 構築と Render の開始時刻の重なり、追加の入力計測 / 統計コピーの寄与。詳細 Trace OFF の比較条件を保ち、全期間の各区間を RAM に集計し、終了後に出力する測定が必要。原因確認前に ImGui や同期処理を変更しない。

### この比較の改善理由に含めないもの

- ImGui を外す効果: 各 UI 条件の両側で ImGui の有無を一致させている
- SwapChain Waitable の通知待ち導入: develop にも既に通知待ちがある
- ImGui Context Lock の大幅な短縮: 比較対象間で基本的な Lock 範囲は同じ
- Shader、DrawCall、Submit 数の削減: 今回の実装差にはない
- 詳細 Trace の無効化: 両方 OFF で比較
- Issue #144 内で発生・修正した回帰の改善量: develop には元々その回帰がない

Issue #144 には入力遅延計測などの追加処理もある。この比較はそれらを含んだ Branch 全体の差であり、個々の最適化を一つずつ無効化した因果分離実験ではない。

## 成果物と追加調査

共通 Root: `out/validation/Performance20261024/DevelopComparison/`

- `Harness/CommonProbe.cpp` / `Harness/CMakeLists.txt`: 共通計測 Source
- `BuildMetadata.json`: Ref、Binary / Source Hash、共通設定
- `Statistics-develop.log` / `Statistics-issue144.log`: 統計単独テスト
- `Runs-20261024-205827/`: 今回の正式 12 試行の `.log` / `.csv` / `-run.json`
- `Runs-20261024-205827/Summary.json`: 全試行、加重平均、比較率、座標検証を含む集計
- `Runs-20261024-201405/Ui0-Pair1-{develop,issue144}.{log,csv}` と `-run.json`: 過去の 20 秒暫定 Pair。今回の集計から除外
- `RunComparison.ps1 -MeasurementSeconds 15`: UI なし / 共通 UI あり、交互順、各 3 Pair の Runner

前面化失敗や Pointer 移動を検出した他の試行は除外した。操作停止後でも最初の `Runs-20261024-205735` は前面不一致を検出して止まったため含めない。

今回の反復比較は完了。共通 UI の低下理由の因果分離、各 Branch の既定 Editor UI そのものの性能、全 Frame の p95 / 最大 / Hitch、表示遅延の比較は実施していない。UI Snapshot の Warmup 後追加確保数も今回は共通 Probe で記録していない。

## 作業状態

両 Ref の Development 共通 Harness は Build 成功。Engine の実装は今回変更していない。標準 Debug Gate は新規実装変更がないため再実行していない。

同じ CueEngine Folder 内で一時的に develop を Build した後、元の `m06/issue144/gpu-waits-and-telemetry` へ復帰済み。既存の未 Commit `SpikeTimingProbe.cpp` は完全コピーから復元し、SHA-256 `CE4C5655852D4B43315E60497BA0114511EFDB9CF1200C6B0B17757E9F455B95` と一致を確認した。Worktree、Commit、Push、Merge は作成・実行していない。
