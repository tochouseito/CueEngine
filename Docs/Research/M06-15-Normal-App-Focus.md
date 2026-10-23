# M06-15 / Issue #144 — 通常アプリへの切替と DWM Compiler の追試

- 調査日: 2026-10-20
- Branch: `m06/issue144/gpu-waits-and-telemetry`、開始時 HEAD: `6effec9`
- 前回: [DWM の CPU Sampling Stack 調査](M06-15-Dwm-Cpu-Stacks.md)
- 今回も Commit / Push / Merge は行っていない。既存の未 Commit 変更を保全した。

## 1. 今回分かったことと比較の成立範囲

**メモ帳への切替でも、DWM → D3D11 → Intel Shader Compiler の経路を観測した。** 小さな自作補助 Window だけに存在する呼出しではなかった。

一方、今回は DWM の長い Draw と Engine の長い待機が一致しなかった。DWM の Draw が約 33～39 ms かかっている間も Engine は複数 Frame を提出し、その区間と重なる Render の最大は約 2.6～2.9 ms だった。Compiler を実行しているという事実だけでは、Engine が同じ時間だけ待つとは判断できない。

切替直後には別に約 8～9 ms の Render スパイクがあった。最大例の大部分は `Graph.Execute` 内、`ExecuteCommandLists` の復帰後にある CPU の Resource 待機だった。前回の DWM メイン Thread の長い CPU 実行と同一の原因としてまとめない。

**ただし、今回得られたのは試行記録であり、条件をそろえた正式な速度比較は未完了。**

- 前回は 1280 × 720。今回の試行は画面上でメモ帳と重ならないよう 960 × 720 にした。
- 最初の記録では Window の品質判定に 1280 固定の比較が残っていた。採取後に修正したが、修正前の記録に変更を遡及して適用していない。
- 修正版で採り直そうとしたところ Windows が初期の前面取得を拒否し、測定開始前に終了した。
- ユーザーから「ウィンドウが見えない」「今は操作できない」と回答があったため、追加の起動・クリック依頼を停止した。960 × 720 の補助 Window 条件との対照実行もまだない。

従って「メモ帳の方が速い」「解像度変更で直った」「補助 Window が原因だった」とは結論しない。取得済みの正しい時刻・Stack・前面 PID から確認できる現象を以下に残す。

## 2. 試行条件と記録品質

保存先: `out/validation/Performance20261020/AppFocus144/`。

| 項目 | 条件 |
| --- | --- |
| 試行 | `Notepad1`、Warmup 後 20 秒、4 秒ずつ F/B/F/B/F |
| Engine | Development、960 × 720、通常 UI、FPS 上限なし |
| 表示 | Buffering 2、GPU 上限 2、SwapChain 2、Waitable ON、Queued presents 2、VSync OFF、Tearing ON |
| Engine 外形 | Desktop 座標 `(0, 0)–(976, 759)` |
| メモ帳外形 | `(1000, 0)–(1500, 320)`、採取前後で一致 |
| メモ帳 | Microsoft.WindowsNotepad Package の Path 上の Version `11.2607.14.0` |
| Process | Probe PID 28784、Render TID 6728、メモ帳 PID 31508、DWM PID 872 / メイン TID 2232 |

調査用の既存 File を開いた専用メモ帳を起動した。メモ帳が既にある場合は起動を中止する手順とし、既存の編集内容や設定は使用していない。試行後も PID・題名が一致する専用 Window だけに Close を要求した。Process の強制終了は行っていない。

前面切替の仲介は従来の別 Process を使い、外部アプリを対象にする場合は仲介 Window を表示しない。借用するメモ帳 Window は Probe から閉じない。指定 PID / Window が変わった場合や無関係なアプリが前面になった場合は切替を拒否する。

実際のキー入力による Alt+Tab の再現ではない。通常の `SetForegroundWindow` と限定した `AllowSetForegroundWindow` を使い、OS の拒否を強制的に解除しない。[Microsoft の前面取得条件](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow)に従う。

### 成立した検証

- 五位相全ての要求成功、安定区間の実測前面 PID 一致。切替中の一時的な前面なしも保存した。
- Frame 24,950、CPU Span 1,094,683。CPU / GPU の記録落ち・上書き 0。
- ETW Event Lost / Buffer Lost / Decode Error 全て 0。CPU Marker 全件を生 CSV と照合。
- GPU 提出 24,950 件を対応。GPU 記録に含まれる測定開始前の二 Frame の Pass は、対応済みの提出と別に保持した。
- 記録された Present 区間 24,948 件を PresentMon と一意照合。全て `Hardware Composed: Independent Flip`、照合失敗 0。Mode 名が同じことだけで表示経路の全条件まで同一とはしない。
- CPU Sample 全体 55,200 件、DWM 全 Thread 2,670 件、Stack なし 8 件。Sampling の未説明区間 `Gaps` は別に保存した。Sample 件数は正確な関数別 ms ではない。
- 最大スパイク付近の DWM Native Sample 63 件を TID / CPU / IP / 相対時刻で全件一意照合。時刻差 -400～+500 ns、Clock 起点差 300 ns。
- CPU Clock の開始 Bracket 100 ns、終了 Bracket 21 µs、Offset Drift 約 8.35 µs。数 ms の差と区別し、細かい順序の精度を誇張しない。

