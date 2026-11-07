# M06-15 Waitable 待機の条件確認と再測定

続報: [GPU Timestamp と CPU 提出・表示待機の対応](M06-15-Gpu-Timestamp-And-Presentation.md) で約 33 ms の表示待機と約 8.8 ms の提出後の遅延を分離した

- Date: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 前報: [Scheduling / GPU Fence / DWM の追加調査](M06-15-Frame-Spike-Scheduling-And-Presentation.md)
- 状態: 調査用 Probe / Script を拡張。Engine の実行方針は変更していない。未 Commit / 未 Push / 未 Merge

## 結果

1. **前報の約 50 ms は今回再現しなかった。** Queue latency 2 は前面を保った ETW 5 秒、ETW なし 5 秒、ETW 20 秒の計 30 秒でも再現しなかった。問題が解消したという結論にはしない。
2. 20 秒記録の最長 Frame は **10.7303 ms**。Waiting 10.0524 ms、Ready 0.0075 ms で、DWM が Render を Ready にしていた。
3. 前面状態を実測で揃えた比較は取得できた。ただし、Background Process の負荷は固定しておらず、設定ごとの十分な反復もない。Queue latency 2 を標準に変える根拠にはしない。
4. Hardware Scheduling Log を取得したが、**GPU Signal submit → completion の内側の分離は未達**。後述の公開 API / Table 出力では、測定中の対象 Process の GPU 実行区間を得られていない。

## 前面状態を確認した理由

既存 Probe は `SetForegroundWindow()` を要求していたが、その成功は確認していなかった。最初の EtwA では両設定とも測定中は Background のままだった。Windows はこの要求を拒否できるため、要求したことを条件達成と扱わない。[SetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)

今回は Host Step 境界で、Foreground、可視 Flag、最小化、Client Size を観測した。前面を要求する実行では User のクリックを待ち、Warmup 後に前面であることも検査した。

- Foreground の各実行で mismatch 0 / change 0 を確認
- Client Size は 1280×720、可視、非最小化を維持
- これは Step 境界の観測であり、完全な遮蔽や境界間の全ての状態変化を検出するものではない
- `IsWindowVisible()` を遮蔽の検出として使っていない
- 計測中に前面化を繰り返す、他 Window へ擬似入力する、System 設定を変える処理は追加していない

## 記録の負荷を区別

| 記録 | 秒数 / 設定 | ETW の範囲 | Window 状態 |
| --- | --- | --- | --- |
| EtwA | 各 3 秒 / latency 1, 2 | 全区間の即時 Marker、Kernel、GPU Base / Scheduler / HardwareScheduling、DXGI / DWM | 両方 Background で一定 |
| EtwB | 各 5 秒 / latency 1, 2 | 全区間の遅延 Marker、Kernel、表示関連 Provider | latency 1 は途中で前面→Background、latency 2 は Background 一定 |
| EtwForeground | 各 5 秒 / latency 1, 2 | 全区間の遅延 Marker、Kernel、表示関連 Provider | 両方 Foreground 一定 |
| NativeForeground | 各 5 秒 / latency 1, 2 | ETW なし、全区間 CSV は保持 | 両方 Foreground 一定 |
| EtwLong | 20 秒 / latency 2 | 全区間の遅延 Marker、Kernel、表示関連 Provider | Foreground 一定 |

全て Development、Buffering 2、GPU 未完了上限 2、SwapChain 2 枚、通常 UI、FPS 無制限、VSync OFF、Tearing ON。各実行で Warmup 2 秒を設けた。前面での測定中に Build や別の解析は実行していない。

### 遅延 Marker

測定中は事前確保した Thread 別配列へ全区間を保存し、Host / Worker 停止後に ETW へ全件を出力する方法を追加した。ETW Header の出力時刻・Thread を元の区間の識別には使わない。

- `OriginalThreadId`、Thread 連番、Frame、開始・終了時刻、追加値、戻り値を保持
- Exporter は新 Marker の OriginalThreadId を使用し、旧 Marker は従来の Header Thread ID に対応
- ETW と CSV の照合は時刻だけでなく、全件の名前・Frame・連番・補助値・戻り値も検査
- 20 秒実行は測定前に 1 Thread あたり 1,048,576 区間を確保。測定中の再確保や間引きをしない
- 表示関連だけに絞る ETW と GPU HardwareScheduling を採る ETW を分けた。必要な Event がない記録から GPU Queue の内訳を推定しない
- Probe の Window 状態検査にも負荷がある。通常 Engine の性能を保証する値にはしない

## 前面を維持した計測結果

Render の平均 / p95 / Max は全ての完了区間から計算。p95 は nearest-rank。Console FPS は測定期間全体の値であり、Render 平均の逆数ではない。

| 記録 | latency | FPS | Render 平均 | p95 | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| ETW / 5 秒 | 1 | 655.657 | 1.5167 ms | 4.5067 ms | 6.7278 ms |
| ETW / 5 秒 | 2 | 1,098.442 | 0.9012 ms | 3.7980 ms | 6.0198 ms |
| ETW なし / 5 秒 | 1 | 618.939 | 1.6077 ms | 4.8585 ms | 8.0882 ms |
| ETW なし / 5 秒 | 2 | 972.728 | 1.0168 ms | 3.1267 ms | 5.4836 ms |
| ETW / 20 秒 | 2 | 864.888 | 1.1471 ms | 4.7057 ms | 10.7303 ms |

