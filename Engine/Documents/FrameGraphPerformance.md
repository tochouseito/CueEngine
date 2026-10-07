# FrameGraph の Queue 同期と性能計測

対象: M06 #89、#91、#92、#96、#97、#99、#100

## State と Queue の受渡し

- `ShaderRead` は Graphics では Pixel / Non-Pixel の両方を表す。Compute の宣言時には `NonPixelShaderRead` に正規化する
- `PixelShaderRead` / `NonPixelShaderRead` で使用段階を明示できる。Compute に Pixel 専用 State を宣言すると失敗する
- 遷移前 State を次の Queue が扱えない場合、Producer で Common に戻してから Consumer で必要 State にする
- Graphics 専用の開始 State を持つ外部 Resource は、Graph の `initial_barriers()` を Graphics に先行提出し、他 Queue はその Fence を待つ
- 同じ Read-only State の Graphics / Compute Read 同士は依存を作らない。UAV は書込み可能 State なので Read 宣言でも Queue 間の依存を維持する。最初に State の変更が必要なら、開始 Graphics 区間または直前の Producer で遷移する
- Copy Queue が Producer で Shader State を作れない場合は、最初の Reader に遷移を置き、残る Reader がその提出完了を待つ
- Write は先行する全 Reader / Writer に依存する。最後の Reader が Copy へ渡すため State を変更するときも、その前に他 Queue の Reader 完了を待つ。Copy Queue との受渡しには Common を使い、無条件の並列 Read を行わない
- Queue を跨いで使用する Transient Resource は途中の Alias 再利用を行わない。並列 Reader が残る Resource の復帰は最終 Graphics 合流後に記録する

`DX12MainFrameGraph` が開始・終了の Graphics 区間と各 Queue の提出を管理する。低水準の Executor を直接使う場合は、開始 Barrier と最終 Barrier の区間も呼出側が提出し、Queue 間の Fence 依存を接続する

## Command と記録準備

同じ Queue の連続 Pass は一つの Command List にまとめる。区間内部の依存は記録順で満たし、区間外の依存だけ Queue Fence で待つ。依存に使う値は `submit()` が返す Completion の `queue_identity()` / `fence_value()` であり、追加 Signal は発行しない

Build 後に描画枠ごとの Pass Callback を構築する。枠の初回記録時に Binding、Device、形状、用途を検証し Native Barrier 表を準備する。後続の区間はその表と Callback の部分範囲だけを使う。枠の再利用で Plan と外部 Binding を再検証し、容量は保持する

## CPU 時間

`IFrameGraphRecorder::performance()` は Backend 型を含まない所有 Snapshot を返す。DX12 実装は専用 Mutex で公開値だけを保護し、Graph 記録全体や GPU 待機中には保持しない

- `record`: 一つの Graph の各記録区間を合計した CPU 経過時間。初回の Native 表準備と Pass Callback を含み、Command の取得・Close・Submit は含まない
- `frameWait`: 同じ描画枠の前回 GPU 完了待機と Timestamp 結果の回収を含む経過時間。Resource Pool の acquire_wait と Command Pool の満杯待機は別区間であり、個別集計には含めない（Render 全体の計測には含まれる）
- `present`: Host が SwapChain の Present 時間を補う。Graph 単体では未計測
- 集計は直近 120 Sample の平均・nearest-rank p95・最大・直近値

## GPU 時間

描画枠が Queue ごとに独立した Timestamp Query Heap と Readback Buffer を所有する。Pass 本体の前後に Timestamp を記録し、同じ Queue で二点だけ Resolve する。最後の合流 Fence 完了後にだけ Map し、Queue 固有の Timestamp 周波数で時間へ変換する

- Graphics / Compute を計測する。Copy は `CopyQueueTimestampQueriesSupported` に対応する Device だけ計測する
- 未対応 Queue と未記録 Pass は `isAvailable == false` で返す。ゼロ時間と未計測を区別する
- `gpuPasses` は直近に回収した完了済み描画枠の値。現在 GPU が実行中の値ではない。各 Pass の `statistics` は全描画枠合算の直近 120 完了 Sample の平均・p95・最大を返す
- `completedGpuFrames` は計測を回収した枠数。Present 回数や GPU FPS を意味しない
- Pass の前後 Barrier、Queue Wait、Present は Pass 本体の時間へ含めない
- Query Heap は使用する Queue 種類ごとに生成し、Readback は描画枠の使用 Queue ごとに `Pass 数 × 16 byte` を確保する

計測そのものにも Query / Resolve / Readback の費用がある。負荷を比較するときは両条件で同じ計測設定を使い、Adapter、構成、VSync、FPS 上限、描画枠数、Sample 数を記録する

## 検証

- Builder: Compute の State 正規化、Graphics / Compute Read 並列、両 Reader に対する Write 依存、RenderTarget → Compute の Common 受渡し
- Executor: Common 開始 Buffer と Graphics 専用 ShaderRead 開始 Texture をそれぞれ Compute の Non-Pixel State へ渡し、終了 State へ戻す経路を WARP / Debug Layer で記録・提出する
- MainFrameGraph: 96 個の連続 Compute Pass を 2 Frame 実行し、Compute Queue の Fence 増加が 2 であること、全 Callback 実行、CPU Sample と GPU Timestamp 回収を確認する

Build と実行結果は中央の M06 検証記録に保存する。PIX Capture と実 GPU の性能比較は今回の Test だけでは検証できない
