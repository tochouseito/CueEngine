# M06-15 / Issue #144 — GPU Context 優先度更新と Signal 付近の待機

- 調査日: 2026-10-20
- Branch: `m06/issue144/gpu-waits-and-telemetry`、開始時 HEAD: `6effec9`
- 前回: [通常アプリへの切替](M06-15-Normal-App-Focus.md)
- Commit / Push / Merge は行っていない。既存の未 Commit 変更を保全した。

## 1. 今回の結論

**切替直後の約 7～8 ms の CPU Resource 待機には、GPU Context の優先度更新と NVIDIA Kernel Driver の処理が関係する可能性が高まった。**

前回の ETL を追加解析し、Render を起こした System Thread 自身に次の Stack があることを、四回の切替全てで確認した。

```text
ntoskrnl!ExpWorkerThread
  → dxgkrnl!DxgkpProcessStatusChangeWork
  → dxgkrnl!DXGPROCESS::ProcessStatusChange
  → dxgkrnl!DXGDEVICE::NotifyProcessStatusChange
  → dxgmms2!VidSchSyncPriorityDevice
  → dxgkrnl!DXGDEVICE::UpdatePriorityClass
  → dxgkrnl!DXGCONTEXT::SetSchedulingPriority
  → dxgmms2!VidSchSetPriorityHwContext
  → dxgmms2!VidSchiUpdateDdiHwContextPriority
  → dxgkrnl!ADAPTER_RENDER::DdiSetContextSchedulingProperties
  → nvlddmkm.sys
```

同じ時間窓には Native の `SetProcessStatus`、`ChangeHwContextPriority`、`ChangePriority` もあった。単に NVIDIA Module が Load されていたという観測ではない。

[Microsoft の DDI 説明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setcontextschedulingproperties)にも、アプリが Focus を得る・失う際に OS が GPU の Scheduling Properties を更新することが記されている。今回の Stack はこの経路と整合する。

**ただし、待っていた Lock の同一性と、Engine 側が Signal API の内部にいたことはまだ確定していない。** 前回の ETL にない API 境界や CSwitch Stack を、Source の順序や隣の標本から補ったことにはしない。Driver の不具合、特定の優先度設定、Cache miss の確定でもない。

## 2. 使用した記録と比較の制限

新しい前面切替や GUI 起動は行っていない。ユーザーが前面操作できない条件を維持し、前回の `AppFocus144/Notepad1/Merged.etl` をオフラインで解析した。

この記録は前回記載の **960 × 720 の試行**であり、旧 1280 固定の Window Size 判定により `quality_errors=[window_shape]` になったもの。前面 PID と全 Marker の照合は成功したが、正式な描画 Size の比較条件は不成立である。今回の追加 Stack 解析によって、この比較の品質判定を成功へ変更していない。

同一試行の四回の切替であり、独立した四 Capture ではない。一般的な Alt+Tab、全 Driver、全描画 Size に同じ待機が起きると一般化しない。

## 3. 四回の照合結果

Render TID は 6728。各 Frame の `Waiting / WrResource` 開始から次の Running までを Native CSwitch で区切り、その間の ReadyThread の通知元を一意に確認した。

| Frame / 切替秒 | Render ms | Resource 待機～再実行 ms | 通知元 System TID | System 標本数 | 優先度更新の標本数 | うち通知元 Thread の標本数 | ChangeHwContextPriority 件数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 7416 / 4 | 9.2375 | 8.169 | 13940 | 11 | 5 | 4 | 21 |
| 12322 / 8 | 9.0546 | 7.867 | 13940 | 10 | 2 | 2 | 21 |
| 17299 / 12 | 9.4129 | 8.178 | 27452 | 9 | 5 | 2 | 21 |
| 22242 / 16 | 8.0574 | 7.126 | 27452 | 9 | 1 | 1 | 21 |

優先度更新の件数は DPC / ISR と判定された標本を除外した。System 標本数の列には割込みの標本も含む。通知元 Thread に観測した標本数は処理時間の ms ではない。

- 四区間の System Sample 計 **39 件全て**を、別形式の Native Sample と TID / CPU / IP / 時刻で一意照合した。許容差は Native Dump の丸めを考慮した 1 µs 以内。
- Stack なしは 0。ただし **22 件は 1 Frame だけの Stack**であり、全ての呼出元を復元できたとは扱わない。
- System 標本には DPC 0、ISR 2 件。Frame 12322 の優先度更新を含む標本 1 件は ISR と判定されているため、表の優先度更新の計数から除外した。
- 各 Render 区間に 21 件ずつの `ChangeHwContextPriority` があり、それぞれの区間では 21 個の異なる Context 値だった。表の Event 件数は API の総所要時間や 21 ms を意味しない。
- `SetProcessStatus` の件数は Frame 7416 / 17299 が各 3、12322 / 22242 が各 2。`ChangePriority` は各 4 件。
- Header と列数で Native Schema を照合し、曖昧な Header を拒否した。PriorityBand の数値だけから、どのアプリの Context かを推定していない。

### 最大 Frame の追加根拠

ETL 相対時刻の順序:

