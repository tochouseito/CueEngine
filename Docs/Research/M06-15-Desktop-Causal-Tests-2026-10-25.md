# M06-15 / Issue #144 — 性能低下の原因を切り分ける比較

2026-10-25。Engine の実装・設定、Git 状態の操作、Defender 保護設定・除外の変更は行わない。調査用の Script / 独立 Launcher は `out/validation/Performance20261025/FpsCause/` に隔離する。

## Defender の検査対象

`DefenderCause-20261025-222630/` で公式の `New-MpPerformanceRecording -Seconds 30` を使用。約 18 MB の ETL と `Get-MpPerformanceReport -Raw` を保存した。

記録された検査の上位は、Codex の会話ログ、PowerShell の Module / AMSI、他アプリのログだった。個々の会話の内容は読んでいない。

- Codex 会話 JSONL 3 File: 計 70 回、検査時間合計 658.9483 ms
- 上位 2 File は約 6～7 MB、各 32 / 30 回、合計 294.3430 / 292.1501 ms
- Process 別では codex.exe 667.8492 ms、pwsh.exe 641.7833 ms、PowerShell / AMSI 表記の系列 236.2267 ms
- 全 Process 14 系列: 1146 Scan、検査時間合計 1983.9902 ms

Raw の Duration は 100 ns Tick。非 Raw 表示で単位を照合した。これは Scan の所要時間であり、MsMpEng の CPU 実行時間ではない。記録前の CPU Counter 143% と、Report 解析後の 300% も瞬間値で、30 秒の平均ではない。背景負荷の一部を観測したが、これだけで高 CPU や FPS 低下全体を Codex に帰属させない。

根拠: `Analysis/DefenderCapture-20261025-222630-Interpretation.md`。

## 表示先モニターの比較

同じ Old Binary SHA256 `37C7DAC0B4B887E175008623528A8E060B8E4AA99A191E4B4F08BDE745CFA7F9`、枠付き 1280 × 720、ImGui ON、Worker ON、Buffering 2、Waitable ON、Queued Presents 2、VSync OFF、Tearing ON、FPS 上限なし。各回 Warmup 2 秒 + 測定 10 秒。順序は DISPLAY1 → DISPLAY2 → DISPLAY2 → DISPLAY1 → DISPLAY1 → DISPLAY2。

Window は DISPLAY1 (20,20) / DISPLAY2 (-1900,20)。Pointer は (1134,836) のまま両方の Window の外に固定し、同じ背景 HWND 上にある。Window / Pointer 不変、前面・寸法検査、Snapshot 追加確保・CPU / GPU 欠落・上書きの検査は全件成立。

| 表示先 | 試行 FPS | 合算 FPS | 3 ms 超 Frame 間隔 |
|---|---|---:|---:|
| DISPLAY1 | 1379.589 / 1462.885 / 1361.384 | 1401.286 | 4 件 / 30 秒 |
| DISPLAY2 | 1372.486 / 1433.252 / 1363.048 | 1389.596 | 171 件 / 30 秒 |

平均差は -0.834%。表示先だけを変えて 2000 FPS に戻る結果ではない。一方で停止の分布は異なり、DISPLAY2 は 169 件が Fence.Wait 最大区間。両表示先の Ready 平均は約 4 us だった。DISPLAY2 の停止位相も約 75 Hz に集中し、表示先の約 60 Hz にそのまま追従する結果ではない。

以前の (20,250)、Pointer が Window 内の試行とは、配置と Pointer の重なりが異なる。この二つを変えたことによる効果を、今回の表示先だけの効果とは混同しない。

証跡: `MonitorCause-20261025-223043/`、`Analysis/CausalPlan-20261025-MonitorResults-And-OldCLI.md`。

## 外部 CPU 採取の有無

同じ Old Binary、DISPLAY1 の Window (20,250)、Pointer (1134,836) は Window 内。各回 Warmup 2 秒 + 測定 20 秒。外部 CPU 累積値の 100 ms 採取だけを ON → OFF → OFF → ON とした。Window / Pointer の観察は共通で、どちらも同じ調査 Launcher を使う。

| CPU 採取 | 試行 FPS | 合算 FPS |
|---|---|---:|
| ON | 1114.372 / 1178.637 | 1146.504 |
| OFF | 1130.594 / 1109.164 | 1119.879 |

OFF による回復はない。CPU 累積値の採取だけを大幅な低下の原因とは判定しない。ただし Launcher 全体が存在しない試行との比較ではない。

両条件とも NVIDIA の pstate / Graphics Clock / Memory Clock 等を 500 ms 間隔で Process Pipe から RAM に受信し、対象終了後に保存した。GPU Telemetry の採取有無はこの比較で変えていない。前面・寸法・Pointer / Window 不変、Trace 欠落 / 上書き、Snapshot 確保の検査は全 4 試行成立。

証跡: `ObserverGpu-20261025-224224/`。

## GPU Clock と局所的な遅さ

上記 4 試行の Clock と GPU Query を時刻照合した。P8 はアプリ測定窓内では観測されず、主に P3 / P5 間で変動した。

- P3（Memory 5001 MHz）の安定 0.5 秒 Bin: GPU Envelope 約 0.052～0.059 ms
- P5（Memory 810 MHz）の安定 Bin: 約 0.227～0.248 ms
- Memory Clock と GPU Envelope の相関は試行ごとに -0.71～-0.86
- Clock と FPS の相関は -0.123～+0.195 と弱い。P0 / P3 の区間も 2000 FPS に戻る結果ではない

