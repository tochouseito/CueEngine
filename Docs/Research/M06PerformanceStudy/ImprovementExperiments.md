# M06 #101 改善案の比較実験

続き: [同期・Present・Command 再利用の追加検証](SchedulingExperiments.md)

## 作業境界

- Issue: https://github.com/tochouseito/CueEngine/issues/101
- Branch: `m06/issue101/render-performance-study`、比較基準は develop `02df9d1`
- この文書は 2026-10-08 の追加実験。前回の計測結果とは Sample を混ぜない
- 実験 Code は調査 Branch に保持し、develop へ Merge しない
- 本実装の採用時は別 Issue を作り、その時点の develop から別 Branch を作る

## 測定条件

- Windows 11 Home 10.0.26200 / Ryzen 7 3700X / RTX 3060、Driver 32.0.16.1047
- Development `/O2`、同一実行 File、Debug Layer / GBV は無効
- 実行 File SHA-256: `84BCDF7B4B32D064D13ED79C1C24210D01C8E737AAE28EFC3B4F6BEA3911A00F`
- 1280 × 720 Window、Test Window は 640 × 480、詳細診断 UI を維持
- Main / Update / Render、Worker 有効、Frame 2 枠、SwapChain 2 枠、60 FPS
- VSync / Tearing は無効、GPU Timestamp / PIX Marker は全条件で有効
- Layout の読込・保存なし。各 Process の Warmup 120 Frame を除外し、600 Frame を測定
- 10 案を各 3 回、各反復の前後に基準。最終 Phase は 36 Process / Render 21,600 Sample
- 初回は表の順、反復 2 と 3 は逆順で実施した
- 最終 Phase の測定中は Source 編集、Build、他の Test、統計の外部集計を実施しなかった
- 平均 / p95 / 最大は生 Sample を pooled 集計。p95 は nearest-rank、反復別 p95 の平均ではない
- API 区間は CPU 経過時間であり、Driver 内の待機や OS Scheduling を含む。GPU 実行時間ではない
- `GraphRecord` は `ImGuiRecordWait` を含む。`Main` は `Readout`、`Preformat`、`UiBuild` を含む

## 比較した案

| Case | 変更 |
| --- | --- |
| baseline | 既存経路、統計取得も GPU 統計集計も毎 Frame |
| statunlock | ImGui / Graph の TimingSamples を Lock 内で所有コピーし、集計を Lock 外へ移す |
| freqcache | Queue ごとに成功した Timestamp 周波数を保持し、Native API 再取得を省く |
| nthstats | 全 Sort を `nth_element` と `max_element` に置き換え、同じ nearest-rank p95 を求める |
| gpustats15 | GPU Query 回収と Sample 追加は毎 Frame、GPU 分布の表示用集計だけ 15 Frame 間隔 |
| readout15 | Main の統計取得を 15 Frame 間隔にする。Sample 収集と UI 構築は毎 Frame |
| combined | statunlock + freqcache + nthstats + gpustats15 |
| combined-readout15 | combined + readout15 |
| preformat | 診断テキストの整形と FrameProgress 取得を Context Lock 外へ移す |
| preformat-combined | preformat + combined |
| preformat-combined-readout15 | preformat + combined + readout15 |

`readout15` は 60 FPS 条件で約 4Hz の統計表示更新となる。FPS 用の進行情報取得は毎 Frame 継続する
GPU 統計の Cache は Pass 共通にし、描画枠ごとに古さの異なる値を交互表示しない
preformat は既存と同じ printf 書式、行順、行数、Window 設定を保ち、行ごとに `TextUnformatted` する
変動する数値の文字列であるため、全 Frame の Vertex 数まで同一に固定した比較ではない

## Render 全体の結果

単位 ms。基準 3,600 Sample、各案 1,800 Sample

| Case | 平均 | p95 | 最大 | 平均の基準比 |
| --- | ---: | ---: | ---: | ---: |
| baseline | 0.6411 | 0.8563 | 1.5274 | — |
| statunlock | 0.6356 | 0.8882 | 1.8868 | -0.85% |
| freqcache | 0.6402 | 0.8499 | 2.2708 | -0.14% |
| nthstats | 0.6377 | 0.8639 | 1.5907 | -0.52% |
| gpustats15 | 0.6435 | 0.8773 | 1.5065 | +0.39% |
| readout15 | 0.6013 | 0.8211 | 1.4262 | -6.21% |
| combined | 0.6386 | 0.8580 | 1.6142 | -0.38% |
| combined-readout15 | 0.5977 | 0.8143 | 1.6069 | -6.76% |
| preformat | 0.6400 | 0.8788 | 1.6859 | -0.17% |
| preformat-combined | 0.6395 | 0.8608 | 1.6284 | -0.25% |
| preformat-combined-readout15 | 0.6116 | 0.8432 | 2.1479 | -4.60% |

