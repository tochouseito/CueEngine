# M06-15 / Issue #144 — Double Buffering の 2,000 FPS 目標に対する基準測定

> 続報: [描画 Adapter と表示経路](M06-15-Adapter-And-Presentation-Path.md)で、NVIDIA の描画 Command と Intel の表示 Context を確認した。後段の経過時間と、純粋な転送時間を区別して報告している。

- 測定日: 2026-10-22
- Branch: `m06/issue144/gpu-waits-and-telemetry`、HEAD: `6effec9`
- 前提と候補: [2,000 FPS の条件と改善候補](M06-15-Goal2000-DoubleBuffering.md)
- 既存の実行 File で測定。Engine Code / Build 設定の変更、Commit / Push / Merge は行っていない。

## 1. 結論

**Double・Waitable ON・通常 UI・1280 × 720 で、詳細記録なしの正式三回を合算した FPS は 1,199.87。平均 2,000 FPS の目標は未達。**

同じ設定・実行 File の実行間変動が大きい。詳細記録なし三回は 1,027～1,367 FPS、CSV 記録あり三回は 874～1,462 FPS だった。CSV ありの方が高い隣接試行もあるため、平均値の小さい差を Trace 負荷の確定値として扱わない。

CSV 三回で **表示待機以外の Render は平均 0.278～0.289 ms と近い一方、DXGI Event 待機は 0.400～0.849 ms と変動した**。今回も優先対象は表示受付の経路であり、CPU の統計計算や軽量な Update だけで目標までの差を埋める根拠はない。

## 2. 条件と計測の意味

| 項目 | 条件 |
|---|---|
| 実行 File | Development の `CueSpikeTimingProbe.exe`、全回同一 Hash |
| Buffering | CPU / GPU 資源枠 2、Worker ON、Update 先行上限 1 |
| GPU 未完了上限 / SwapChain 枚数 | 2 / 2 |
| 表示 | Waitable ON、Queued presents 2、VSync OFF、Tearing ON、上限 FPS なし |
| UI / Size | 通常 UI、Client 1280 × 720 |
| 期間 | Warmup 2 秒、その後各 20 秒 |
| 前面 | 取得を待って開始。全 Step 境界で前面・可視・非最小化・Client Size を検査 |
| 統計品質 | 通常の直近 120 標本の平均 / p95 / Max / 件数と GPU Query を維持 |
| ETW | Collector、Stackwalk、CPU Sampling は全回なし |
| NoSink | 詳細 CPU / GPU Trace の記録先を外す。通常の UI / GPU 統計は停止しない |
| TraceCsv | 詳細 CPU Span と GPU の生記録・Clock Calibration を有効化。終了後に CSV を出力 |

今回の NoSink は **詳細 Trace の Compile 定義も無効な通常 Build** とは異なる。同じ Trace 対応 Binary の Sink を外した対照実行であり、Scope の Sink 確認などは残る。CMake Cache は OFF だが、既存 Development Binary は過去に ON で Build されたもの。この区別を維持し、NoSink の値を Compile OFF の実測値と呼ばない。

Probe は Compile OFF では起動を拒否するため、厳密な Compile OFF 比較は今回の既存 Tool では行っていない。機能の実装依頼前なので、そのための C++ / CMake 変更もしていない。

GPU 環境の読み取り: NVIDIA GeForce RTX 3060 Laptop GPU / Driver `32.0.16.1714`、Intel UHD Graphics / `30.0.101.2079`。これは存在する GPU の一覧であり、Window の表示先 Adapter LUID との対応はまだ取得していない。電源・GPU Clock・他 Process の負荷も固定していない。測定中には Build や解析を並行しなかった。

## 3. 実行結果

順序は表のとおり。Formal の完了 Code は全て 0、stderr は空。最初の NoSink1 は起動確認用で、元の起動 Shell が終了 Code を保存していなかったため、正式三回の合算から外す。

| 順序 | 名前 | FPS | 計測中完了 Frame | Frame 間隔の平均 ms（完了数 / 実時間から） |
|---:|---|---:|---:|---:|
| Pilot | NoSink1 | 1,490.123 | 29,803 | 0.6711 |
| 1 | NoSink2 | 1,366.648 | 27,333 | 0.7317 |
| 2 | TraceCsv1 | 1,212.427 | 24,249 | 0.8248 |
| 3 | NoSink3 | 1,205.878 | 24,118 | 0.8293 |
| 4 | TraceCsv2 | 1,461.692 | 29,234 | 0.6841 |
| 5 | NoSink4 | 1,027.085 | 20,542 | 0.9736 |
| 6 | TraceCsv3 | 873.735 | 17,475 | 1.1445 |

正式三回をそれぞれ実時間・Frame 数で合算:

- NoSink: 71,993 Frame / 60.000688 秒 = **1,199.87 FPS**、平均間隔 **0.8334 ms**。
- TraceCsv: 70,958 Frame / 60.000848 秒 = **1,182.62 FPS**、平均間隔 **0.8456 ms**。

NoSink の合算値から目標の 0.5 ms までは約 0.333 ms、約 40% の平均 Frame 間隔削減が必要な参考値。FPS では約 67% の増加に相当する。ただし当日の変動が大きく、この一組の記録から将来の改善可能幅を保証しない。当初の約 650 FPS と条件を揃えた Before / After ではないため、そこからの改善率にも使わない。

## 4. 全期間の分布と内訳

以下は TraceCsv の全完了 CPU 区間から算出。UI の直近 120 標本とは別。Frame 間隔は連続する Render 完了時刻の差で、異なる Frame の Max を足していない。

