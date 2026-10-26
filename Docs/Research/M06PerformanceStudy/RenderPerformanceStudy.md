# M06 #101 Render 性能調査

追加の改善案比較は [改善案の比較実験](ImprovementExperiments.md) に記録した
採用候補の判断は追加実験の結果も参照する。異なる Phase の Sample は混ぜない

## 作業境界

- Issue: https://github.com/tochouseito/CueEngine/issues/101
- 調査 Branch: `m06/issue101/render-performance-study`
- 比較基準: develop `02df9d17509fde4a21406a47efc42016ed7fbe9d`
- 既存の FrameGraph Development `/O2` / `/Zi` は基準 Commit へ統合済み
- この Branch の計測 Hook、Probe、比較条件は develop へ Merge しない
- 改善案を本実装する際は別 Issue を作り、その時点の develop から別 Branch を作成する

## 測定条件

2026-10-08、Windows 11 Home 10.0.26200、Ryzen 7 3700X、RTX 3060、Driver 32.0.16.1047
実際に採用した Adapter の Vendor / Device ID は 4318 / 9475

- MSVC 19.51.36260.0、Development、Engine Module は `/O2`、Debug Layer / GBV は Code から有効化しない
- 通常表示の 1280 × 720 Window、ImGui Test Window は 640 × 480
- Main / Update / Render、Worker 有効、Frame 2 枠、SwapChain 2 枠、60 FPS、VSync 無効
- Tearing は基準で無効。Tearing 条件では実際に有効であることも確認
- 既存 Layout File は読み込まず、保存もしない
- 各 Process で Warmup 120 Frame、その後 600 Frame を測定。各比較を 3 回、基準は各回の前後で計 6 回
- 24 Process、測定 Render 14,400 Sample。すべて正常停止し、固定領域の記録欠落は 0
- Window の遮蔽、GPU Clock / Power State、他の Background Process は固定していない
- 計測 Hook の時計取得・記録費用が含まれる。Hook 無効との独立比較は未実施

## 計測方法と区間の関係

`CueEditorTimingProbe` が製品と同じ `EditorHost.initialize / step / shutdown` を呼ぶ
FrameController の Render Callback 時間を絶対 Frame ID で保存する。FPS 上限待機は別 Sample とする
各 Thread は固定領域へ排他的な Index を予約して書き、全 Worker の Join 後だけ CSV へ保存する
測定 Loop では進行数だけを参照し、平均 / p95 の取得や File 書込みを追加しない

以下の区間は入れ子であるため合算しない

- `GraphRecord` は `ImGuiRecordWait` と `ImGuiRecord` を含む
- `GraphPrepare` は `GpuReadback` と描画枠の GPU 完了待機を含む
- `Main` は `Readout`、`UiBuild`、`SnapshotCopy` を含む
- CPU の `ImGuiRecord` は GPU の ImGui Pass 実行時間とは別である
- 既存の ImGui Context Wait 集計は Main / Render の両方を含む。今回の Hook は Render の開始、記録、再取得を分ける

## 基準の CPU 内訳

単位 ms。6 回、計 3,600 Render Sample の pooled 集計。p95 は nearest-rank

| 区間 | 平均 | p95 | 最大 |
| --- | ---: | ---: | ---: |
| Render 全体 | 0.6651 | 0.8840 | 1.7734 |
| Present | 0.2494 | 0.3430 | 1.1428 |
| GraphRecord | 0.1864 | 0.3115 | 0.9248 |
| CommandSubmit | 0.1040 | 0.1409 | 0.3369 |
| CommandAcquire | 0.0493 | 0.0636 | 0.3192 |
| GraphPrepare | 0.0244 | 0.0309 | 0.2442 |
| CommandClose | 0.0174 | 0.0245 | 0.1679 |
| TimestampFrequency | 0.0136 | 0.0179 | 0.3722 |
| GraphMark | 0.0022 | 0.0030 | 0.2584 |
| ImGui 開始 Lock 待機 | 0.0008 | 0.0011 | 0.1907 |
| ImGui 記録 Lock 待機（GraphRecord 内） | 0.0882 | 0.1914 | 0.8076 |
| ImGui 記録本体（GraphRecord 内） | 0.0366 | 0.0586 | 0.2084 |
| ImGui 再取得 Lock 待機 | 0.0011 | 0.0001 | 0.4011 |
| GPU Timestamp 回収・集計（GraphPrepare 内） | 0.0130 | 0.0161 | 0.1554 |
| Main | 0.2242 | 0.3399 | 0.9498 |
| Main の統計取得 | 0.0481 | 0.0585 | 0.3101 |
| UI 構築 | 0.1591 | 0.2648 | 0.8846 |

