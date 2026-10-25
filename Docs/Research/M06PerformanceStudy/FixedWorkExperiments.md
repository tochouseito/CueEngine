# Issue #101: 固定処理・ImGui・表示経路の比較

2026-10-08、`m06/issue101/render-performance-study` で実装・実測した研究結果
研究 Branch は未マージ。採用する変更は新しい Issue と `develop` からの Branch で実装する

## 結果

**採用候補は Descriptor の逆引きと使用登録の重複排除。ImGui の独立記録は追加調査候補**
FrameGraph 準備 Cache、Waitable SwapChain、Borderless には、この軽い描画条件で一貫した大幅な Render 短縮を確認できなかった

| 案 / 比較条件 | 基準 Render 平均 / p95 (ms) | 試作 Render 平均 / p95 (ms) | 判断 |
| --- | --- | --- | --- |
| FrameGraph 固定準備の Cache、1280×720 | 0.6030 / 0.8045 | 0.6001 / 0.8250 | 個別準備は短縮したが Render 全体への効果は小さい |
| ImGui の Context と Native 記録の分離、1280×720 | 0.6030 / 0.8045 | 0.5817 / 0.8567 | 平均約3.5%短縮、p95 と Main は悪化。追加調査 |
| Waitable SwapChain、1280×720 | 0.6030 / 0.8045 | 0.6098 / 0.8187 | 今回は改善を確認できない |
| Borderless、双方1920×1080 | 0.6704 / 0.9378 | 0.6633 / 0.8887 | 平均約1.1%短縮。反復ごとには改善・悪化が混在 |
| Descriptor 逆引き、通常 UI、1280×720 | 0.6030 / 0.8045 | 0.6035 / 0.8061 | Font 中心の通常 UI では効果が小さい |
| Descriptor 逆引き、256 Texture UI、1280×720 | 0.6811 / 0.8834 | 0.5876 / 0.7996 | 平均約13.7%、p95 約9.5%短縮。3反復すべて改善 |

CPU 比較に採用したデータは **36 Process / 21,600 Render Sample**。全実行正常終了、記録欠落0
PresentMon は別の **9 Process / 5,382選択行**。CPU 比較に ETW 付きの値を混ぜていない

## 条件と測定方法

| 項目 | 条件 |
| --- | --- |
| OS / CPU | Windows 11 Home 10.0.26200 / Ryzen 7 3700X、8 Core / 16 Logical Processor |
| GPU / Driver | RTX 3060 / 32.0.16.1047 |
| 実 Device | Vendor 4318 / Device 9475、Dedicated Memory 12,703,498,240 bytes |
| Build | MSVC 19.51 / x64 / Development / O2、D3D12 Debug Layer・GBV 無効 |
| Monitor | Primary Monitor、1920×1080、75 Hz |
| Frame / Present | Worker 有効、FrameController 二枠、SwapChain 二枠、60 FPS、VSync・Tearing 無効 |
| 通常 UI | 既定 Test Window と詳細統計表示。統計表示取得は15 Frameごと |
| Texture 比較 | 同じ256枚の4×4 RGBA Texture、16×16の画像配置、Test / TEST表示。詳細統計 UI は使わない |
| GPU Timestamp / PIX Marker | CPU 比較の全条件で有効。PIX Capture を実施した意味ではない |
| 反復 | 各条件3回、各Processで120 Frame Warmup + 600 Frame測定 |
| CPU 比較順 | 各反復の前後に基準条件を配置。2回目は提案の順番を反転 |
| 観測 | 固定容量の生 Sample Collector。測定 Frame 内では File IO、結果 Sort、外部収集をしない |
| p95 | 反復の生 Sample をまとめた nearest-rank。各反復のp95の平均ではない |

Hook と Clock Read の観測負担を含む。各 Metric は入れ子や待機を含むため、無条件に合計して CPU 稼働時間にしない
GUI の遮蔽、電源・Clock、外部 Process の負荷は完全には制御していない

1280×720の比較とTexture比較の実行File SHA256:
`03B2D7937F5D5BB20CE01DB94B29E811EBB05BA343A50D9940EB44E989E69163`

寸法補正後の1920×1080比較とPresentMonの実行File SHA256:
`17802422F87EC99B5346AAAF9D2B880702747CC583437FB3F7EE0D773E92CAB9`

後者は比較用通常Windowの寸法補正をProbeへ追加したもの。Rendererの試作、計測Hook、1280×720条件は変更していない

## 1. FrameGraph の固定準備 Cache

`cacheGraphPreparation`でImmutable PlanとNative Barrierの準備を再利用する
Plan ID、Device、外部BindingのHandle・Native Pointer、内部Resource Pointerを毎回照合する
CacheはCOM参照を保持し、解放後に同じAddressが割り当てられる問題を防ぐ。ResizeはGraphを再構築してCacheを破棄する
Barrier自体、Fence・Lease確認、必要Viewの確認は各Frameで継続する

