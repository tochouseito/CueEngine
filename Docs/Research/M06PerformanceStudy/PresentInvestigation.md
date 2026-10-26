# Present の追加調査 — Issue #101

続き: [2026-10-09のSwapChain設定・表示経路の追加比較](SwapChainArticleExperiments.md)。Driverが更新されているため、今回と次のPhaseの値を直接比較しない

## 結果

2026-10-08、`m06/issue101/render-performance-study` で調査した。Phase 1 / 2 は管理者昇格なしで計測し、その後ユーザーの UAC 許可を受けて PIX Timing Capture を取得した。

- 通常構成の Native Present 1800回中1695回、**94.2%で Context Switch がなかった**。その区間だけでも平均 **0.2361ms** かかる。
- Context Switch があった105回は平均 **0.3923ms**。長い呼び出しには OS の待機・スケジューリングが関係するが、平均費用の全体を説明するものではない。
- Main のみでの実行、最小 UI、UI 開始の Gate では Present の安定した大幅短縮を確認できなかった。
- 描画 Command の GPU 完了を先に待つ条件でも Present は短縮されなかった。Native Present は **0.2602 → 0.3113ms**、Render 全体は **0.6680 → 2.0446ms**。この待機は改善策として採用しない。
- CueEngine 側で Present を包む通常の処理は平均約 **0.0007ms**。大半は `IDXGISwapChain::Present` の呼び出し区間にある。

今回の環境・描画条件で、Present を 0.1ms 以下にする具体的な改善策は見つかっていない。PIX の Call Stack では DXGI 内の Command 提出と Driver 処理を確認した。ただし、Driver の非公開関数、Busy Wait、非実行時間の正確な内訳は確定できていない。

## 測定条件

| 項目 | 条件 |
| --- | --- |
| CPU / GPU | Ryzen 7 3700X / GeForce RTX 3060 |
| OS / Driver | Windows 11 10.0.26200 / 32.0.16.1047 |
| Build | MSVC x64 Development `/O2`、D3D12 Debug Layer / GBV 無効 |
| Window | 1280×720、通常 Window、Primary Monitor 1920×1080 / 75Hz |
| Frame / SwapChain | 60 FPS、Frame 2枠、SwapChain 2枚、VSync / Tearing 無効 |
| 描画 | ClearFinalColor と Editor の ImGui、GPU Timestamp / PIX Marker 有効 |
| UI | 通常は詳細な計測 UI、統計表示更新は15 Frameごと |
| 反復 | Warmup 120 + 測定600 Frame、各条件3回、前後に基準条件 |

Phase 1 は18 Process / 10800測定 Frame、Phase 2 は9 Process / 5400測定 Frame。全27 Processが成功し、時間記録の欠落と無効な Dispatch Sample は0件だった。Frame 外の Message Pump も同一時計で記録した。

Phase 間で診断コードを追加したため、**同じ Phase 内の同じ実行 File** で比較する。異なる Phase の値を改善率の比較に使わない。

- [Phase 1 環境・実行 File Hash](PresentDispatchEnvironment.json)
- [Phase 2 環境・実行 File Hash](PresentGpuWaitEnvironment.json)

GUI の遮蔽、電源状態、他 Process の負荷は完全には制御していない。Phase 1 の2回目は複数の条件で時間が増加した。これは結果の変動として扱い、原因は断定しない。

## Phase 1: Dispatch と Main / UI

### Native Present

| 条件 | Sample | avg ms | median ms | p95 ms | Context Switchあり |
| --- | ---: | ---: | ---: | ---: | ---: |
| 基準、追加 Profilingなし | 3600 | 0.243856 | 0.228200 | 0.3735 | 未測定 |
| 通常構成 + Profiling | 1800 | 0.245178 | 0.231350 | 0.3456 | 105 / 1800 |
| Main のみで実行 | 1800 | 0.255034 | 0.227750 | 0.4070 | 156 / 1800 |
| 最小 UI | 1800 | 0.254963 | 0.225550 | 0.4159 | 153 / 1800 |
| UI 開始 Gate | 1800 | 0.239156 | 0.227300 | 0.3436 | 136 / 1800 |

通常の Profilingあり / なしの平均差は約0.0013ms。ただし、基準 Process 自体の平均は0.2310～0.2603msで変動しており、差全体を計測 Hook の負荷と断定しない。

### Thread の切替

通常構成の1695回の切替なし区間は avg 0.236065ms / p95 0.3115ms。105回の切替あり区間は avg 0.392292ms / p95 0.9322msだった。

