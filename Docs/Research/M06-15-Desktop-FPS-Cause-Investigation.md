# M06-15 / Issue #144 — 平均 FPS と周期的低下の原因調査

2026-10-25。エンジン本体・Third-party Source・既存 Build・Git の変更なし。既存の未 Commit 変更は保持した。調査用 Code、実行 File と記録は `out/validation/Performance20261025/FpsCause/` に隔離した。

## 現時点の結論

1. 周期的な 3 ms 超の Render 完了間隔では、Waitable ON 時は `DXGI.Ready`、OFF 時は `Fence.Wait` が支配的である。OFF にしても停止自体は消えず、待機先が移る。
2. 停止時刻は 74.973 Hz（約 13.338 ms）の位相に強く同期する。毎回 13.338 ms ごとに止まるという意味ではなく、その整数倍の時刻に集中する。良好だった過去の試行にも同じ同期は存在し、75 Hz 同期だけでは平均 FPS の低下を説明しない。
3. 通常対照の GPU Pass Envelope は約 0.06 ms で、CPU の提出後から最初の GPU 計測点までの待ちが増えていた。再開後の軽量記録では Envelope 自体にも一時的な増加があるが、約 3 ms の停止は前 Frame の GPU 開始遅延との対応が強い。GPU 区間の長さと開始前の待ちを区別する。
4. Main の Win32 更新では `WindowFromPoint` が大きい。自己 EXE 内の API 呼出を測ると、Win32 NewFrame の壁時間の約 77.5% を占めた。これは全 Frame / FPS 低下の 77.5% という意味ではない。
5. 保存した Old Binary 自体も 19 時台の同時点比較では約 1100 FPS に落ちていた。その後 23 時台には同じ Binary と現在版で約 2000 FPS を再現した。Profiler 整理固有の変更だけでは、この変動を説明できない。
6. 計測用翻訳単位の最適化漏れは見つかったが、`/O2` の対照試行では大幅改善せず、主原因とは判定できない。
7. 再開後の軽量記録で、長い Ready 待機は 2 Frame 前の Present 履歴を DWM が処理した時刻と約 44 us 差で対応した。同じ Frame 差の Queue Packet でも、待機解除→Packet Stop→Ready 復帰を照合できた。周期的停止の直接の待機経路は Presentation 側のキュー回収にあると絞れる。
8. Windows 再起動後の比較でも Old 1201.190 FPS、Current 1189.489 FPS（各 10 秒 × 3 回）で、2000 FPS に戻らなかった。周期 Ready 待機も継続。詳細は [再起動後比較](M06-15-Desktop-Post-Reboot-Comparison.md)。
9. その後の [原因切り分け比較](M06-15-Desktop-Causal-Tests-2026-10-25.md)では、NVIDIA Overlay の OFF 操作前から Old 2104 FPS へ復帰。OFF 1943、ON 復帰 2042、現在版 2014 FPS。Overlay / Game Filters は元の ON に戻した。回復後 Snapshot の有効画面数は 1 台で、以前の 2 台から変化していた。個別介入による因果の確定までは行っていない。
10. [現在版の時間内訳](M06-15-Desktop-Current-Timing-Breakdown-2026-10-25.md)は CPU Main 平均 0.145 ms、Update 0.0011 ms、Render 0.486 ms、GPU Pass Envelope 0.0591 ms。全標本の平均・p95・最大・件数と、重複しない Render 内訳を記録した。
11. ユーザーからデュアル→シングルへ変更した確認を得た。デュアルモニター時の表示経路・同期負荷が最有力候補。画面数、リフレッシュレートの組合せ、DWM / Driver の個別要因はまだ分離しておらず、同条件での反復切替による因果確認は未実施。

DWM の Present 履歴処理・キュー回収までが遅れる内部理由（DWM、Driver、他 GPU Work、OS Scheduling 等の区別）と、Windows の Hit Test が以前より高コストになった内部要因は未確定。Defender など特定 Process の因果は証明していない。

## 保存 Binary の直接比較

旧実行 File は 2000 FPS を記録した保存物と SHA256 が一致した。

