# Issue #101: SwapChain の資料に基づく追加比較

2026-10-09、`m06/issue101/render-performance-study` で試作・計測した。研究 Branch は未マージ

## CPU 比較の結果

**今回の軽い描画・60 FPS 条件では、Present 呼出時間を安定して短縮する設定は見つからなかった**

一方、Borderless / NONEでTearingを許可すると、Presentから表示までの平均時間は7.60→3.57msに下がった。p95とCPU呼出時間は改善していない。CPU高速化と表示遅延を別に判断する

Buffer 数、最大表示待ち Frame 数、Scaling を組み合わせて検証した。CPU Frame 枠は全条件で2、GPU完了確認と Resource 寿命管理は維持した

### 通常 Window、1280×720

単位は ms。各候補1800 Sample、前後の基準は合計3600 Sample。p95は生 Sample の nearest-rank

| 条件 | Buffer / 表示待ち上限 | Native Present avg / p95 | Render avg / p95 |
| --- | --- | --- | --- |
| 基準、STRETCH | 2 / 未設定 | 0.2423 / 0.3270 | 0.6006 / 0.7777 |
| Waitable、既存比較条件 | 2 / 2 | 0.2509 / 0.3643 | 0.6128 / 0.8187 |
| Waitable、待ち上限1 | 2 / 1 | 0.2447 / 0.3314 | 0.6068 / 0.8086 |
| Waitable、Buffer 3枚 | 3 / 2 | 0.2434 / 0.3229 | 0.5996 / 0.7680 |
| Waitable、Buffer 3枚・上限3 | 3 / 3 | 0.2486 / 0.3327 | 0.6098 / 0.7900 |
| Waitable、Buffer 4枚・上限4 | 4 / 4 | 0.2460 / 0.3310 | 0.6049 / 0.7884 |
| Scaling NONE | 2 / 未設定 | 0.2470 / 0.3377 | 0.6117 / 0.8132 |

Buffer 3 / 上限2のRender平均差は約-0.0010ms。反復ごとの変化は +0.49% / +0.02% / -1.00%で、一貫しない

Waitableの追加待機は平均0.0034～0.0036ms。Renderの計測範囲はこの待機を含む。表示受付でほぼ待たない条件であり、GPU高負荷時や低遅延設定一般の効果を否定する試験ではない

Buffer 3 / 上限2はBuffer 2 / 上限2との比較で枚数だけを変える。Buffer 3 / 上限3は上限だけを変える。Buffer 4 / 上限4は複合条件として扱う

### Borderless、1920×1080

通常 Window と画素数が異なるため、上のPhaseとは改善率を比較しない。全条件で同じBorderlessとBuffer 2枚を使う

| 条件 | Native Present avg / p95 | Render avg / p95 |
| --- | --- | --- |
| 基準、STRETCH、Tearingなし | 0.2869 / 0.3951 | 0.6526 / 0.8601 |
| NONE、Tearingなし | 0.2868 / 0.4104 | 0.6510 / 0.8541 |
| NONE、Tearingあり | 0.2916 / 0.4015 | 0.6622 / 0.8763 |

NONEのRender変化は -0.87% / -1.56% / +1.70%。NONE + Tearingは +1.91% / +1.77% / +0.75%。CPU時間短縮のための採用根拠は得られなかった

## PresentMon による表示経路・表示時間

CPU比較と別のProcessでPresentMon 2.6.0を使用した。初回は3条件を1回ずつ確認し、表示時間の差があったNONE / NONE + Tearingだけ追加2回検証した。合計7 Process、各Processは120 Warmup + 600測定Frameで正常終了

| 条件 | Process数 / 選択行 | PresentMode | MsInPresentAPI avg / p95 | MsUntilDisplayed avg / p95 |
| --- | --- | --- | --- | --- |
| STRETCH、Tearingなし、初回の補助確認 | 1 / 598 | Hardware Composed: Independent Flip | 0.2879 / 0.4074 | 7.4765 / 13.4218 |
| NONE、Tearingなし | 3 / 1794 | Hardware Composed: Independent Flip | 0.2901 / 0.4023 | 7.5971 / 13.5264 |
| NONE、Tearingあり | 3 / 1794 | Hardware: Independent Flip | 0.2945 / 0.4452 | 3.5695 / 13.8474 |

NONE / NONE + Tearingの各ProcessのMsUntilDisplayed平均は7.72→3.52、7.62→3.65、7.45→3.54ms。3回とも平均時間が下がったが、まとめたp95は改善していない。Present呼出時間の高速化として採用しない

全選択行で表のPresentModeを確認し、ETW Lost Event / Buffer / Overflowed Presentは0。DXGIがTearingを実際に許可したことと、生成Flagsも確認した