## 反復ごとの確認

各反復の前後の基準 Sample を合わせ、その反復の各案と比較した Render 平均の差

| Case | 反復 1 | 反復 2 | 反復 3 |
| --- | ---: | ---: | ---: |
| statunlock | -0.35% | +0.52% | -2.66% |
| freqcache | -1.16% | -0.62% | +1.33% |
| nthstats | -0.96% | +1.62% | -2.17% |
| gpustats15 | +1.37% | +1.94% | -2.10% |
| readout15 | -4.56% | -4.34% | -9.64% |
| combined | -0.05% | +1.69% | -2.73% |
| combined-readout15 | -6.33% | -5.40% | -8.50% |
| preformat | -0.96% | +1.18% | -0.71% |
| preformat-combined | +0.71% | +1.53% | -2.93% |
| preformat-combined-readout15 | -2.45% | -3.23% | -8.04% |

基準の前後だけでも平均差は最大約 0.022ms ある
単独案の数 us の平均差や、一つの最大値だけを根拠に採用しない
readout15 とその組合せは平均が 3 回とも小さい。一方、p95 は反復 2 で基準より大きくなる条件もある
この条件で平均短縮を確認したが、p95 / 最大を毎回改善する保証はない

## 対象区間の結果と解釈

| 対象 | 基準平均 ms | 変更後平均 ms | 解釈 |
| --- | ---: | ---: | --- |
| TimestampFrequency / freqcache | 0.01280 | 0.00065 | 再取得区間を短縮したが、Render 全体の平均はほぼ同じ |
| GPU Readback・集計 / gpustats15 | 0.01312 | 0.00644 | 約半分になったが、Render 全体で一貫した短縮はない |
| Main の統計取得 / nthstats | 0.04863 | 0.03842 | 1 回の集計を約 21% 短縮。Render 全体で一貫した短縮はない |
| Main 全体 / nthstats | 0.21530 | 0.20565 | Main の平均は小さい。全体 Render の改善とは分けて扱う |
| UI Build / preformat | 0.15119 | 0.12838 | Context 内の区間は短縮した |
| 新設 Preformat / preformat | — | 0.02623 | 新しい Context 外の仕事が発生する |
| Main 全体 / preformat | 0.21530 | 0.22005 | 処理移動の結果、Main の平均はわずかに増えた |
| ImGui 記録 Lock 待機 / statunlock | 0.08995 | 0.08973 | 想定した待機短縮は確認できない |
| ImGui 記録 Lock 待機 / readout15 | 0.08995 | 0.04702 | 平均で約 48% 短縮した |
| Main 全体 / readout15 | 0.21530 | 0.17159 | 平均で約 20% 小さい |
| GraphRecord / readout15 | 0.18032 | 0.13632 | Lock 待機を含む記録経過時間も短縮した |

Readout の個別 Sample は 1 回の取得費用。readout15 は Frame 数の 1/15 回だけ取得するため、
基準の 0.04863ms/Frame に対し、約 0.04993ms/15Frame の平均取得費用となる
Frame あたりの削減は Main 合計で確認する

周波数取得や GPU 集計を早く終えると Render が ImGui Lock へ早く到達し、待機時間が増える場合がある
今回の区間値はこの説明と整合するが、Thread Scheduling の Capture がないため因果は断定しない
独立の部分短縮を単純に足し、Render 全体の予測改善値としない

## 採用候補

### 第一候補: 診断統計の取得・表示更新周期を設定可能にする

readout15 単独が単純で、3 回とも Render 平均を短縮した
今回の 60 FPS 条件では Render 平均 -6.21%、Main 平均約 -20%、ImGui 記録 Lock 待機平均約 -48%
計測 Sample 自体は毎 Frame 収集し、診断表示の集計・取得だけ Cache する設計が候補となる
本実装では任意 FPS でも意図した更新周期を保てる設定、起動直後、停止、表示切替を設計する

combined-readout15 は最小平均だが、readout15 単独との差は約 0.0035ms に留まる
今回の変動幅では、追加 4 変更を同時採用する根拠にしない