- Old: `37C7DAC0B4B887E175008623528A8E060B8E4AA99A191E4B4F08BDE745CFA7F9`
- Current: `992144D34C321A862B75B1F907C607AD160B24D12F2013B9DC914F5EC3921266`

同じ DLL（dxcompiler / dxil / WinPixEventRuntime）と EngineResources を共通 Directory に配置。枠付き Client 1280 × 720、DISPLAY1、Development / RTX 3060、Buffering 2、Worker ON、Queued Presents 2、VSync OFF、Tearing ON、FPS 上限なし、詳細 Trace ON とした。各 Warmup 2 秒 + 測定 10 秒。

Native 調査 Launcher は Process の起動時情報で位置を指定し、計測中には Window 操作も入力も行わない。実 Client / Window Rect、Pointer、前面維持と Hash を検査した。

### 最初の比較

Window 外枠位置 (20, 200)、Pointer (508, 761)。

| 版 | FPS | 結果 |
|---|---:|---|
| Old | 1145.598 | 品質成立 |
| Current | 1122.294 | 品質成立 |
| Current 再試行 | 1095.346 | 途中の Pointer / 前面変化により不採用 |

1 回ずつの差は約 -2%。この差を統計的に確定した Profiler 回帰率とは扱わない。

### 対照試行

全版を同じ `Bundle/CueSpikeTimingProbe.exe` の Path / File 名へ順に配置し、名前による Profile 条件差も減らした。位置 (20, 250)、Pointer (630, 812)。全 8 試行で前面・寸法・Pointer / Window 不変、欠落・上書き 0、Snapshot 追加確保 0、Exit 0。

| 試行 | 内容 | FPS |
|---|---|---:|
| 1 | Current、Waitable ON | 1103.000 |
| 2 | Current、Waitable OFF | 1168.997 |
| 3 | Current、Waitable OFF | 1136.124 |
| 4 | Current、Waitable ON | 1117.683 |
| 5 | 同じ Current Engine lib、Probe TU だけ /O2 | 1114.546 |
| 6 | 同じ Current Engine lib、Probe TU だけ /Od | 1123.627 |
| 7 | /O2 + WindowFromPoint の自己 IAT 計測 | 1085.645 |
| 8 | Old、Waitable ON | 1097.074 |

ON の合算 1110.341 FPS、OFF の合算 1152.557 FPS（+3.802%）。OFF は停止を根本解消していない。最適化対照は各 1 回で、微小差の有意性は主張しない。Hook は API の内訳を知る計測で、性能改善比較には使わない。

## 周期的な低下

最初の旧 / 現行比較では、3 ms 超の間隔が旧 146 回 / 10 秒、現行 126 回 / 10 秒。各 145 / 125 件は Ready が最大区間だった。現行の同 Frame の平均間隔 3.440 ms のうち、Render 3.427 ms、Ready 2.589 ms、Present 0.372 ms、Graph Record 0.115 ms。Main 供給待ちや UI 構築がそのスパイク本体ではない。

対照試行でも同じ傾向だった。

| 条件 | 3 ms 超の間隔 | 最大区間の分類 | 同 Frame の主待機平均 |
|---|---:|---|---:|
| ON・1回目 | 401 | Ready 400 件 | 2.789 ms |
| ON・2回目 | 387 | Ready 386 件 | 2.832 ms |
| OFF・1回目 | 288 | Fence 287 件 | 2.877 ms |
| OFF・2回目 | 251 | Fence 249 件 | 2.770 ms |

上記の件数割合は、停止時間全体の割合ではない。OFF の位相集中度も 74.973 Hz で 0.922～0.937 を維持していた。

`DX12SwapChain::wait_for_frame()` は Frame Latency Event を待つ。OFF では同箇所が即時通過するが、後続の GPU Resource 再利用時に同じ GPU 完了側の遅れが Fence 待ちとして現れる結果と整合する。

## GPU の時刻照合