| 時刻 µs | 観測 |
| ---: | --- |
| 15,495,495～15,495,626 | SetProcessStatus |
| 15,495,620～15,503,977 | ChangeHwContextPriority が連続 |
| 15,496,119 | Render が WrResource 待機へ入る |
| 15,497,921.1 / 15,499,425.0 | System TID 27452 に上記の優先度更新 → NVIDIA の Stack |
| 15,499,342.6 | System TID 27744 に同経路と `ntoskrnl!ExReleaseResourceLite` |
| 15,504,292 | System TID 27452 が Render を Ready にした |
| 15,504,297 | Render が再び Running |
| 15,504,304 | Render から Fence 17300 の Signal Native Event |

`ExReleaseResourceLite` は通知元と異なる System Thread の標本にある。これを Render が待った同じ Lock の解放と断定しない。通知元 TID 27452 の後半には NVIDIA 内の 1 Frame だけの標本もあり、その内部関数名や全期間の活動は未解決。

この待機より後で DWM → Intel Compiler の長い処理が別にあったことは、前回の結果と両立する。**Intel の Shader Compiler と NVIDIA の優先度更新を一つの処理としてまとめない。**

## 4. 次回用の計測を追加

これまでの `ExecuteCommandLists` 用 QPC と GPU Record は変更せず、次の CPU Scope を追加した。

| Scope | 測定する区間 |
| --- | --- |
| `CommandPool.Submit` | Pool の提出処理全体 |
| `CommandPool.SubmitLock` | Pool Mutex の取得まで |
| `Queue.Submit` | Queue の提出処理全体 |
| `Queue.SubmitLock` | Queue Mutex の取得まで |
| `Queue.Signal` | `ID3D12CommandQueue::Signal` の直前～復帰直後 |
| `Graph.MarkSubmitted` | 提出後の完了点・Resource 利用・統計の登録 |

`Queue.Signal` の detail に Fence 値、outcome に HRESULT の 32 bit 値を残す。API 呼出し後の登録を Signal に含めない。親子 Scope は包含区間なので加算しない。Mutex の保持範囲・順序、API 呼出し順、失敗時の動作は変更していない。Trace ON 時の Lock 取得後には計測終了処理の負荷が加わる。

`analyze_spike_etw.py` でもこれらの Scope を全 Marker 照合後の CPU 状態解析対象へ追加した。記録と統計の間引きは行わない。Trace ON 時は標本数と採取負荷が増えるため、次回も保持範囲・記録落ちを検証する。通常の Trace OFF Build では既存の無効化機構に従う。

今回追加した Scope を含む長い Signal 待機は、まだ再計測していない。新しい計測結果で旧 ETL の 8.178 ms を Signal 内の時間に書き換えてはいない。

## 5. 検証と保存先

ローカル成果物: `out/validation/Performance20261020/Signal144/`。

- `SystemAndRenderSamples.jsonl` / 同 `.images.json`: System 全 Thread と Render Thread の Sample / Stack / Module 出自。
- `PriorityComparison.json` / `.log`: 四回の Resource 待機、通知元、Priority Event、完全な Stack と ISR 判定。
- `Frame7416.txt` / `Frame12322.txt` / `Frame22242.txt`: 追加した Native Dump。最大 Frame は元の `AppFocus144/Notepad1/PeakRaw.txt`。
- `AnalyzePriority.py` / `TraceProcessor/`: 再解析用 Program。
- `DevelopmentBuild.log` / `DevelopmentTests.log` / `AnalysisTests.log` / `DebugBuild.log`: 検証。

追加 Sample の処理は Microsoft TraceProcessor `1.12.10` と前回のローカル Symbol Cache を使用した。新しい Symbol のネット取得は行っていない。

NVIDIA Kernel Module の ETL Metadata は `nvlddmkm.sys`、NVIDIA Corporation、Version `32.0.16.1714`。Microsoft の呼出元は公開 Symbol で解決し、NVIDIA 内部は Module / RVA のまま保持した。Engine 側の PDB を差し替えて旧 Capture に無理に当てていない。

検証:

- Trace ON の Development で Probe / CommandPoolTests / TimingTraceTests を Build。
- 前面操作の不要な既存 `Cue.Renderer.DX12.CommandPool` と `Cue.Foundation.TimingTrace` を実行し、2 件成功。
- Python 解析 Test 14 件成功。
- `CUE_ENABLE_TIMING_TRACE=OFF` に戻して、`pwsh -NoProfile -File scripts/codex_build.ps1` による既定 Debug Build 成功。
- 新しい GUI 計測、修正版での正式な描画 Size 比較、待機 Stack の再採取は未実施。

生 ETL と個別解析用 Program は Git 管理外。本書と Renderer 内の計測・解析 Script の変更は現在の作業 Tree に保持した。性能改善や不具合解消を実施済みという報告ではない。

## 6. 次に確定すること

前面操作が可能になったら、修正版の同じ描画 Size で `-Stacks -CpuSampling` を組み合わせる。新しい Scope と同じ Frame の CSwitch / ReadyThread Stack を照合し、Signal、Engine Mutex、提出後登録のどこに待機があるかを確定する。その上で Kernel の取得・解放 Stack と GPU 優先度更新を結び、待機対象を調べる。

現時点では Engine から優先度を固定・引き上げる実装や、Waitable OFF、Driver / Windows 設定変更は行っていない。