- Native準備平均: **0.004459 → 0.001048 ms**、約3.4µs短縮
- Cache条件の測定1,800 FrameはすべてHit。二枠・二Bufferの同じBindingを再利用できた
- Renderの反復差: **-0.47% / +0.76% / -1.76%**。全体の改善とは断定しない
- 二枠・三Bufferの条件も短い試験で確認したが、正式な性能比較には含めていない

## 2. ImGui の Native 記録分離

`separateImGuiRecording`で研究用の独立Recorderを使用する
Mainのpublish時にTexture所属とCallbackを検証し、DrawDataの固定IDとTexture Pinを公開する
Renderは共有Contextを取得せず、所有Snapshot、Render専用のVB/IB Ring、PSO、RootSignatureから記録する
提出後の回収LockでDescriptorのFenceを登録し、その後Texture Pinを解除する
Textureの生成・更新・破棄は既存のMain経路と公式Backendを維持する

| Metric | 基準平均 (ms) | 試作平均 (ms) |
| --- | --- | --- |
| GraphRecord | 0.14234 | 0.07826 |
| ImGuiRecord | 0.03605 | 0.02575 |
| 記録前Context待機 | 0.04849 | 記録時のLock取得を除去 |
| 提出後回収Lock待機 | 0.00023 | 0.01621 |
| Present | 0.23874 | 0.26185 |
| Main | 0.17828 | 0.20616 |
| Main p95 | 0.2845 | 0.4081 |

GraphRecordは下がったが、回収時の競合が残り、検証のMainへの移動とPresent増加も含む
Renderの反復差は **+4.13% / -4.51% / -10.37%**。一貫した全体改善の証拠には不足する
次に調べるなら回収LockとMainの重なりを対象にし、単に待機を別Threadへ移す変更は区別する

試作RecorderはRGBA8の現在の契約と標準Sampler/Reset Callbackを扱う。未知のDraw Callbackは拒否する
第三者Sourceは変更せず、MIT参照元の通知を研究Headerへ保持している
Shaderは`Engine/Shader/Hlsl/StudyImGui.hlsl`

## 3. Waitable SwapChain

`waitableSwapChain`でFrame Latency Waitable Objectを生成し、描画前に待機する
最大Present Latencyは2に固定して既存二枠と比較する。Stop Event、Timeout、Resize時のFlag保持、Handle解放を追加した

- 追加待機平均 **0.003178 ms**、p95 **0.0043 ms**。60 FPS制御下ではほぼ待機しない
- Render差: **+3.74% / -1.32% / +0.93%**。改善を確認できない
- Borderlessと組み合わせても、後述の表示待ち短縮は確認できない
- 低遅延目的、75 Hzに近い投入速度、GPU負荷が増えた場合の評価は今回実施していない

[GetFrameLatencyWaitableObject](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)と
[SetMaximumFrameLatency](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency)の契約に従う

## 4. Borderless と表示経路

初回は通常WindowがOSによって1920×1061へ調整された。寸法が異なる比較を除外し、Probeの初期化後に通常Windowを補正してWarmup中にResizeを反映した
最終比較はGetClientRectと実メタデータで双方1920×1080を確認している
通常Windowの外枠は1936×1119で画面外へはみ出す。描画Bufferの画素数は同じだが、装飾と可視領域の違いは残る

CPUの反復差は **-3.59% / -1.28% / +1.66%**。平均約1.1%の差を安定した高速化とは判断しない

PresentMon 2.6.0を公式Releaseから取得し、署名とSHA256を確認した
現在の権限ではProcess名の取得・終了通知を利用できなかったため、Host初期化前にPIDを取得してそのPIDだけを追跡し、自分のETW Sessionを明示停止した
`Application=<unknown>`でも、起動したCueEngineのPIDとの一致を検証している

| 条件 | 選択行のPresentMode | 表示時刻が取得できた行 | MsUntilDisplayed 平均 / p95 (ms) |
| --- | --- | --- | --- |
| 通常Window | Composed: Flip、1,794行 | 133 / 1,794 | 22.216 / 29.583 |
| Borderless | Hardware Composed: Independent Flip、1,794行 | 1,794 / 1,794 | 8.973 / 15.363 |
| Borderless + Waitable | Hardware Composed: Independent Flip、1,794行 | 1,794 / 1,794 | 9.270 / 15.920 |