開始 / 再取得の待機は平均では小さい。記録前の Context 待機が GraphRecord 平均の約 47% を占める
CommandAcquire / Submit は CPU の Lock、管理と Native API 呼出しを含む。GPU 実行時間ではない

## 比較結果

各比較 1,800 Render Sample。基準は 3,600 Sample
`readout15` は統計 Sample の収集を維持し、Main の集計取得だけ 15 Frame に一度にする
表示値はその間 Cache を使う。UI の文字列構築と描画は毎 Frame 継続する

| 条件 | Render 平均 | p95 | 最大 | 比較対象 |
| --- | ---: | ---: | ---: | --- |
| 既定の詳細 UI | 0.6651 | 0.8840 | 1.7734 | 基準 |
| 詳細 UI / 統計取得 15 Frame 間隔 | 0.6325 | 0.8933 | 2.7777 | 既定 UI |
| Test / TEST のみ | 0.6116 | 0.8448 | 1.4014 | 既定 UI。表示内容の削減 |
| Test / TEST のみ / 統計取得 15 Frame 間隔 | 0.6080 | 0.8942 | 9.3621 | Test / TEST のみ |
| Test / TEST のみ / GPU Timestamp 無効 | 0.6647 | 0.9693 | 2.0312 | Test / TEST のみ |
| Test / TEST のみ / PIX Marker 無効 | 0.6803 | 0.9345 | 9.9201 | Test / TEST のみ |
| 詳細 UI / Tearing 有効 | 0.6737 | 0.8912 | 1.6286 | 既定 UI |

### 解釈

- 詳細 UI の集計間引きは pooled 平均で 0.0326ms、約 4.9% 小さい。ただし p95 は改善せず、反復間でも一貫した改善ではない
- TEST のみの表示は平均で約 8.0% 小さいが、描画内容を減らした比較である。Engine 全般の高速化とは扱わない
- 詳細 UI の最終描画量は 2,328～2,432 Vertex、TEST のみはすべて 60 Vertex。動的な数値表示の形状も変動する
- TEST のみでの集計間引きは平均差が 0.0036ms と小さく、最大値が悪化した。どの UI にも同じ効果があるとは言えない
- GPU Timestamp / PIX を無効化しても、この条件では一貫した高速化を確認できない。悪化の原因をこれらの無効化に断定することもできない
- Tearing を有効にするだけでは Render / Present の確実な短縮は確認できない
- 最大値の 9ms 台を特定の Code の重さと断定しない。OS Scheduling / Driver / Power State の Capture が必要
- 同一反復の基準前後でも Render 平均は約 0.02～0.04ms 変動した。数 us の差を改善として採用しない

## 改善候補と優先順位

### 1. ImGui 統計集計の Context Lock を短くする

`ImGuiManager::timing_info` は Context Lock を持って 120 Sample を Sort する
既存 `FrameController::timing_info` と同様に Sample を Lock 内でコピーし、集計を Lock 外へ移す案
GPU Wait の集計も所有 Snapshot へ分離する。Graph Performance の集計 Lock も同様に短くする

今回の実測で記録前 Lock 待機と集計取得の頻度を改善候補として確認した
この変更そのものの改善幅は未測定であり、別 Issue / develop 起点の実装 Branch で A/B 検証する
診断 UI の更新間隔も構成で選べるようにする案はあるが、p95 を改善する保証はない

### 2. Command の取得・提出を Native 処理と管理処理に分けて判断する

初期計測の平均 0.0493 / 0.1040ms は小さな C++ 検証だけの費用ではない
追加計測で Allocator Reset、List Reset、Token 生成、Queue 検証、Execute、Signal を分ける
外部 CommandList の Device / Type 検証や Fence の寿命確認は維持する
結果が Native 処理中心なら、Pool 初期容量や単純な Token 再利用で大幅に短縮できるとは言わない

### 3. Timestamp 周波数の取得を毎 Frame 行う必要性を確認する

平均 0.0136ms の取得がある。Queue の寿命に対応する取得・保持へ分離する候補
失敗時の伝播と再生成時の失効を維持し、API の契約と GPU Tick の変換を確認する
これは未実装の改善案であり、全体 0.1ms 到達の主対策ではない

API: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-gettimestampfrequency

### 4. Present の内部要因を Capture する

平均約 0.25ms が残る。現在の値だけでは Driver / DXGI / Desktop 合成 / Scheduling の割合は分からない
今回の Tearing 比較では確実な短縮を確認できなかった。Present の省略や GPU Fence 確認の削除は行わない
PIX / ETW 等で確認してから表示設定の変更を検討する