各 Queue の GetClockCalibration と Timestamp Frequency を用い、GPU Query を同じ Frame / Queue の CPU 提出と対応付けた。GPU の大きな生 Tick は整数差を取ってから換算し、CPU QPC とは別の単位として処理した。

| 試行 | 提出 API 復帰→最初の GPU 計測点 平均 | 同 p95 | GPU Pass 全体の平均 |
|---|---:|---:|---:|
| 過去・有効3回のうち Trial2（2184 FPS） | 0.0930 ms | 0.7007 ms | 0.0602 ms |
| 今回 Current ON | 0.2933 ms | 2.3438 ms | 0.0624 ms |
| 今回 Current OFF | 0.4502 ms | 2.9019 ms | 0.0621 ms |
| 今回 Old ON | 0.2779 ms | 2.2917 ms | 0.0602 ms |

過去行は 2073 FPS の 3 試行合算値ではなく、そのうち 1 試行の GPU 照合結果。

OFF 試行の 1 ms 以上かつ成功した Fence Event 711 件を、待っていた Fence の提出・GPU Query と照合した。平均待機 2.036 ms、対応する提出→GPU 最初の計測点 3.022 ms、GPU Pass 全体 0.0612 ms、GPU 最後の計測点→CPU 待機復帰 0.0766 ms。GPU の実行開始が遅く、Query 後の CPU 復帰だけが数 ms 遅いという結果ではない。ただし最後の Query は Fence Signal 自体の実行時刻ではない。

## ImGui / Win32 の処理

過去の Win32 NewFrame 全体は約 15～16 us、今回の Old / Current は約 88.6 us とほぼ同じだった。

WindowFromPoint 計測版は、自己 EXE の USER32 IAT Entry だけを RAII で差し替え、同じ POINT で元 API を呼んだ。HWND と LastError を維持し、Worker 停止・Trace 登録解除後に復元。Engine / Third-party は変更していない。外部 Process の Memory 操作は行っていない。

全 10,857 個の `Win32.WindowFromPoint` が同 Thread / 同 Frame の `ImGui.PlatformNewFrame` に完全包含された。

- PlatformNewFrame 平均: 84.478 us
- WindowFromPoint 平均: 65.452 us
- 合計壁時間の比: 77.48%

Main がこの更新を含む ImGui Context Lock を保持し、Render の ImGui 記録が同じ Lock を待つため、Main の Native 処理時間増加が Render 側にも波及する。旧 Trace の `ImGui.Lock` は Render 入口と Pass 記録の 2 回を含むため、現行の `ImGui.RecordWait` と呼出単位の平均だけを比較して倍増と判断しない。

## 除外できた候補と限界

- TimingSamples の集計アルゴリズム、FrameController の待機・Worker 制御は整理前後で同じ。新しい ProfileMetric の系列 Mutex と非 inline 呼出は存在するが、同条件の旧版も遅いため、35～47%の大幅低下の説明にはならない。
- 通常 UI は Profiler の全体 snapshot() を毎 Frame 呼んでいない。Registry 全走査・Snapshot 配列確保を原因とはしない。
- 現行の Engine 主要 lib は Development /O2 /MD。計測 Probe 自体には最適化指定がなかった。元の同じ Engine lib にリンクして TU だけ /O2 と /Od にした対照では大幅改善なし。
- 過去の Old / 同じ SHA でも、別の追加試行に約 1273 FPS の記録がある。2000 FPS は固定的に再現していた保証値ではない。
- 最初の比較と対照では位置・Pointer が異なるため、両 Run の絶対 FPS の差を単一要因の効果とは扱わない。
- 100 ms 間隔の CPU 累積値を RAM に保持して終了後に保存した。対照中の CPU 全体は約 59～67%だったが、保護された DWM / MsMpEng の Process CPU は読めなかった。計測外では MsMpEng の高負荷を観測したが、これを低下の原因と断定しない。
- WPR は非管理者で開始に失敗（0xc5585011）した後、ユーザーの承認と UAC 操作により管理者権限で取得した。取得結果と制約は次節に記す。

## 管理者権限による詳細記録