GPU 区間の一時的な約 0.25 ms への増加は低 Clock と強く対応する。全体の FPS 低下を単独で説明するという因果確認ではない。

証跡: `Analysis/CausalPlan-20261025-GpuPstate-Correlation.md`。

## 時間予算と外部入力

Old 高速 Trial2（2184 FPS、2073 FPS 合算の一試行）と今回の Old 4 試行合算（1133 FPS）では、Frame 間隔が約 424.65 us、Render が約 420.26 us 増えた。Render 内の平均増加は Graph.Record 約 164.83 us、Present 約 116.64 us、Ready 約 77.26 us、Queue.Submit 約 32.72 us で、合計は Render 増加の約 93%。

これらには CPU cycles 自体の増加と、cycles より wall time が大きく伸びる区間の両方がある。Graph 内の ImGui Lock と Main 側 Win32 更新を別途二重加算しない。cycles は命令数や実 CPU 時間へ機械的に換算しない。

旧 Probe は Performance.cfg と ImGui 設定の自動読込を明示的に無効化し、試行ごとに独立した storage を使っている。設定 File の自動上書きは確認できなかった。ただし当時の全 DLL / OS 環境の同一性までは証明できない。

根拠: `Analysis/CausalPlan-20261025-CpuCycle-Budget.md`、`Analysis/OldProbeExternalInputs-20261025.md`。

## NVIDIA オーバーレイの候補

19:30 の ETL の Cue Process 内に `nvspcap64.dll` があり、測定区間中もロードされていた。RTSS / Overwolf / DiscordHook / D3D12SDKLayers 名の Image は同 Process 内に見つからなかった。これは過去のロード証拠であり、原因の証明ではない。

現在の NVIDIA App の UI でも「NVIDIA オーバーレイ」と「ゲームのフィルタおよび写真モード」は ON。NVENC の Snapshot は Encoder 使用率 0%、Session 数 0 で、録画実行中とまでは判定しない。

ユーザーの明示許可を受け、ON → OFF → 元の ON の各状態で同じ Old Binary を毎回再起動した。変更したのは NVIDIA オーバーレイの Switch 一つで、Game Filters は親の OFF 中に自動で無効となり、ON 復帰時に元の ON へ戻ることを UI で確認した。比較後は Overlay / Game Filters とも ON。Defender 保護設定は変更していない。

| 状態 | 試行 FPS | 合算 FPS |
|---|---|---:|
| 元の ON | 2090.729 / 2117.696 | 2104.213 |
| 一時 OFF | 2026.000 / 1859.018 | 1942.507 |
| 元の ON 復帰 | 2121.076 / 1962.483 | 2041.781 |
| ON 復帰後の現在版 | 2121.470 / 1907.419 | 2014.434 |

各状態 10 秒 × 2 回。最初の ON 試行は前面化 Timeout で不成立となったため `On-ForegroundTimeout/` に隔離し、上表には含めない。取り直した 8 試行は全て前面・寸法・位置・Pointer・欠落・Hash の検査成立。

**OFF 操作前から既に約 2000 FPS に回復していた。** OFF による回復を示す結果ではない。Module 列挙は全件成功・切詰めなしだったが、`nvspcap64.dll` は OFF を含む全 8 試行に残る。機能 Switch の OFF と DLL の完全な非読込を同一視しない。

On / Restored / Current の Pointer は (1546,88)、Off は (1558,88) と 12 px 異なるが、全て Window 外の同じ背景 HWND。試行中の移動は 0。画面名は全て DISPLAY2。同じ 10 秒試行内の最大 / p95 を、状態間の平均 FPS と混同しない。

証跡: `OverlayCause-20261025-230630/`、`Analysis/OverlayCause-20261025-230630-Findings.md`。

## 回復時の表示環境と現在版の時間内訳

高速測定直後の 23:18 の Snapshot では、Windows の有効な Screen は 1 台（DISPLAY2 primary、1920 × 1080）。22:30 の表示先比較時には 2 台が有効だった。Defender の RealTimeProtectionEnabled は現在も true。

表示名の変化だけでなく、有効画面数の差を確認した。その後、ユーザーから「デュアルモニターではなくシングルにした」と、実際に画面構成を切り替えた確認を得た。Snapshot だけから推測した変更ではないため、デュアルモニター時の表示経路・同期負荷を最有力の原因候補とする。

ただし正確な切替時刻や各 Frame の構成履歴は保存しておらず、同条件の単画面→複数画面→単画面という反復比較ではない。Pointer の Window 内外、時刻・背景負荷も以前と異なる。画面数そのもの、異なるリフレッシュレートの組合せ、DWM / Driver の同期のどれが支配的かは未分離であり、歴史的な FPS 差全量を単一要因と断定しない。

現在版は同一 SHA のまま合算 2014.434 FPS。CPU Render は低速時 0.8283 ms → 今回 0.4860 ms、Ready 0.1230 → 0.00326 ms、Present 0.2771 → 0.1543 ms、Graph.Record 0.2450 → 0.1303 ms へ短縮した。GPU Pass Envelope は 0.06563 → 0.05908 ms で、CPU 時間の差全体を GPU 演算量だけで説明する結果ではない。

[現在版の全期間・時間内訳](M06-15-Desktop-Current-Timing-Breakdown-2026-10-25.md)に、Main / Update / Render、UI、Queue / Present / Fence、GPU 各 Pass の平均・p95・最大・件数を保存した。入れ子と並列の重複を区別している。