通常構成の WaitReason Bit は次の通り。1 Sampleで複数のBitが立つため合計は切替回数と一致しない。

| Bit | 名前 | 該当Sample |
| ---: | --- | ---: |
| 27 | WrResource | 94 |
| 30 | WrQuantumEnd | 6 |
| 0 | Executive | 4 |
| 28 | WrPushLock | 2 |
| 32 | WrPreempted | 1 |

BitMapは理由の存在を示す。各理由の待機時間、回数、Lockの所有者、GPU／DWM待ちの内訳は得られない。`WrResource` を CueEngine の Mutex待ちと解釈しない。Cycle平均は QueryThreadCycleTimeで852783、ProfilingのCycleTime差分で856878だった。**Cycleをmsへ換算しない。**

切替がなくても Driver 内部で Busy Wait している可能性がある。また、Interruptによる停止もあり得る。「切替なし」を「純粋な有用CPU処理」や「GPUと無関係」と同一視しない。

### Main / UI との重なり

通常構成の1800回では、近似Timeline上で UI Build と Present の重なりは0回。Message Pump との重なりは6回、合計0.0057msだった。Mainのみの条件では、同じThreadで直列に動くため重なりはない。

UI Gate 条件は Graph の Snapshot 回収後に次の UI を開始する。そのため、**1800回すべてで UI Build と Present が重なる**。それでも Native Present の平均は0.2392msで、通常構成の0.2452msから大きく変わらなかった。UIとの同時実行が Present の約0.24msを作っているという証拠は得られなかった。

Source上も、Presentを呼ぶ前にImGui Context排他・Snapshot Pinを解除する。Graph、CommandPool、Queueの提出LockもPresent中は保持しない。

UI GateはRenderのImGui待機をMainへ移す既存の診断条件であり、Presentの改善策として新規採用したものではない。

## Phase 2: GPU 完了を先に待つ

Graph が提出した最新 Graphics Fence を、ImGui の提出後処理と Snapshot 回収の後で確認する。診断条件だけ、同じ Fence の完了を CPU で待ってから Present を呼ぶ。追加 Signal、Command、ResourceBarrier は発行しない。

| 条件 | Sample | Native Present avg / p95 ms | GPU待機 avg / p95 ms | Render全体 avg / p95 ms |
| --- | ---: | ---: | ---: | ---: |
| 通常の非同期実行 + Profiling | 3600 | 0.260154 / 0.3610 | なし | 0.667985 / 0.9246 |
| GPU完了後にPresent + Profiling | 1800 | 0.311268 / 0.4524 | 1.323274 / 2.6284 | 2.044648 / 3.3196 |

通常条件では全3600回、照会時点で最新 Graphics Fence が未完了だった。GPU待機条件は全1800回、待機後の完了を確認した。

GPU待機条件のNative Present平均は各反復0.3360 / 0.3303 / 0.2675ms。同じ反復の前後基準平均0.2581 / 0.2710 / 0.2513msを、いずれも上回った。待機を含めたRenderも全反復で悪化した。

この結果は、**今回のGraphのGPU完了を先に待つことでPresentを短縮する案を否定する**。GPU待機時間にはGPU実行・Queue進行・Event通知・CPU再実行の遅延が含まれ、1.323msをGPUの描画時間として扱わない。

Fence完了は、このSignal以前の描画Commandの完了を表す。DXGI／DWMの合成、前のPresentの表示完了、SwapChain Bufferの解放までは保証しない。通常条件の未完了判定も照会時点だけであり、その後のPresent内部でGPUを待ったことを証明するものではない。

## Phase 3: PIX の Present Call Stack

ユーザーの UAC 許可後、PIX 2602.25 で Timing Capture を2回取得した。1回目の `present-approved-01.wpix` は起動処理が大半を占め、Warmup 完了前のため定常描画の根拠には使わない。2回目は通常起動した Probe の Warmup 後に Attach した。

条件は Phase 2 と同じ実行 File、Development / 1280×720 / 60FPS / Frame 2枠 / SwapChain 2枚 / VSync・Tearing 無効。追加 Dispatch Profiling と GPU事前待機は無効。PIX は CPU Sampling 8000Hz と GPU Timing を取得した。Probe は1620 Render Frameで正常終了し、記録欠落は0件だった。

| 項目 | 結果 |
| --- | ---: |
| Capture | `present-approved-02.wpix` |
| 有効区間 | 232135700～5329808900 ns、5.0976732秒 |
| 対象 Render Thread の Queue Submit | 302件 |
| Render CPU Sample | 4453件 |
| Stack と対応できた Sample | 4424件、99.35% |
| `DX12SwapChain::present` を含む Sample | 1144件 |
| ETW Lost Event / Buffer | 0 / 0 |