今回の短い比較では latency 2 の FPS が高いが、前報の異なる実行では逆の例もある。Foreground 未確認だった古い記録と混ぜて改善率を出したり、latency 2 が常に有利と結論したりしない。入力イベントを連続して与える比較は行っておらず、入力遅延の改善も主張しない。

## 20 秒記録の最長 Frame

EtwLong / Frame 5265 / Render TID 14624:

| 区間 | 経過時間 | Running | Waiting | Ready |
| --- | ---: | ---: | ---: | ---: |
| Frame.Render | 10.7303 ms | 0.6704 ms | 10.0524 ms | 0.0075 ms |
| DXGI.Event | 10.0808 ms | 0.0209 ms | 10.0524 ms | 0.0075 ms |

Unknown はいずれも 0 ms。DXGI Event の戻り値は `WAIT_OBJECT_0`。1 ms を指定した Wait の経過時間が 1 ms 以内になっているという意味ではない。

同じ区間を xperf で復号した時系列。時刻は Merged ETL の相対 µs で、µs 単位の丸めを含む。

| Event | 時刻 |
| --- | ---: |
| DXGI Event 開始 | 26,685,804.0 |
| Render Switch Out / Waiting / UserRequest | 26,685,809 |
| dwm.exe TID 11008 → Render ReadyThread | 26,695,861 |
| Render Switch In | 26,695,869 |
| DXGI Event 復帰 | 26,695,884.8 |

この例は CPU が実行可能になってから数 ms 放置されたものではない。DWM から Ready になってから Switch In までは約 8 µs だった。ただし、表示関連に絞ったこの記録では対応する QueuePacket の完了を取得していないため、待機全体のうち何 ms が GPU / DWM のどちらに属するかまでは断定しない。

前報の約 50 ms は CPU 区間 CSV しかなく、後から CSwitch / DWM Event を復元することはできない。今回再現しなかったため、その Frame の根本原因は未確定のまま残す。

## GPU 内部の分離を試した結果

EtwA の modern DxgKrnl に `0x4408841` を指定し、HardwareSchedulingLog bit `0x04000000` を加えた。**SchedulingLog Event 85 件**を取得した。大きい Binary Field を含むため、通常の CSV Field 上限では読み切れない箇所もあり、元 ETL を保持している。

次の経路で既存データの展開を試した。

1. Microsoft 公式 TraceProcessor `Microsoft.Windows.EventTracing.Processing.All` 1.12.10 / `UseDirectXData()`
2. SDK 同梱 WPA Profile の GPU Utilization (FM) を WPAExporter で出力
3. 新しい GPU Work Table の自動出力 Profile を構成

1 と 2 は対象 Process の Packet を各 5 件返したが、全て起動・Warmup の時刻だった。測定開始は latency 1 が約 3.418 秒、latency 2 が約 9.519 秒に対し、得られた Packet はそれぞれ約 1.449～1.492 秒、約 7.556～7.587 秒。この出力を測定中の描画や約 50 ms の原因説明へ転用していない。

TraceProcessor の `IsPartial` は Raw Clock 値を持たず ns 単位の時刻であることも示し、Event 欠落とは異なる。出力は時刻 Field の有無とこの Flag を区別して保持した。[Microsoft の TraceProcessor](https://learn.microsoft.com/en-us/windows/apps/trace-processing/tutorial)

3 は Table 登録まで確認したが、有効な列設定による区間出力を作れていない。この失敗は GPU Work が存在しないことを示さない。**生 Hardware Scheduling Log は取得済みだが、今回の自動解析経路では対象 Frame の GPU Queue 待ちと GPU 実行へ分離できていない。**

これ以上を確定するには GPU Work / GPUView の HWS 表示を正しく展開するか、Engine の既存 GPU Timestamp を Frame ID・CPU Clock 校正とともに全件記録する経路が必要。Renderer のその変更は今回実施していない。

## 品質確認と変更範囲

- 4 回の ETW 全てで Event / Buffer 欠落 0、対象 Event の Decode Error 0
- ETW の区間 1,827,096 件が元 CSV と全件一致。別に Clock Marker 14 件
- ETW なし比較を含め、全 2,173,174 区間で Probe dropped 0
- 20 秒記録の Clock 読取り幅からの不確かさは ±100 ns。その他は ±50 ns。計測全体の精度保証ではない
- `analyze_spike_etw.py` の境界探索後の `islice` が先頭から再走査していたため、添字から直接再開するよう変更。積算条件や標本を変更せず、既存の分類 Test 3 件が成功
- 指定の `scripts/codex_build.ps1` による最終 Debug Build 成功。関連 Debug Test 2 件成功: TimingTrace / InputLatency
- Development の Probe / Exporter Build と実 Window の上記 9 実行が成功
- 通常設定は `CUE_ENABLE_TIMING_TRACE=OFF` へ復帰

変更は手動 Probe、ETL Exporter、調査 Script と文書に限定する。Engine の Priority / Affinity / Buffering / GPU 同期 / Waitable の方針、統計の 120 標本と更新頻度は変更していない。既存の作業差分を保全し、Commit / Push / Merge はしていない。

成果物は `out/validation/Performance20261019/Waitable144/`。各 ETL / CSV / Log、`Summary.json`、`EtwLong/Frame5265.txt`、GPU の各出力と失敗を含む Log、解析用の小さな Program を保持した。外部解析 Package はこの調査用 out 配下に置き、Engine の依存には追加していない。
