# M06-15 / Issue #144 — DWM の CPU Sampling Stack 調査

- 調査日: 2026-10-20
- Branch: `m06/issue144/gpu-waits-and-telemetry`、開始時 HEAD: `6effec9`
- 前回: [PresentHistory と DWM の CPU 描画区間](M06-15-Dwm-Present-History.md)
- 今回も Commit / Push / Merge は行っていない。前回までの未 Commit 変更を保全した。

## 1. 結論

**前面切替後の長い表示準備待ちに重なる DWM の処理を、Intel の Shader Compiler 内まで絞れた。**

新たな 20 秒の記録では最大 Render が 20.7481 ms。その同じ時間窓で DWM メイン Thread の CPU Sample 21 件中、命令位置が `igc64.dll` 内だったものは 15 件。Stack から、DWM の PixelShader 作成経路で 7 件、DrawIndexed 経路で 8 件を確認した。単に DLL が Process に Load されていたという観測ではない。

`igc64.dll` の ETL 内の FileDescription は `Intel Graphics Shader Compiler for Intel(R) Graphics Accelerator`。公開 Symbol で呼出元の DWM / D3D11 関数を解決した。Intel 内部の関数名は未解決のため、Module / RVA を保持した。

この結果は、前回不明だった「Shader Create Event の外の長い Draw」の一部も Compiler 内の処理だったことを示す。**Compiler 内部のどの最適化が重いか、Cache miss か、Driver の不具合かは確定していない。** 過去の約 50 ms や GPU 提出後約 30 ms の全事例を説明したものでもない。

## 2. 計測条件と品質

保存先: `out/validation/Performance20261020/CpuStack144/Run1/`。

- 既存の Development `CueSpikeTimingProbe.exe` を使用。1280 × 720、通常 UI、Buffering 2、GPU Queue 上限 2、SwapChain 2。
- Waitable ON、Queued presents 2、VSync OFF、Tearing ON、FPS 上限なし。
- Warmup 後、4 秒ずつ前面 / 非前面 / 前面 / 非前面 / 前面へ切替。別 Process の小さな静的 Win32 Window を引き続き使用。通常の別アプリとの比較ではない。
- Kernel の既存記録に `PROFILE` と `-stackwalk Profile` を追加。Sampling 間隔や Windows の表示・電力設定は変更していない。
- `xperf -a profile -freq` は ETL 全域で Period 1000 µs を報告。Sampling / Stack 採取の追加負荷があるため、この実行の平均 FPS は通常時の性能比較に使用しない。
- Probe PID 30644、Render TID 7112、DWM PID 872、DWM メイン TID 2232。
- Frame 17,302、CPU Span 759,897。CPU / GPU の記録落ち・上書き 0。ETW Event Lost / Buffer Lost / Decode Error は全て 0。CPU の全 Marker を生 CSV と照合。
- 前面条件と五つの安定区間の検証成功。GPU 提出 17,302 件の対応に成功。GPU 記録には提出が測定開始前にある二 Frame の Pass も残っており、集計対象の対応済み Frame と区別した。

CPU Sample の品質は ETW Lost とは別に扱う。

- TraceProcessor の全 Sample 45,639 件、DWM 全 Thread 2,237 件。DWM の Stack なしは 5 件。
- 全 DWM Sample では DPC が 35 件、ISR が 155 件。下表の選択区間では DPC は全て 0、ISR は Frame 8576 に 1 件。
- TraceProcessor は Sample 間の未説明区間 `Gaps` を 45,655 件報告した。これは API 上の推定未説明時間であり、ETW Event Lost とは同じ数ではない。全件を `SampleGaps.jsonl` に保存した。CPU Sampling が全 CPU 時間を漏れなく説明すると扱わない。
- Sample Weight は推定値。15 / 21 を厳密な CPU 時間比率や 15 ms として扱わない。実際の Running / Waiting / Ready 時間には CSwitch / ReadyThread を使う。
- TraceProcessor の Sample は変換後の相対 ns で、元の Raw QPC を直接持たない。別の xperf Native Dump の 19.4～19.6 秒にある DWM Sample **57 件全て**を、TID / CPU / IP / 時刻で一意照合した。時刻差は -400～+500 ns。ClockSync から得る ETL 起点と TraceProcessor の ReferenceValue の差は QPC 1 tick（100 ns）。精度を超える順序の断定には使わない。

## 3. 同一 Frame の結果

全 Render を時間順で保持し、長い順の六 Frame を選択。時刻窓は Render 開始～終了の半開区間。DWM は TID 2232 のみを選び、Sample を間引いていない。