19:30:17 から WPR の GPU / DesktopComposition / CPU を記録した。アプリの測定区間は 5.002 秒、起動・終了情報を含む ETL 全体は 50.003 秒。保存と Metadata 整理は 19:36:49 に正常完了した。ETL は約 3.76 GB、Lost Event / Buffer は 0。WPR は停止済み。

この記録では平均 340.692 FPS だったが、通常試行の代わりには使わない。記録自体の負荷に加え、Pointer は DISPLAY2 上、アプリは DISPLAY1 上で、通常の対照試行と Hover 条件も異なった。

また、19:29:29 から始まった Steam の大型 Download / 展開と重なった。これは今回の詳細記録の交絡要因であり、18 時台の FHD 試行や 19:02 の対照試行の低下を遡って説明するものではない。他のアプリの停止やセキュリティ設定の変更は行っていない。

アプリ測定を含む Trace 相対 11.000～16.100 秒の CPU 解析では、全 16 論理 CPU に対する非 Idle は約 77.77% だった。

| Process | Sampled CPU 使用率（全論理 CPU 比） | Context Switch から算出した CPU 時間 |
|---|---:|---:|
| Steam | 15.34% | 12.484 秒 |
| System | 8.54% | 6.890 秒 |
| Defender | 6.77% | 5.530 秒 |
| CueEngine Probe | 6.32% | 5.217 秒 |
| Discord | 3.99% | 3.235 秒 |
| DWM | 3.49% | 2.834 秒 |

CPU 時間は複数 Core の合算。Probe の内訳は Render 3.676 秒、Main 1.259 秒、Update 0.143 秒。Kernel Module に属する Sample が多いが、Module 集計だけでは内部の待機原因は断定できない。

この高負荷記録では Render 平均 2.572 ms、Present 1.151 ms、Queue Submit 0.632 ms、Graph Record 0.554 ms。3 ms 超の間隔 430 件の最大区間は Present 306 件、Graph Record 89 件、Render 外 21 件、Ready 14 件だった。通常試行の Ready 支配とは分布が異なるため、Capture 中の Present 平均を通常時の主要原因として置き換えない。

ETL は独立した TDH Reader で RAW QPC のまま抽出した。GPU 関連 Event 84,499 件を JSONL 化し、Schema / Property / Callback Error は 0。Device / Context / HwQueue の所有関係と Queue Packet の時刻照合に使用する。

### 中断時点（19:56）

ユーザーの指示により調査を一旦中断した。Download 終了後の軽量 GPU 専用記録を準備したが、アプリの前面待機が 45 秒で Timeout（Exit 11）したため、有効な測定試行は成立していない。この ETL を FPS 比較には使わない。

軽量記録は 19:56:48 に保存・停止済み。19:56:57 に計測アプリと管理者 Script の終了、および `WPR is not recording` を確認した。再開時は前面で操作を止められる時間を確保してから新しい記録を開始する。既存の重負荷記録と通常対照試行は保存済み。

### 再開後の軽量記録（21:08～21:10）

ユーザーが操作を止める時間を確保したうえで再開した。最初の `CaptureLight-20261025-210821` はアプリ測定に成功したが、ETL の DxgKrnl Event は 0 件だった。Lost Event が 0 でも、必要な Event が収集できたことを意味しない。GPU 内部の解析には不採用とした。

