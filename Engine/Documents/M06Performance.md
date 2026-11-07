# M06 Performance Tests

Milestone: [M06](https://github.com/tochouseito/CueEngine/milestone/7)

## Issue と実装範囲

| Issue | 対策 |
| --- | --- |
| [#88](https://github.com/tochouseito/CueEngine/issues/88) | Thread ごとの高精度 Timer、停止 Event、FPS 待機の短い Spin |
| [#89](https://github.com/tochouseito/CueEngine/issues/89) | Compute ShaderRead の NonPixel 化、Graphics 専用 State と Copy の受渡し |
| [#90](https://github.com/tochouseito/CueEngine/issues/90) | 常駐 Resource の `acquire_wait`、GPU 参照完了後の再借用 |
| [#91](https://github.com/tochouseito/CueEngine/issues/91) | Command 容量の最古 Fence 待機、同じ Queue の連続 Pass の一括記録 |
| [#92](https://github.com/tochouseito/CueEngine/issues/92) | 同じ互換 Read State の Queue 間並列、Write／UAV／Copy 境界の排他 |
| [#93](https://github.com/tochouseito/CueEngine/issues/93) | ImGui VB／IB Ring と Texture ごとの最終 Fence |
| [#94](https://github.com/tochouseito/CueEngine/issues/94) | Graph Callback を Context Lock 外で実行、Snapshot と Texture Pin の所有維持 |
| [#95](https://github.com/tochouseito/CueEngine/issues/95) | 描画枠ごとの Snapshot と DrawList 配列容量の再利用 |
| [#96](https://github.com/tochouseito/CueEngine/issues/96) | Build の実行表、枠ごとの Binding／Callback／Barrier／Pool Lease 配列再利用 |
| [#97](https://github.com/tochouseito/CueEngine/issues/97) | 提出時に発行した Completion Fence を共有、二重 Signal の除去 |
| [#98](https://github.com/tochouseito/CueEngine/issues/98) | ImGui 設定文字列を所有コピーし、File 保存を Context Lock 外へ移動 |
| [#99](https://github.com/tochouseito/CueEngine/issues/99) | ScopedFlag、SharedCommandCompletion、許可 Draw Callback 判定の共通化 |
| [#100](https://github.com/tochouseito/CueEngine/issues/100) | CPU 区間と GPU Pass の 120 Sample、平均・p95・最大、PIX Marker |

## 同期上の制約

Resource の `acquire_wait` は CPU 側で GPU 完了を待つ安全な経路。既存 `acquire` は非待機のまま。現在 CPU に貸出中の Resource は待っても回復できないため Error にする。GPU Queue 内の非同期依存待機へ自動変換する API は含めない

CommandPool は初期 Context 数 0、Queue 種類ごとに最大 32 を維持する。全 Slot が GPU 提出待ちの場合だけ最古の提出を Mutex 外で待ち、CPU に全数貸出中の場合は Error にする。同じ Queue の連続 Pass をまとめるため、Pass 数と Command Context 数を一致させない

Read の並列実行は State が一致する Graphics／Compute の互換 Read のみ。UAV は Read 宣言でも Queue 間排他。Copy に渡す State 遷移は全先行 Reader の終了へ依存する。Pool Resource の State 変更は Write Lease で扱い、Frame 間で旧 State 復元と新しい読み取りが競合しないようにする

GPU Timestamp の Query Heap と Readback は Queue と描画枠ごとに所有し、同じ Buffer へ別 Queue が同時に書き込まない。GPU 完了後だけ CPU が読む。Copy Queue の Timestamp 非対応時は未計測とする。ImGui は公式 Backend が実際に進めた Ring を追跡し、Skip した Frame を Ring 使用として数えない。停止・Resize の回収では全面待機を維持する

## 計測の読み方

Main／Update／Render Callback、FPS 待機、UI 構築、Snapshot、Context 待機、ImGui GPU 待機、Graph 記録、描画枠 Fence 待機、Present と GPU 各 Pass を区別する。直近 120 Sample の平均・nearest-rank p95・最大を返す。GPU は提出時点ではなく完了後の Timestamp を集計する

区間は入れ子なので、Render と Graph 記録／Present を合算しない。Graph の枠待機には Query 回収を含む。ResourcePool／CommandPool の待機は Render 全体へ含まれるが、現在の UI では個別に分離しない。計測自体の時計取得、Timestamp、Lock、Snapshot 集計の負荷があり、計測なしとの GPU 性能比較は未実施

PIX の公式 API を Debug／Development の Pass 名で記録する。Release は PIX Marker を無効にする。API 導入と GPU Timestamp の検証は、PIX Capture の実取得と区別する

## FrameController 比較

2026-10-07、Windows、AMD Ryzen 7 3700X、Visual Studio 2026、Release `/O2`。GPU／Window なしで同じ Controller Source と Windows Service を Compile し、空 Callback、上限 60 FPS、120 Frame を各構成 2 回測定した。変更前の分岐は `develop` の `1e2d5c0`。計測 Source は [FrameControllerTimingProbe.cpp](../Tests/Performance/FrameControllerTimingProbe.cpp)

| 構成 | 変更前の平均 FPS | 変更後の平均 FPS | 変更後の直近完了間隔 |
| --- | --- | --- | --- |
| Single、1 枠 | 32.63–32.69 | 60.50 | 16.667 ms |
| Worker、1 枠 | 32.56–32.62 | 60.47–60.50 | 16.667 ms |
| Worker、2 枠 | 32.43–32.67 | 60.49–60.50 | 16.667 ms |

最初の Frame は待機しないため、120 Frame を壁時計で割った値は 60 FPS を少し超える。実際の完了間隔で上限を確認する。空 Callback の待機精度の比較であり、Engine の描画速度、GPU 負荷、電力削減を示すものではない。標準 `sleep_for(16 ms)` の Callback は変更しておらず、OS の Sleep 粒度の影響が残る

Raw CSV: [変更前](../../Docs/Research/M06Performance/FrameControllerBefore.csv)／[変更後](../../Docs/Research/M06Performance/FrameControllerAfter.csv)。CPU 時間の短い Sample は `GetProcessTimes` の粒度の影響が大きいため、CPU 使用率改善の根拠にしない

再現用 Target は `CueFrameControllerTimingProbe`。Release Build 後、`out/build/windows-vs2026/Engine/Tests/Release/CueFrameControllerTimingProbe.exe` の標準出力を CSV へ保存する。計測中の他の Build、GPU Test、常駐負荷などの条件も併せて記録する

## 検証

2026-10-07 の最終 Source で、`scripts/codex_build.ps1` による MSBuild と各 CTest Preset が成功した。NuGet restore は実行していない

| 構成 | Build | CTest | 全 Test 経過時間 |
| --- | --- | --- | --- |
| Debug x64 | 成功 | 39 / 39 | 37.01 s |
| Development x64 | 成功 | 39 / 39 | 27.53 s |
| Release x64 | 成功 | 39 / 39 | 25.67 s |

実 Adapter と WARP の実画素、Compute State 持越、Command 32 個の GPU 待機と 33 個目の再利用、ImGui Snapshot 再利用、Graph Callback 中の Main UI 構築、Resize／最小化／復帰／終了を確認した。通常の D3D12 Warning／Error／Corruption の Break 設定を維持して合格した

追加テストで発見した Fixture の Descriptor 容量不足を解消し、Warning 1328 が出る Buffer の初期 ShaderRead 指定は COMMON へ変更した。ShaderRead の持越は Texture で別に検証する。独立レビューで発見した Queue 別 Readback、UAV 排他、Reader 合流、Pool State 遷移の所有制約も修正した

Raw Log は `out/validation/M06-Performance/{debug,development,release}-{build,tests}.log`。生成物は Git 管理しない。実 PIX／Nsight Capture、手動での UI 表示確認、長時間の高負荷・電力計測は未実施。Resource／Command 個別待機や計測機能自体の負荷は今後の測定対象として残る