| Frame | Render ms | DXGI 待機合計 ms | DWM Running ms | Sample 件数 | 命令位置が igc64 | Stack 内に igc64 | Shader Create 重複 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 15479 | 20.7481 | 19.8886 | 19.7923 | 21 | 15 | 18 | 8.0061 |
| 8576 | 18.6497 | 17.7121 | 17.9298 | 18 | 10 | 15 | 5.6306 |
| 12031 | 17.5716 | 16.8446 | 17.5716 | 18 | 8 | 16 | 5.5640 |
| 15487 | 17.1075 | 16.2018 | 16.8172 | 17 | 9 | 14 | 0 |
| 12043 | 16.4135 | 15.8024 | 15.7040 | 16 | 10 | 14 | 0 |
| 8588 | 16.2157 | 15.5881 | 15.9536 | 16 | 10 | 14 | 0 |

「命令位置」は排他的な Module 別件数。「Stack 内」は同じ標本を各 Module について一度数える包含件数であり、他の Module の包含件数と加算しない。選択した六区間に Stack なしはなかった。命令位置の Module が不明な Sample は Frame 8576 に 2 件、8588 に 1 件あり、不明のまま保持した。

六 Frame は約 8 / 12 / 16 秒の切替直後に集まった。同一 Capture の複数の切替で観測したもので、独立した複数 Capture の再現性を検証した数ではない。

### 最大 Frame の順序

同一 ETL の相対時刻で比較した。

| Event | ETL 相対時刻 µs |
| --- | ---: |
| DWM Draw 開始 | 19,549,734 |
| Engine Frame 15479 Render 開始 | 19,551,542.95 |
| DWM Draw 終了 | 19,571,103 |
| DWM TID 2232 が Render TID 7112 を Ready にした | 19,571,432（Native）、19,571,431.8（QPC） |
| 最後の DXGI.Event の待機復帰 | 19,571,445.95 |
| Engine Render 終了 | 19,572,291.05 |

Native ReadyThread の通知元 Process / Thread も確認した。DWM Draw 終了の約 329 µs 後に Ready、そこから約 14 µs 後に待機が戻る。DXGI.Event は二回の待機（11.0556 + 8.8330 ms）であり、一つの連続 API 呼出しとして合算値を示しているわけではない。

同じ Render 窓の DWM は Running 19.7923 ms、Waiting 0.9262 ms、Ready 0.0296 ms、Unknown 0。長い時間の主な状態は CPU 実行であり、DWM が全時間 Ready のまま CPU を得られなかった事例ではない。

## 4. 解決できた Stack

以下は呼出元 → 命令位置へ向かう抜粋。間の層は省略しており、全 Stack / IP / RVA は JSONL に保存している。

### PixelShader 作成経路

Frame 15479、ETL 相対 19,554,949 µs の標本:

```text
dwmcore!CConnection::MainCompositionThreadLoop
  → dwmcore!CComposition::ProcessComposition
  → dwmcore!CDrawListEntryBatch::Render
  → dwmcore!CBrushRenderingEffect::SetStateOnDevice
  → dwmcore!CRenderingTechnique::GetShaders
  → dwmcore!CD3DPixelShader::Create
  → dwmcore!CD3DDevice::CreatePixelShader
  → d3d11!CDevice::CreatePixelShader
  → d3d11!CDevice::CreatePixelShader_Worker
  → igd10um64xe.dll
  → igc64.dll（命令位置 RVA 0x7B6762）
```

この Frame で命令位置が igc64 の 7 件が作成経路。Native Shader Create に重なる igc64 の標本も 7 件。

### DrawIndexed 経路

同じ Frame、ETL 相対 19,556,952.5 µs の標本:

```text
dwmcore!CConnection::MainCompositionThreadLoop
  → dwmcore!CComposition::ProcessComposition
  → dwmcore!CDrawListEntryBatch::Render
  → d3d11!CContext::TID3D11DeviceContext_DrawIndexed_<1>
  → igd10um64xe.dll
  → igc64.dll（命令位置 RVA 0xDC5EFF）
```

残る 8 件は DrawIndexed 経路で、Native Shader Create の外側だった。他の五 Frame でも命令位置が igc64 の標本は CreatePixelShader / DrawIndexed に分類できた。後半三 Frame は Create Event が重ならず、igc64 の 9 / 10 / 10 件は全て DrawIndexed 経由だった。

従って Native の Shader Create / Link の時間だけを Compile 全体の時間と見なすことはできない。Draw 時の内部処理にも Compiler が呼ばれていることは確認できたが、未解決の Intel 関数を特定の再 Compile / Cache miss と決めつけない。

### Image の出自

ETL の Image Metadata より:

- `igc64.dll`: Intel Graphics Shader Compiler、Intel Corporation、`30.0.101.2079`。
- `igd10um64xe.dll`: User Mode Driver for Intel(R) Graphics Technology、Intel Corporation、`30.0.101.2079`。
- いずれも同じ `DriverStore/FileRepository/iigd_dch.inf_amd64_357acc06f2c40efb/` 配下。
- `dwmcore.dll`: Microsoft、`10.0.26100.9711`。

DWM には NVIDIA の DLL も存在した。Load されている Module の一覧だけで担当 GPU を判定せず、今回示した Stack の Intel 経路と区別する。上記 Version の良否や更新効果は比較していない。

## 5. 変更と検証

- `scripts/capture_spike_etw.ps1` に `-CpuSampling` を追加。現在は検証した `-FocusCycle` との組合せだけを許可。`-Stacks` の CSwitch / ReadyThread と Profile Stack の指定を結合する。
- Engine の C++、PSO、Shader、FrameController、待機方針は今回変更していない。
- 解析用 C# / Python と出力は今回の `out/validation/.../CpuStack144/` に保存。Microsoft TraceProcessor `1.12.10` をオフライン解析だけに使用し、Engine の依存 Package は追加していない。
- 公開 Symbol は採取後に Microsoft Symbol Server から取得した。Loader の進捗対象は 2,795 PDB、実際の Load は 204。選択 Module だけを取得する絞り込みは達成できていない。採取中の通信・計測負荷と混同しない。Intel 関数の私有 Symbol は得られていない。
- Native Start / Stop の未対は開始・終了各 2 件を保持。完全な区間の重複から選択 Frame を解析し、未対を 0 ms と補完していない。
- PowerShell 構文確認成功。既存 Python 解析 Test 12 件成功。一回目は Sandbox の一時 Directory 操作拒否で 1 件失敗し、Repository 内の一時領域を使って再実行した。
- `pwsh -NoProfile -File scripts/codex_build.ps1`: 既定 Debug Build 成功。`CUE_ENABLE_TIMING_TRACE:BOOL=OFF` を維持。今回の性能測定は Build 開始前に完了している。

## 6. 再現と成果物

Trace ON で作った Development Probe を使用する。Build 手順と通常設定への復帰は[統合記録](M06-15-Work-Summary.md)を参照。

```powershell
pwsh -NoProfile -File scripts/capture_spike_etw.ps1 `
    -OutputDirectory C:/Users/sinse/source/repos/CueEngine/out/validation/Performance20261020/CpuStack144/RunNew `
    -Seconds 20 -Cases latency2 -FocusCycle -HardwareScheduling -CpuSampling
```

解析では `UseCpuSamplingData()` と `UseSymbols()`、公開 Symbol を使用。Sample Weight は推定として扱う。[Microsoft の Symbol 解析手順](https://learn.microsoft.com/en-us/windows/apps/trace-processing/symbols)、[Stackwalk の公式説明](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/stackwalk)に基づく。

主要成果物:

- `Run1/Merged.etl`、`Events.csv`、`on-q2-*`: 生記録、CPU / GPU / 前面条件の照合。
- `Run1/DwmSamples-Symbols.jsonl`、同 `.images.json`: DWM 全 Sample と全 Stack、Module の出自。
- `Run1/StackComparison.json`: 選択六 Frame の時刻、Scheduling、排他 / 包含件数、代表 Stack、Native 区間。
- `Run1/PeakRaw.txt`、`Clock.txt`、`ProfileFrequency.txt`、`Display.txt`: 独立した Native Sample / ReadyThread / Clock / DWM Event の Dump。
- `Run1/SampleGaps.jsonl`、`SampleGaps.log`: 推定未説明区間。
- `TraceProcessor/Program.cs`、`CpuStacks.csproj`、`AnalyzeStacks.py`: 今回の解析 Program。
- `DebugBuild.log`、`AnalysisTests-Retry.log`: 検証結果。

`out/` は Git 管理外で、Clone / Push に生 ETL、Symbol Cache、解析用 Program は含まれない。本書と採取 Script の変更は作業 Tree に残した。

## 7. 残る確認

1. **通常の別アプリへの切替でも同じ DWM → Intel Compiler 経路が出るか**を比較する。補助 Window 固有の描画・切替条件を分ける。
2. Intel 内部の処理理由、Shader の種類、Cache の状態は未確定。Engine の PSO Cache が DWM 内の Compiler 処理を直接短縮するとは判断しない。
3. 切替直後の事例と、安定状態の数 ms の待ち、以前の提出後約 30 ms の待ちは分けて追う。

今回の結果を根拠に Worker OFF や Waitable OFF を恒久変更する実装は行っていない。