| 記録 | Frame 間隔 平均 / p95 / Max ms | Render 平均 ms | DXGI Event 合計平均 ms | Fence Event 合計平均 ms | 残りの Render 平均 ms |
|---|---|---:|---:|---:|---:|
| TraceCsv1 | 0.8248 / 1.0696 / 5.0670 | 0.8184 | 0.5329 | 0 | 0.2855 |
| TraceCsv2 | 0.6841 / 0.8888 / 3.2485 | 0.6779 | 0.3996 | 0 | 0.2782 |
| TraceCsv3 | 1.1446 / 4.4331 / 5.3232 | 1.1385 | 0.8495 | 0 | 0.2890 |

「残り」は Render 時間から、その Render Thread / Frame 内に完全に含まれる DXGI / Fence Event の和を引いたもの。CPU 演算だけではなく、他の排他や再実行待ちも含む。対象 Event が互いに重ならないことを全件検証した。

Present API の平均は順に 0.1399 / 0.1365 / 0.1200 ms。最も FPS が低かった回で、Present API も最長だったわけではない。DXGI Event 待ちと区別する。

### GPU と長い Frame

| 記録 | GPU Pass 全体区間の平均 / Max ms | CPU 最大 Render の Frame | その Render / DXGI Event 合計 ms |
|---|---|---:|---|
| TraceCsv1 | 0.03045 / 0.05837 | 6027 | 5.0611 / 4.1363 |
| TraceCsv2 | 0.03202 / 0.05901 | 10069 | 3.2426 / 2.7166 |
| TraceCsv3 | 0.04362 / 0.07578 | 9484 | 5.3141 / 4.2408 |

GPU 列は各 Frame の最初から最後の Pass Timestamp の区間。Present の転送・表示側処理まで含むものではなく、GPU の純粋な Shader 実行時間とも同一ではない。

Render 内の Fence Event 待ちは三回とも 0 だった。前回までの別条件で発生した Fence 待ちが消滅したという意味ではない。

ETW / Stackwalk なしでも最大約 5.3 ms は再現したので、全スパイクを前回の Stack 展開中 Hard Fault だけで説明できない。一方、過去の約 30～50 ms は今回の測定では再現していない。

Native DXGI Wait の 2 ms 超過は、通知成功が計 3,091 回、Timeout が計 390 回。1 ms の Timeout 指定だけが長い待機の理由ではない。今回 Kernel ETW はないため、この内側を Waiting / Ready、DWM、Driver のどれかへ断定して割り振らない。

## 5. 次の比較対象

優先順は前回の改善案を維持するが、今回は平均の変動も表示受付側へ絞る根拠が増えた。

1. 描画 Adapter LUID と Window の表示先 Adapter / Output を取得し、同一 GPU での表示と Adapter 間の経路を比較する。単に NVIDIA / Intel の Module があることだけでコピー回数を確定しない。
2. 必要なら同じ 1280 × 720・前面維持で、Stackwalk / Sampling なしの ETW と Present 履歴を取得する。表示受付を返す条件と GPU / 表示側の処理順序を調べる。
3. FrameGraph / RHI の毎 Frame 処理を減らす候補は残すが、UI の統計を間引く、GPU Fence を外す、Single に変えることを今回の目標達成策にしない。

今回の無操作測定から入力遅延の改善は主張しない。入力イベントの到達 → 反映 Frame の Present / CPU 完了の計測機能は維持している。

## 6. 検証と成果物

`out/validation/Performance20261022/Goal2000/`:

- `Baseline/*-launch.json` / `*-done.json`: 引数、PID、実行 File Hash、正式六回の終了 Code。
- 各 `.log` / `.err` / `*-storage/logs/`: FPS、通常 GPU 直近標本、前面・Size 検査、Host の停止。
- `Baseline/BaselineAnalysis.json` / `BaselineAnalysis.log`: 全期間の区間分布、Frame 間隔、同一 Frame の待機内訳、上位 Frame。
- `Baseline/TraceCsv*-spans.csv` / `*-gpu.csv`: 生区間。
- `Baseline/TraceCsv*-gpu-analysis.json` / `*-gpu-frames.jsonl`: GPU Clock 対応と全 Frame の GPU 計測。
- `Baseline/GpuAndWaitSummary.json`: GPU 対応品質と Wait の戻り値別件数。
- `Baseline/GpuInventory.json` / `Baseline/SourceHashes.json` / `Baseline/ExecutableHash.json`: 読み取った環境と File の同一性。
- `RunTrial.ps1` / `AnalyzeBaseline.py` / `VerifyGpuAndWaits.py`: 今回の実行・集計・検証。

検証結果:

- 全七回の前面・Size 不一致 0、状態変更 0、Snapshot 追加確保 0。可視 Flag は完全な遮蔽検査ではない。
- 正式六回の実行 File Hash 一致、終了 Code 0、stderr 空。
- CSV 三回の CPU / GPU 欠落・GPU 上書き 0。CPU 連番の全件連続性と Log の保持件数一致を確認。
- GPU 提出数と対応済み Frame 数は三回とも一致。不正 GPU 記録 0。提出未記録の Warmup 二 Frame ずつは境界として区別した。
- GPU 解析 Test 三件成功。初回は Sandbox の既定 Temp 書込み制限で一件実行できず、Repository 内の専用 Temp で再実行して成功した。
- Engine Code は今回変更していないため、Build は再実施していない。既存の未 Commit 変更を保全した。

実行 File SHA-256: `821BDC0B547FF1ECAD730746692B34B79F54B8AD8BE6D5B0D4874FF96C132AA8`。

計測アプリは全て終了済み。生記録と個別 Script は Git 管理外で、Push / Clone には含まれない。