### 確認した経路

代表的な Stack は以下の経路を通る。

```text
DX12SwapChain::present
  → CDXGISwapChain::Present / PresentImplCore
  → FlipPresentToDWM / FlipPresentCore
  → CD3D12Device::PresentExtended
  → CloseAndSubmitCommandList
  → D3D12Core::CCommandQueue<0>::ExecuteCommandLists
  → NVIDIA Driver / Windows Graphics Kernel
```

`CloseAndSubmitCommandList` は775件、D3D12Core の `ExecuteCommandLists` は622件に含まれる。**Present 自身も内部で Command を提出している**。Engine の Graph 提出だけが GPU Command 提出の費用ではない。ただし、この Stack から DXGI の内部 Command を省略できるとは判断しない。

Present Sample の先頭 Frame が属する Module は次の通り。これは Sampling の件数であり、各 Module の Wall Time や0.26msの内訳ではない。

| Module | Sample |
| --- | ---: |
| ntoskrnl.exe | 369 |
| nvwgf2umx.dll | 362 |
| dxgkrnl.sys | 83 |
| ntdll.dll | 64 |
| nvlddmkm.sys | 49 |
| dxgi.dll | 48 |
| dxgmms2.sys | 47 |
| D3D12Core.dll | 47 |
| nvspcap64.dll | 19 |
| その他 | 56 |

NVIDIA のオーバーレイ Module `nvspcap64.dll` は Stack 内に27件、先頭 Frame に19件含まれた。Present への介入は確認できたが、無効化による短縮は未計測。次の比較候補とする。NVIDIA 設定は変更していない。

### 確定できないこと

- 132件には `EtwpApcPoolThunk` / `EtwpTraceStackWalk`、9件には DPC 実行経路が含まれる。計測処理や割り込みも Present 中の Stack に混ざる。Kernel の369件すべてを Present 本来の仕事と解釈しない。
- 8000Hz Sampling と GPU Timing の取得には負荷がある。Capture 中の値を管理者昇格なしの基準値へ直接換算しない。Inclusive 件数は入れ子で重複し、合計しない。
- Render の Switch Out は625件あるが、その時刻に一致する Stack は0件だった。今回の抽出では Present の非実行時間を対応付けられない。`present_inactive_ns: null` は不明であり、待機0ではない。
- Microsoft の公開 PDB と Probe の PDB は解決できた。NVIDIA 非公開関数の Symbol は取得できず、Module / RVA で記録する。近接する Export 名を内部関数名として使わない。
- 完全な DXGI Present / DWM / 表示完了の Timeline は今回の抽出結果にない。GPU Busy Wait と DWM 待機の時間別分類は未完了。

### 保存・再現