### 個別候補: 周波数保持と p95 部分選択

対象区間の短縮を確認した。小さい変更として独立 Issue で契約・測定を再確認する余地がある
Render 全体の大幅な改善としては扱わない
周波数保持は成功後の Native API 再取得を省くため、その API の将来の失敗通知も省く
Queue / Device 再生成と Device Lost の契約を確認してから本実装する
API の Hz 単位と Queue ごとの取得は [Microsoft Timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing) を確認した

### 今回は高速化目的で優先しない

- statunlock: Render と Lock 待機の安定した短縮を確認できなかった
- GPU 分布集計の間引き: 表示の鮮度を下げる割に全体改善が確認できなかった
- preformat: Context 内の時間は減ったが Main へ移動し、Render 全体はほぼ同じだった
- すべて同時の変更: readout15 を含めない組合せは全体でほぼ同じだった

最良条件でも Render 平均は約 0.60ms。Present 平均約 0.25ms、CommandSubmit 平均約 0.096ms が残る
Present / Driver / OS Scheduling の解析が次の調査対象であり、Render 全体 0.1ms の達成を主張しない

## 検証・限界

- 全 36 実行が成功、選択 Render Sample 21,600、記録欠落 0
- 起動前に Sort / nth_element の結果一致を 8 Pattern × 260 時点で確認
- 空、単一、重複、昇順、降順、循環枠、負値、長い休止時間について全統計を比較
- 組合せ条件の短時間起動・停止も成功した
- Source の同期と表示経路を独立した read-only Review で確認した
- 調査用 Development Build と指定 Script による通常 Debug Build は成功
- Debug 関連 Test 10 件すべて成功、17.22 秒
- TimingSamples、QueuePool、CommandPool、FrameGraphFrames、MainFrameGraph、FrameController、ImGuiManager、
  ImGuiDX12Backend / WARP、EditorHost Lifecycle を検証
- PowerShell Runner の構文検証と `git diff --check` は成功
- PIX / Nsight / CueEngine MCP はこの Session に公開されておらず、Capture は未実施
- GPU Clock / Power State、Window 遮蔽、Background Process を固定していない
- Hook 自体の費用を独立評価していない。全比較は同じ Hook 世代を使用する
- 手動の画面比較、別 GPU / Driver、実ゲーム負荷、高 FPS の検証は未実施
- preformat は今回の既定 UI 専用。custom UI の不要な準備は本実装候補の設計で除く

## 探索用の初期 Phase

最終 Phase の前に 27 Process / 16,200 Render Sample も記録した
その際は Source の実装作業を並行したほか、GPU 集計 Cache が Slot 別となる初期版だった
一部で複数区間が同時に長くなる反復があり、採用判断の主表には使用していない
修正後の同一実行 File で全 10 案を再測定し、初期 Sample は混ぜずに保存した
今回の追加計測は計 63 Process / 37,800 Render Sample。短時間の起動確認はこの数に含めない

## Artifact と再現

- [全区間 pooled 集計](ImprovementPooledMetrics.csv)
- [反復別集計](ImprovementRunMetrics.csv)
- [同一反復の基準との比較](ImprovementPairedMetrics.csv)
- [実行・欠落数の確認](ImprovementValidation.json)
- 生 CSV / 条件 JSON / Environment / Log: `out/validation/M06-PerformanceStudy/FinalImprovementRuns/`
- 初期 Phase: `out/validation/M06-PerformanceStudy/ImprovementRuns/`

```powershell
. ./scripts/vcpkg_environment.ps1
$env:VCPKG_ROOT = get_vcpkg_root
cmake --preset windows-vs2026-performance
cmake --build --preset windows-vs2026-performance-development
& ./Engine/Tests/Performance/RunEditorTimingStudy.ps1 -Cases @(
    'statunlock','freqcache','nthstats','gpustats15','readout15',
    'combined','combined-readout15','preformat','preformat-combined','preformat-combined-readout15'
) -OutputDirectory out/validation/M06-PerformanceStudy/RepeatImprovementRuns
python Engine/Tests/Performance/SummarizeEditorTimingStudy.py out/validation/M06-PerformanceStudy/RepeatImprovementRuns
```

Runner は現在、入力 Case 配列を反復ごとにコピーする。次回実行は反復 3 を初回順へ戻す
本計測時の順序とこの修正を区別し、同じ数値の再現を保証するものではない