表示経路の違いは確認できた。通常Windowは表示時刻が取得できた行が少なく、条件付きの値なので、この表だけで全Frameの遅延短縮を断定しない
NAを未表示やDropの証拠とは扱わない
PresentMonは各Capture718行から先頭120行を除外し、598行を選択した。Engine Frame IDとの対応付けは行っていない
全Captureで、取得中に報告されたEventsLost / BuffersLost / OverflowedPresentsは0。終了後の最終カウンタを別APIで取得したものではない
ETWのMsGPUTimeはEngine PassのGPU Timestampではなく、GPU待機も含む観測値なので、Shader実行時間として採用判断しない
指標の定義は[PresentMon公式資料](https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README-ConsoleApplication.md)を参照

## 5. Descriptor 逆引きと使用登録

`descriptorLookup`でGPU HandleのHeap物理IndexからLogical Slotを逆引きする
予約・解放時にMapを更新し、範囲、Alignment、Slotの有効性・予約状態、Native IDの一致を確認する
同じTextureの使用登録は最初の一度だけ処理する。Snapshot Pinと最終Fenceの寿命契約を維持する

256 Texture条件:

- Descriptor検証平均 **0.062729 → 0.001976 ms**
- 使用登録平均 **0.037986 → 0.001887 ms**
- Render改善率 **16.06% / 13.05% / 12.05%**、3反復すべて改善
- Main平均 **0.16449 → 0.16297 ms**。Renderの削減をMainへ移した結果ではない

CPU単独試験も実施。各条件2,048 Command × 32 Round、7反復、全168行で線形探索と逆引きのChecksum一致を確認した

| 容量 / 複数Textureを参照 | 線形Lookup (ns/Command) | 逆引きLookup (ns/Command) |
| --- | --- | --- |
| 32 | 13.20 | 2.23 |
| 256 | 99.85 | 2.17 |
| 1,024 | 463.66 | 2.32 |

Fontが先頭Slotにある検索だけなら線形探索約2nsと同等。使用登録は容量1,024で約667nsから約2.6nsへ減った
単独試験はCPU費用の分離用であり、Render全体の倍率に換算しない

## 検証と残る制約

- `scripts/codex_build.ps1`による通常Debug Build成功
- 研究Development Build、研究DebugのBackend / MainFrameGraph / Probe Build成功
- 通常Debugの関連CTest **9件成功**
- WARPで公式記録、独立記録、Descriptor逆引き、それらの組合せをDevelopmentとDebugで比較。各条件9画像、全RGBA Checksumが一致
- Glyph、半透明、Sampler切替、Reset Callback、FramebufferScale、Texture生成・更新・破棄・容量制限を含む既存試験成功
- Cache有効のMainFrameGraph Resize・再構築試験はDevelopment / Debugとも成功。DebugのInfoQueue・Live Object確認も成功
- 実Hostの各条件を短時間実行。Waitableは最小化・復帰・Resize・停止を確認
- Debugの実Main/Render並行描画、256 Texture、Waitable Lifecycleも正常終了
- 生Render Frame IDが各ProcessでWarmup後の600個に一意対応すること、寸法、Monitor、Frame設定、出力欠落、画素Checksum、Micro試験のChecksumを検証

PIX / Nsight Capture、長時間実行、GPU負荷のあるScene、他GPU、75 FPS以上、HDR・任意Draw Callbackは未検証
研究Recorderのprepared経路はManagerの有効Render Scope内だけから呼ぶ契約が必要。本実装ではprivate APIやTokenで制限する余地がある
通常UIへのDescriptor逆引き採用は将来の拡張性を理由に判断できるが、現状のFont中心UIを大幅に短縮するという説明にはしない

## 保存データと再現

- `Fixed*Metrics.csv` / `FixedEnvironment.json`: 1280×720の4案
- `DescriptorStress*`: 256 Texture比較
- `PresentationExact*`: 寸法補正後の通常Window / BorderlessのCPU比較
- `DescriptorMicro.csv` / `DescriptorMicroSummary.csv`: CPU単独試験
- `PresentMon*`: 寸法一致後の表示経路・各反復・生Sampleからの統合集計
- `FixedWorkValidation.json`: 36 Processの寸法・Render Sample検証と画素Checksum

生CSVとLogは`out/validation/M06-PerformanceStudy/`、PresentMon Captureはその`PresentationPaths/FurtherExact/`に保存
寸法不一致の`FurtherPresentationCpu`と`PresentationPaths/Further`は結論用データから除外した

比較入口は`RunEditorTimingStudy.ps1`、集計は`SummarizeEditorTimingStudy.py`
通常比較はBaseline `new-baseline`、Cases `new-cache/new-snapshot/new-waitable/new-descriptor`
Texture比較はBaseline `new-stress`、Case `new-descriptor-stress`
表示比較はBaseline `new-screen-windowed`、Case `new-borderless`
PresentMonは`RunPresentationPathStudy.ps1`と`SummarizePresentationPaths.py`を使い、`PresentationPaths`内の新しい出力先へ取得する