`Engine/Tests/Performance/AnalyzePixPresent.py` を追加した。Capture を読み取り専用で開き、対象 Process の Render Thread と Present Stack に絞って集計する。CaptureFacts の有効区間、PixStorage 拡張の利用は [PIX の公式説明](https://devblogs.microsoft.com/pix/querying-the-pix-timing-capture-file-format/)に従う。Symbol 解決は [DbgHelp](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-syminitialize) の独立 Session で実行する。

`StackEvents` の16 byte組は、今回の PIX 版で Thread / 時刻を照合して確認した形式。将来の PIX 版での互換性は保証しない。Device Path の変換も今回のPCの C: に合わせたもの。汎用解析 Tool としては未完成。

- [集計抜粋・Hash](PixPresentSummary.json)
- 詳細 Stack / 実行ログ / Probe CSV: `out/validation/M06-PerformanceStudy/PresentationPaths/PresentTimeline/`
- Capture SHA-256: `A9F7D984F1A2F3CAE6860DBF507849C9E2AF2BF2C705A81ABBDAF95B40CD1C8F`
- Probe SHA-256: `75A562ECBDB5B3F3E1FB2138A99CB071FCC2CBE3E63155DE5444121C2E2A17F5`

## 計測の有効性

Windows の `EnableThreadProfiling` を現在Threadに対して使い、Dispatchだけを要求した。Hardware Counter、外部Thread停止、Kernel ETW Sessionは使わない。

- `ContextSwitchCount` / `CycleTime`: 開始と終了の累積値の差分
- `WaitReasonBitMap`: 開始側Readで直前区間を消費し、終了側ReadのBitを採用
- QueryThreadCycleTime: 同じThreadのCPU Cycleを別に取得
- Native Present時計: Counter読み取りを区間外に置く
- Dispatch時計: Native区間に境界の時計処理などが少量含まれる
- Timeline: 保存時刻から測定時間を引いた近似区間。Counter読取に伴う終了時刻のずれがある
- 固定容量Collector: 実行中は記録のみ。IO・Sort・集計はHost停止とWorker Join後

Phase 1の各Profiling ProcessでEmpty、Sleep、CPU Busyを60回ずつ測った。

| 校正 | Sample | avg経過 ms | Context Switchあり | CPU Cycle平均 |
| --- | ---: | ---: | ---: | ---: |
| Empty | 720 | 0.000026 | 0 | 1000 |
| Sleep | 720 | 13.6090 | 720 | 46743 |
| CPU Busy | 720 | 2.0002 | 605 | 7064375 |

EmptyのHook全体は平均0.001006ms / p95 0.0014ms。Native Presentを包む区間には、これに加えて保存やTimeline記録などの費用が入る。

Sleepは2msを要求したが、Host初期化前のTimer条件では平均13.609msになった。校正は待機／CPU実行の区別に使い、FPS制御や正確な2ms待機の試験としては扱わない。Sleepの全SampleでDelayExecution Bitを確認した。CPU Busyでの切替理由はQuantumEnd／Dispatch Interruptであり、**切替ありのSampleがすべてBlocking待機というわけではない**。

### 参照仕様

- [PERFORMANCE_DATA](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-performance_data)
- [ReadThreadProfilingData](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readthreadprofilingdata)
- [KernelWaitReason](https://learn.microsoft.com/en-us/dotnet/api/microsoft.windows.eventtracing.cpu.kernelwaitreason?view=trace-processor-dotnet-1.0)

## 保存したもの

- `Engine/Tests/Performance/PresentDispatchProbe.h`: Windows専用Counterと校正、Thread内での解除
- `PerformanceStudy.h`: 調査用Hook、Timeline、GPU完了状態の固定Collector
- `DX12SwapChain::present`: Native Presentの外側のCounter Hook
- `WindowsHost::step / render`: Message Pump記録、診断条件だけのGPU完了確認・待機
- `EditorTimingProbe.cpp`: 5種類のPresent診断条件、停止後CSV出力
- `RunEditorTimingStudy.ps1`: 自分が起動したProcessに対する45秒Timeout
- `SummarizePresentDispatch.py`: 同一時計での重なりとCounterの集計

通常Buildでは調査用Hookと記録を無効にする。GPUの事前待機は診断Caseだけで有効にする。

集計は [Phase 1](PresentDispatchSummary.json) と [Phase 2](PresentGpuWaitSummary.json) に保存した。生のFrame記録、Dispatch、Timeline、Fence状態、ログは `out/validation/M06-PerformanceStudy/PresentationPaths/PresentTimeline/` 以下に保存した。

## 検証・未実施・リスク

- 規定の `scripts/codex_build.ps1` による通常Debug Build成功
- Research Development Probe Build成功
- Host / FrameController / MainFrameGraph / ImGui関連のDebug CTest 8件成功
- Thread Counter、GPU待機の短い自動試験成功
- 本計測27 Process成功、16200測定Frame、無効Counter Sample・時間記録欠落なし
- Profiling Handleは所有Threadで解除。解除失敗もProbeの保存検証に含める
- Read-onlyの独立確認でCounter差分、Queue寿命、Fenceの意味を確認

最初の PIX 試行は UAC を許可できずキャンセルした。その後 Phase 3 で Capture と公開 Symbol 付きの Call Stack 取得に成功した。Python の構文確認、実 Capture の解析、解析前後の Capture Hash 一致を確認した。Phase 3 では C++ / Build 設定を変更していないため、Engine の Build / CTest は再実行していない。Driver 非公開関数の時間、DWM／表示完了、Present 非実行時間の対応付けは未検証。

GPU待機に使う既存APIは無期限待機なので、Probe内の期限だけではGPU Hangを止められない。Runnerの45秒Timeoutで起動したProbeだけを停止する。実際のGPU Hangを起こす試験は実施していない。

MSVCのサンドボックス内BuildでC1902が発生したため、通常ユーザー環境で再実行して成功した。Windowsの管理者昇格は使っていない。

Issue #101の調査Branchを維持した。Commit、Push、Merge、外部Issue投稿は行っていない。将来の実採用は新規IssueとdevelopからのBranchで行う。