## 再現方法

通常 Build は調査機能が無効。調査用 Preset だけ `CUE_PERFORMANCE_STUDY=ON` にする

```powershell
. ./scripts/vcpkg_environment.ps1
$env:VCPKG_ROOT = get_vcpkg_root
cmake --preset windows-vs2026-performance
cmake --build --preset windows-vs2026-performance-development
pwsh -NoProfile -File Engine/Tests/Performance/RunEditorTimingStudy.ps1
```

- 出力: `out/validation/M06-PerformanceStudy/Runs/`
- 生 CSV と条件 JSON は ignored な `out/` に保存
- `RunMetrics.csv`: 反復別集計
- `PooledMetrics.csv`: 条件別に raw Sample をまとめた集計。p95 の平均ではない
- 出力先を再利用する場合も、Runner は今回生成した CSV だけを集計する
- 速度を CTest の合否条件にしない

## 検証と限界

- 調査用 Development Build 成功、通常 Debug Build 成功
- 初期 Hook の通常 Debug 関連 Test 5 件成功
- Probe の全 24 条件で Initialize / Step / Shutdown 成功、記録欠落 0
- PIX / Nsight / ETW Capture、手動での画面確認、GPU Clock 固定、Hook 有効 / 無効の費用比較は未実施
- 計測中に外部の集計確認も行っており、Background 負荷を隔離したベンチマークではない
- 試行間の変動がある。平均の小差だけで本実装の採用を決めない

Code と測定条件はこの調査 Branch に保持し、develop へ Merge しない

## 追加の Command 内訳調査

Hook を追加した別 Phase として基準条件だけ 3 回、計 1,800 Frame を測定した
元の A/B 24 実行には混ぜていない。合計は 27 Process / 16,200 Render Sample
追加 Phase も終了成功、記録欠落 0

| 区間 | 平均 ms | p95 ms | 最大 ms |
| --- | ---: | ---: | ---: |
| Render | 0.6631 | 0.9355 | 1.7387 |
| CommandAcquire | 0.0497 | 0.0643 | 0.2442 |
| Pool Lock 待機 | 0.0003 | 0.0003 | 0.1838 |
| Slot 走査・再利用準備 | 0.0479 | 0.0622 | 0.1357 |
| Allocator Reset（準備内） | 0.0090 | 0.0115 | 0.0320 |
| List Reset（準備内） | 0.0362 | 0.0471 | 0.1242 |
| CommandSubmit | 0.0993 | 0.1231 | 0.3749 |
| DX12 Completion 生成（Submit 内） | 0.0006 | 0.0009 | 0.0026 |
| Queue の Type / Device 検証（Submit 内） | 0.0003 | 0.0004 | 0.0226 |
| Queue Lock 待機（Submit 内） | 0.0001 | 0.0001 | 0.0003 |
| ExecuteCommandLists（Submit 内） | 0.0889 | 0.1098 | 0.3571 |
| Signal（Submit 内） | 0.0076 | 0.0103 | 0.0673 |

取得の約 91% は二つの Reset、提出の約 97% は Execute / Signal の API 呼出し中の経過時間だった
Driver 内の実行・待機や OS の Thread Scheduling を区別した値ではない
外部 List の検証削除や Token 再利用を優先しても、今回の測定条件では大幅な効果を期待する根拠がない
毎 Frame の新規 Slot 生成数自体は未計測であり、初期容量増加の効果も断定しない

```powershell
pwsh -NoProfile -File Engine/Tests/Performance/RunEditorTimingStudy.ps1 `
    -CommandBreakdown -OutputDirectory out/validation/M06-PerformanceStudy/CommandRuns
```

追加 Hook を含む実行 File で A/B 全体を再実行すると、元の値には追加計測費用が入る
異なる Hook 世代の数値を直接の高速化比較に使わない
`CommandMetrics.csv` と `CommandRunMetrics.csv` に追加 Phase の集計を保存した
最終 Source の通常 Debug Build と CommandPool / QueuePool Test 2 件も成功した

### 大きな最大値の区間

- `minimal-pixoff` 3 回目 Frame 156: Render 9.9201ms のうち ImGui 記録 Lock 待機 8.3048ms、Present 0.9701ms
- `minimal-readout15` 3 回目 Frame 694: Render 9.3621ms のうち Present 8.5730ms

少なくともこの二つは、同じ ImGui 記録本体の重さでは説明できない
Lock 所有 Thread と DXGI / Driver の Scheduling の確認が次の調査対象である
Present / Context Lock の長い裾を無視して平均だけを最適化しない