PresentMonのFrame行はEngineのFrameIdと結合せず、先頭120行を除き最大600行を採用する補助観測。生CSVは718行で、選択は598行となった。ETW欠落0でもEngineの600 Sampleと同一の範囲とは保証しない。2 PhaseのCaptureを混ぜる集計では記録元Directoryを保存し、同じrepeat番号を別Processとして区別する

MsUntilDisplayedはPresentからOSが報告する表示時刻までであり、入力から表示までの遅延ではない。Tearing条件は画面の一部更新を許すため、完全なFrame全体の表示完了とも同一視しない。表示時間目的の設定候補にはなるが、Tearing許容とp95を含めた判断が必要

NONE自体はCPU比較で効果を確認していない。Tearing単独でも同じ表示時間の差が出るかは、このBorderless条件で未検証。現時点でNONEを必須設定にしない

- [表示経路検証](SwapChainDisplayValidation.json) / [表示時間集計](SwapChainDisplayPooledMetrics.csv)
- [初回Capture環境](SwapChainDisplayCaptureEnvironment.json) / [追加Capture環境](SwapChainDisplayCaptureConfirmEnvironment.json)
- 指標の意味は [PresentMon公式資料](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md) を参照

## 実装と検証

- `PerformanceStudy.h`: 調査専用の最大表示待ち上限とScaling選択、生成時の実DXGI条件の保存
- `DX12SwapChain.cpp`: 条件に応じたSetMaximumFrameLatency / Scaling、調査BuildだけGetDesc1 / GetMaximumFrameLatencyで照会
- `EditorTimingProbe.cpp`: 比較条件、実設定のJSON保存、NONEのLifecycle試験
- `SummarizeSwapChainStudy.py`: 実Buffer / 上限 / Flags / Scaling / 寸法、Sample範囲と件数、前後基準との比較を検証

通常Buildの設定は変更していない。実上限の0は、非Waitable条件で未設定・未照会という記録であり、待機可能Frame数0ではない

- 規定のDebug Build成功、計測用Development Build成功
- 関連Debug CTest 7件成功、16.65秒
- NONE / Waitableの最小化・復帰・Resize・停止成功、それぞれ100 Render Frame、記録欠落0
- CPU比較36 Process / 21600測定Render Sample、全実行成功、記録欠落0
- 実DXGI条件と要求値の一致、同一実行File Hashを検証
- 読み取りレビューで追加条件と同期・寿命管理を確認

サンドボックス内BuildはMSVC C1902で失敗し、通常ユーザー環境で再実行して成功した

## 条件・保存先

Ryzen 7 3700X / RTX 3060 / Windows 11 26200 / Driver 32.0.16.1742。MSVC x64 Development / O2、D3D12 Debug Layer / GBV無効。60 FPS、Worker有効、CPU Frame 2枠、VSync無効。ClearFinalColorと詳細統計ImGuiを描画し、統計表示は15 Frameごと。GPU Timestamp / PIX Markerは有効

以前の調査のDriver 32.0.16.1047から更新されている。Driverによる改善率は今回検証していない。採否は今回の同一Driver・同一実行Fileの比較で判断する

各候補3回、120 Warmup + 600測定Frame。各反復の前後に基準を配置し、2回目は条件の順序を反転。GUIの遮蔽・電源状態・外部負荷は完全には制御していない。異なる過去Phaseと今回の値を混ぜない

実行File SHA-256: `8F4C34085A90617E7D5523A750E2B358B7ED646307AA0186091D7DF1FB8D921B`

- [通常Window集計](SwapChainLatencyPooledMetrics.csv) / [反復ごとの比較](SwapChainLatencySwapChainPairedMetrics.csv) / [実条件検証](SwapChainLatencySwapChainValidation.json) / [環境](SwapChainLatencyEnvironment.json)
- [Borderless集計](SwapChainBorderlessPooledMetrics.csv) / [反復ごとの比較](SwapChainBorderlessSwapChainPairedMetrics.csv) / [実条件検証](SwapChainBorderlessSwapChainValidation.json) / [環境](SwapChainBorderlessEnvironment.json)
- 生CSV・ログは `out/validation/M06-PerformanceStudy/PresentationPaths/SwapChainArticles-20261009/` 以下

## 参照資料

- [NVIDIA: Advanced API Performance: Swap Chains](https://developer.nvidia.com/blog/advanced-api-performance-swap-chains/): CPU投入Frame数より1～2枚多いBuffer、表示待ち上限の調整
- [Microsoft: SetMaximumFrameLatency](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency): Waitable Flagが必要
- [Microsoft: DXGI_SCALING](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ne-dxgi1_2-dxgi_scaling): NONEは拡縮せず、寸法不一致時は背景領域が生じる
- [Microsoft: Flip Model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model): 表示経路、Independent Flip、Tearing

入力から表示までの実測とGPU高負荷時の検証は未実施。NONEのResize中の見た目は目視確認していない。本実装への採用は新規Issueとdevelopからの別Branchで行う