### 成立していない検証

`Probe.WindowState` の Size Bit は修正前の 1280 固定判定で不成立だった。採取時の要求値と配置は分かるが、記録全体で 960 × 720 が維持されたと品質 Gate からは保証できない。

解析側にも可視・非最小化・要求 Size の検査を追加した。追加前の解析結果は `succeeded=True` だったが、追加後は同じ試行を **`succeeded=False / quality_errors=[window_shape]`** と判定する。前面 PID と Marker の検証成功を、全比較条件の成功と取り違えない。

`Notepad2` は初期前面取得の期限切れ、Probe Exit 11。計測区間を作っておらず比較対象から除外した。Collector の停止・Merge は成功している。

## 3. DWM Compiler 経路は通常アプリでも出た

各切替の後 200 ms を全件解析した。最長 Render の時間窓だけでは、その後に実行された Compiler を見落とすためである。

| 切替秒 | igc64 に命令位置がある標本 | CreatePixelShader 経由 | DrawIndexed 経由 | 重なる DWM Draw ms | その Draw と重なる Render の最大 ms |
| --- | ---: | ---: | ---: | --- | ---: |
| 4 | 25 | 6 | 19 | 33.353 | 2.9421 |
| 8 | 25 | 5 | 20 | 38.718 | 2.5924 |
| 12 | 16 | 5 | 11 | 35.322 | 2.8556 |
| 16 | 15 | 5 | 10 | 17.699 と 16.734 | 2.6810 |

Module は前回と同じ `igc64.dll` / `igd10um64xe.dll`、ETL Metadata の Version `30.0.101.2079`。公開 Symbol を前回のローカル Cache から解決した。Intel の私有関数は RVA のまま保存している。

Stack の呼出元 → 命令位置の抜粋:

```text
dwmcore!CComposition::ProcessComposition
  → dwmcore!CDrawListEntryBatch::Render
  → d3d11!CContext::TID3D11DeviceContext_DrawIndexed_<1>
  → igd10um64xe.dll
  → igc64.dll

dwmcore!CRenderingTechnique::GetShaders
  → dwmcore!CD3DPixelShader::Create
  → dwmcore!CD3DDevice::CreatePixelShader
  → d3d11!CDevice::CreatePixelShader_Worker
  → igd10um64xe.dll
  → igc64.dll
```

Compiler の標本は各切替境界からおおむね 14～59 ms 後にあった。最大 Render スパイクは境界直後から約 8～10 ms で終了しており、時間窓が異なる。長い DWM Draw の間には各区間で約 20～45 件の Engine Render が重なった。

この事例は「DWM が Compiler 内で CPU 処理をする」ことと「Engine が DWM の処理完了まで長く待つ」ことを別に確認すべき根拠になる。Driver の欠陥や Cache miss の確定ではない。

## 4. 境界直後の別の待機

| Frame | Render ms | Graph.Execute ms | DXGI.Event ms | DWM メイン状態 |
| --- | ---: | ---: | ---: | --- |
| 7416 | 9.2375 | 8.3155 | 0.4763 | 全区間 Waiting |
| 12322 | 9.0546 | 8.0485 | 0.4889 | 全区間 Waiting |
| 17299 | 9.4129 | 8.3587 | 0.6246 | 全区間 Waiting |
| 22242 | 8.0574 | 7.2762 | 0.4934 | 全区間 Waiting |

四 Frame の時間窓には DWM メイン Thread の CPU Sample がなく、CSwitch / ReadyThread でも Waiting だった。Shader Create の重なりも 0。前回の約 20～30 ms の DXGI 待機と分けて扱う。

### 最大 Frame 17299 の分解

- Render の CPU 状態: Running 0.6175 ms、Waiting 8.7866 ms、Ready 0.0088 ms、Unknown 0。
- `Graph.Record`: 0.0558 ms。
- `ExecuteCommandLists`: 0.0402 ms。
- GPU Pass 全体: 約 0.0379 ms。最終 GPU Timestamp は測定開始後約 12.001410 秒。近傍 Calibration の感度は約 3.016 µs。
- `ExecuteCommandLists` 復帰から `Graph.Execute` 終了まで約 **8.215 ms** が別に残る。

Native ETW の順序（ETL 相対 µs）:

| Event | 時刻 |
| --- | ---: |
| ExecuteCommandLists 復帰（QPC 照合） | 15,496,113.2 |
| Render が Waiting / WrResource に移った | 15,496,119 |
| 最終 GPU Timestamp（Calibration 照合） | 約 15,496,262.1 |
| System TID 27452 が Render を Ready にした | 15,504,292 |
| Render が Running に戻った | 15,504,297 |
| Render からの SignalSynchronizationObjectFromGpu、Fence 17300 | 15,504,304 |
| Graph.Execute 終了 | 15,504,327.85 |

Resource 待機から Running に戻るまで約 8.178 ms。GPU の最後の Query より後も CPU の待機が続いていた。Engine の `Fence.Event` で描画完了を待った区間と同じではない。

Source 上、Queue Submit は `ExecuteCommandLists` 後に `ID3D12CommandQueue::Signal` を呼び、その後に完了点と Resource 利用を登録する。現在の提出用 Timestamp は ExecuteCommandLists だけを囲むため、Signal の API 全体は含まれない。上記順序から **Signal 付近の Resource 待機が候補**になる。

ただし、この ETL には CSwitch Stack がなく、Signal 自体の API 開始・終了 Scope もない。`WrResource` の名前だけから待っている Lock、Driver、API を断定しない。Signal の約 8 ms と確定したものではない。

## 5. 変更と検証

- `FocusExperiment.h`: 外部比較 Window を借用する経路を追加。外部 Window は閉じず、仲介 Window は非表示。既定の自作補助 Window 経路を維持。
- `SpikeTimingProbe.cpp`: 比較 HWND と、960 / 1280 の描画幅を指定可能にした。960 の場合は Monitor 左上に配置。Window の期待 Size も指定幅に合わせ、期待値を Log に残す。
- `capture_spike_etw.ps1`: `-FocusWindow` / `-FocusWidth` を追加。`FocusCycle` 以外では非既定値を拒否。
- `analyze_focus_cycle.py`: Window の可視・非最小化・期待 Size を全観測変化で検証。前面切替とは独立した品質 Gate とする。
- Window Shape の欠落、Size 不一致、不可視、最小化を検出する Test を追加。
- Runtime / Renderer の C++、PSO、待機方針、Thread 優先度、Windows 設定は変更していない。

検証:

- Development の Probe / ETW Export Build 成功。その後、期待 Size の修正を含む Probe の Development Build 成功。
- Python 解析 Test 14 件成功。PowerShell 構文確認成功。
- 描画幅を含む新しい計測経路の正式採取は未完了。試行の前面切替成功と混同しない。
- `CUE_ENABLE_TIMING_TRACE=OFF` へ戻した上で、`pwsh -NoProfile -File scripts/codex_build.ps1` の既定 Debug Build 成功。最初は Sandbox 内の Compile で PDB Manager の C1902 が出たため、同じ標準 Build を Sandbox 外で再実行した。

## 6. 成果物と次の確認

`out/validation/Performance20261020/AppFocus144/`:

- `Notepad.json` / `NotepadAfter.json`: 専用 Window の PID、Path、配置、終了要求の同一性確認。
- `Notepad1/Merged.etl` / `Events.csv` / `on-q2-*`: 生記録と全 Marker / GPU 対応。
- `Notepad1/FocusBeforeShapeGate.json` / `FocusAfterShapeGate.log`: 追加検証前後の品質判定。
- `Notepad1/DwmSamples-Symbols.jsonl` / `SampleGaps.jsonl`: 全 DWM Sample、未解決 RVA、推定未説明区間。
- `Notepad1/StackComparison.json` / `TransitionStacks.json`: 上位 Frame の同一窓と、切替後全域の両方の結果。
- `Notepad1/RenderNativePeak.json` / `SubmitNativePeak.json` / `PeakRaw.txt`: CPU Resource 待機と通知元、Signal の Native 根拠。
- `Notepad1/PresentMon.csv` / `Present-modes.json`: Present 全件照合。
- `Notepad2/Done.json` / `on-q2.log`: 初期前面取得失敗。比較に不使用。
- `AnalyzeStacks.py` / `AnalyzeTransitions.py` / `TraceProcessor/`: 今回の解析用 Program。
- `DevelopmentBuild*.log` / `DebugBuild.log` / `DebugBuild-Retry.log` / `AnalysisTests.log`: Build / Test。

生 ETL、Symbol Cache、個別解析 Program は Git 管理外。本書と Test / 採取 Script の変更は作業 Tree に保持している。

残る確認は次の二つ。

1. ユーザーが前面化できる時に、修正版で **同じ 960 × 720 の補助 Window / メモ帳条件**を採る。今回の試行だけでは条件差の寄与を分けられない。
2. 今回の Resource 待機に対し、**Queue Signal の API 区間と CSwitch / ReadyThread Stack**を採る。GPU Pass、ExecuteCommandLists、Signal、提出後の登録を別に確認する。

今回、Compiler の処理も CPU Resource 待機も解消したという変更は行っていない。