Windows 標準の `GPU.Light` を `wpr -exportprofile` で出力して比較し、独自 Profile に `NonPagedMemory="true"` と `Strict="true"`、開始・保存時の Capture State を追加、Keyword を標準と同じ `0x277` に合わせた。CPU Sample と Stack は収集しない。[Microsoft の WPR 解説](https://devblogs.microsoft.com/performance-diagnostics/authoring-custom-profile-part3/)も Kernel Provider には NonPagedMemory が必要としている。これらをまとめて変更したため、単一の属性だけによる改善の対照試験ではない。

修正後の `CaptureLight-20261025-211022` は 21:10:34 に取得・保存・停止が正常完了した。ETL は 119,537,664 byte、全 Event 1,068,320 件を走査し、対象 DxgKrnl Event 148,941 件を抽出。Metadata 509 件、Schema / Property / Callback Error、Lost Event / Buffer はすべて 0。

ただし時系列の突合で、DxgKrnl の Present / Source0 VSync は測定開始から約 0.80 秒後以降しか残っていないことを確認した。WPR Memory Ring の上書き等が候補だが、発生機序は未確定。Lost Event 0 を全 5 秒の収録保証とは扱わない。CPU / GPU Query の CSV は全期間を保持するが、ETW との照合は両者が存在する共通区間に限定する。

| 項目 | 値 |
|---|---|
| Binary | Current（以前の対照と同じ保存物） |
| Renderer / Client | RTX 3060 / 枠付き 1280 × 720 |
| ImGui / Worker / Buffering | ON / ON / 2 |
| Waitable / Queued Presents | ON / 2 |
| VSync / Tearing / FPS 上限 | OFF / ON / 無制限 |
| 測定 | 5.000874 秒、5,084 Frame、1016.622 FPS |
| 位置 / Pointer | Window (434,273)、Pointer (1082,944)、Hover は計測アプリ |
| 品質 | 前面・寸法検査成立、Pointer / Window の移動 0、Trace 欠落・上書き 0、Snapshot 追加確保 0 |

以前の通常対照とは位置・時刻・記録の有無が異なるため、1016.622 FPS と約 1100 FPS の差を特定変更の回帰率とはしない。直前の Event 未収集試行は 1033.602 FPS だったが、各 1 回である。

軽量記録でも通常試行と同型の停止を再現した。3 ms 超の Render 完了間隔 226 件のうち 225 件は Ready が最大区間。74.973 Hz の位相集中度は 0.9625、60 Hz は 0.0246。これは約 75 Hz の周期への同期を示すが、毎表示周期で必ず停止する意味ではない。

| CPU Scope | 平均 | p95 |
|---|---:|---:|
| Frame.Render | 0.971 ms | 2.856 ms |
| DXGI.Ready | 0.166 ms | 1.917 ms |
| DXGI.Present | 0.301 ms | 0.512 ms |
| Graph.Record | 0.291 ms | 0.681 ms |
| ImGui.RecordWait | 0.199 ms | 0.564 ms |
| Queue.Submit | 0.116 ms | 0.201 ms |

Scope は入れ子であり単純合算しない。ImGui.RecordWait は Graph.Record 内に含まれる。

CPU 提出開始から最初の GPU Query までの遅れは平均 0.306 ms、p95 2.397 ms、最大 5.764 ms。Ready 主因スパイク 225 件の一つ前の Frame では平均 3.107 ms、p95 4.167 ms。その前 Frame の GPU Pass Envelope 平均は 0.175 ms で、3 ms の描画処理を実行していたという値ではない。

ただし今回の GPU Pass Envelope 全期間平均は 0.167 ms、p95 0.276 ms だった。約 1.5～4.0 秒の区間で約 0.255 ms に伸び、最後の 0.5 秒は約 0.055 ms に戻る。末尾 120 Sample の表示だけで全期間を代表させず、以前の通常試行の約 0.06 ms とも区別する。GPU Clock 変化・Preemption 等の区別は未確定。

RAM 保持した CPU 累積値では、測定内の約 4.90 秒における PC 全体の非 Idle は約 63.82%。Defender は約 1.51 論理 Core、DWM は約 0.20 論理 Core 相当だった。背景条件の記録であり、これだけで個別スパイクの発生元を断定しない。

### Present 履歴・Queue Packet と Ready 復帰の対応

ETW とアプリの共通窓は測定開始 +0.7987 秒以降の約 4.202 秒。Probe の Present 4,258 件はすべて CPU Present Scope 内にあり、Source0、FlipInterval 0、成功だった。Source0 の実 VSync 周期は平均 13.337970 ms（74.973926 Hz）。

共通窓の 1 ms 超 Ready は 286 件。終了位相は実 VSync 後の平均約 4.669 ms に集中し、位相集中度 0.9781。物理 VBlank と同時に解除される現象ではない。

そのうち Frame 対応が揃う 284 件では、すべて 2 Frame 前の Probe PresentHistory の Token / TokenData が DWM PID 1828 の履歴 Stop と対応し、その Stop が Ready 待機中にあった。履歴 Stop→Ready 終了は平均 43.83 us、中央値 42.8 us、p95 62.3 us、最大 129.8 us。1 Frame 前の履歴 Stop は逆に Ready 終了後で、この対応とは異なる。

独立した Queue Packet 照合でも、長い Ready 286 件のうち 276 件について同一の `hContext + pQueuePacket` を照合した。

- 対象 Packet は Start Event 244（Flags 4）→Unwait Event 238（0x00ee）→Stop Event 180。
- 全 276 件で `Ready Frame - Packet.FenceValue = 2`。
- 中央値は Ready 開始→Unwait 2347.1 us、Unwait→Packet Stop 21.2 us、Packet Stop→Ready 終了 5.6 us。
- この 2 Frame 前の ImGui GPU Query 終了は、全 276 件で Ready 開始より早い。Query 終了→Ready 開始は中央値 941.9 us、Query 終了→Ready 終了は中央値 3325.8 us。この ImGui Pass 自体の実行終了を待っていたという関係ではない。

以上は Queued Presents 2 の設定と整合し、DWM の履歴処理と Presentation Queue の解放を待っている経路を強く裏付ける。DWM の履歴 Stop は物理表示完了の保証ではなく、Event 244 / 450 / 451 の厳密な GPU 実行開始・完了の意味も未確認。後続 GPU Work、Driver、OS Scheduling、DWM のどれが遅延を作ったか、過去の 2073 FPS との差の全量を説明するかまでは断定しない。

なお従来の DmaPacket Start / Stop（175～177）だけでは Cue の Event が無いが、追加の DmaPacket Info（450 / 451）には Cue の HwQueue / ProgressFenceValue と一致する記録がある。従来 ID の不在を GPU 処理自体の不在とは扱わない。解析 Reader を拡張し、元の JSONL と拡張版を両方保存した。

## 証跡

`out/validation/Performance20261025/FpsCause/` に、保存 Binary、共通 Runtime、起動 Script と Launcher、全 CSV、版・位置・前面・Pointer の検査、入力 Hash、集計を保存した。

- `Runs-20261025-185158/`: 最初の旧 / 現行比較。不成立の3回目も保存し、正式解析から除外。
- `Controls-20261025-190234/`: 同名・同 Path の 8 対照試行と RAM 保存した CPU 累積値。
- `Analysis/`: 全区間・cycles・位相解析・全スパイク分類・GPU Queue 時刻照合。
- `OptimizedProbe/`: 同じ既存 Engine lib を使う /O2 /Od と API 内訳計測の独立 Build。
- `CaptureGpu.ps1`: GPU / DesktopComposition / CPU をメモリーモードで5秒記録する Script。既存 WPR 記録があれば触らず中止し、自分で開始したものだけを終了する。
- `Capture-20261025-193017/`: 管理者権限の ETL、Trace Header / Statistics、CPU 分析、測定 CSV、独立した TDH Reader と抽出 JSONL。
- `CaptureLight-20261025-211022/`: 再開後の GPU 専用 ETL とアプリ内全 5 秒の測定 CSV。
- `Analysis/CaptureLight211022-Correlation-Summary.md`: CPU / GPU / VSync / DWM 履歴の照合と共通窓の制約。
- `Analysis/CaptureLight-20261025-211022-GpuQueueFindings.md`: Queue Packet、Unwait、Frame 差 2、GPU Query 完了との照合。元の JSONL と追加 Event 450 / 451 を含む拡張 JSONL を保持。

Commit / Push / Merge、セキュリティ・OS・Driver設定の変更は行っていない。実験アプリは終了済み。

今回の再開作業では Engine Source / Test / Build 設定は変更していないため、Engine の Build / CTest は実行していない。独立した解析 Reader の Build と ETL 読取、CSV の時刻・Frame 照合を検証し、最後に WPR 停止とアプリ終了、`git diff --check` を確認した。
