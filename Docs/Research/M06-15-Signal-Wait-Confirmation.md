# M06-15 / Issue #144 — Signal 内の待機と GPU Device 排他の確認

- 調査日: 2026-10-21
- Branch: `m06/issue144/gpu-waits-and-telemetry`、開始時 HEAD: `6effec9`
- 前回: [GPU Context 優先度更新と Signal 付近の待機](M06-15-Gpu-Priority-And-Signal.md)
- 今回は前回までに追加した Engine の計測を使って採取・解析した。Engine の C++ は新たに変更していない。Commit / Push / Merge は行っていない。

## 1. 結論

**今回捕捉した切替直後の長い CPU 待機は、GPU Device アクセス排他の取得待ちだった。API 区間と、待った側・解除した側の Stack まで対応できた。**

- 同じ 960 × 720 の補助 Window / メモ帳条件を一回ずつ採取し、各四回、計八回の切替を解析した。
- **七件は Engine が呼ぶ `ID3D12CommandQueue::Signal` の区間内**。Signal の時間は約 4.26～8.92 ms。
- **一件は `Present` 内部の Signal 提出経路**。Present は約 9.02 ms、その中の Resource 待機は約 8.68 ms。
- 八件全てで Render 側は `DXGDEVICEACCESSLOCKEXCLUSIVE::Acquire` → `ExAcquireResourceExclusiveLite` で待っていた。
- 八件全てで通知元は System の `DXGPROCESS::ProcessStatusChange`。同じ排他 Class の Destructor → `ExReleaseResourceLite` から対象 Render を Ready にしていた。

前回は「Signal 付近が候補」だったが、今回は新しい API 計測と Stack により上記まで確認できた。旧 Capture の区間を遡って Signal 内の時間へ書き換えたものではない。

GPU 優先度更新との関係も、今回の通知元 Thread の CPU Sample で確認できた。ただし NVIDIA 内部関数、Lock の実アドレス、更新処理の個別所要時間や Driver の不具合までは確定していない。

## 2. 採取条件と成立確認

成果物 Root: `out/validation/Performance20261021/Signal144/`。

| 項目 | Companion1 | Notepad1 |
| --- | --- | --- |
| 比較先 | 静的な自作補助 Window | 調査専用メモ帳 |
| Probe PID / Render TID | 18940 / 13940 | 26036 / 10512 |
| 計測 | Warmup 後 20 秒、4 秒ずつ F/B/F/B/F | 同左 |
| Client Size | **960 × 720** | **960 × 720** |
| Engine 外形 | `(0, 0)–(976, 759)` | 同左 |
| 描画設定 | 通常 UI、Buffering 2、GPU 上限 2、SwapChain 2 | 同左 |
| 表示設定 | Waitable ON、Queued presents 2、VSync OFF、Tearing ON、FPS 上限なし | 同左 |
| Kernel Stackwalk | CSwitch + ReadyThread + Profile | 同左 |
| Frame 数 | 25,596 | 25,706 |
| CPU Span 数 | 1,276,439 | 1,282,063 |
| GPU Record 数 | 102,409 | 102,848 |
| 前面・Size・品質 Gate | 成功 | 成功 |

今回は両方とも自動で初期の前面を取得できた。ユーザーは操作可能と回答したが、手動クリックを必要とせず計測を完了した。前面取得制限を強制解除する操作や疑似入力は使っていない。

メモ帳は既存 Process がないことを確認して専用 File で起動し、外形を `(1000, 0)–(1500, 320)` に配置した。採取前後で配置が一致した。終了時は PID と題名が一致する専用 Window だけに Close を要求した。仲介 Window は非表示。補助 Window 条件の計測中にはメモ帳をまだ起動していない。

### 記録品質

- 両 Capture で CPU / GPU の記録落ち・上書き 0。
- ETW Event Lost / Buffer Lost / Decode Error 全て 0。
- CPU Span 計 **2,558,502 件全て**を ETW Marker と照合した。
- 前面切替五位相、実測 PID、可視・非最小化・要求 Client Size を検証し、両方とも `succeeded=True / focus_errors=[] / quality_errors=[]`。
- GPU 提出は各 Capture の全 Frame と対応した。記録中の測定開始前の二 Frame の Pass は提出のないものとして各回別に保持しており、欠落 0 と混同しない。
- CPU Clock の開始 Bracket は 100 ns、終了 Bracket は 2.7 µs。Offset Drift は補助条件 100 ns、メモ帳条件 0。
- Render の ThreadActivity は補助条件 30,119 件、メモ帳条件 30,385 件。全件に SwitchIn Stack があり、Ready Stack はそれぞれ 30,057 / 30,328 件。全活動での Ready Stack 欠落は残したまま、今回選んだ八件では両側とも取得できた。
- 八件の待機開始・Ready・再実行、計 **24 イベント**を独自 Native Export と一意照合した。時刻差の最大は補助条件 100 ns、メモ帳条件 0。
- 最大 Signal の Native ReadyThread では、System TID 7444 → Probe TID 10512 を別の xperf Dump でも確認した。
- API の包含判定には 2 µs の許容幅を使い、親子 Scope の時間は加算していない。

Stackwalk 自体の負荷があるため、この Capture の平均 FPS を通常時の性能や改善率に使わない。同じ Size の有効な二条件を得たが、各一回の順次実行なので、条件間の時間差をメモ帳だけの効果と確定しない。

## 3. 切替直後の八件

各切替の前後 250 ms に開始した Render から最大の Frame を選択した。

| 条件 / 切替秒 | Frame | Render ms | Signal ms | Present ms | 主な Resource 待機 ms | 待機を含む API | 通知元 System TID |
| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 補助 / 4 | 7388 | 5.2152 | 4.3460 | 0.4283 | 4.3114 | Signal | 20120 |
| 補助 / 8 | 12412 | 5.4385 | 4.5741 | 0.3218 | 4.5425 | Signal | 20120 |
| 補助 / 12 | 17580 | 5.1200 | 4.2579 | 0.3056 | 4.2232 | Signal | 23772 |
| 補助 / 16 | 22735 | 5.2616 | 4.6666 | 0.2477 | 4.6335 | Signal | 20120 |
| メモ帳 / 4 | 7286 | 9.4396 | 0.0175 | 9.0166 | 8.6833 | Present 内の Signal 提出 | 28776 |
| メモ帳 / 8 | 12482 | 8.6471 | 7.7678 | 0.4337 | 7.7304 | Signal | 21984 |
| メモ帳 / 12 | 17664 | 9.4323 | 8.6692 | 0.3275 | 8.6347 | Signal | 7444 |
| メモ帳 / 16 | 22820 | 10.0162 | 8.9213 | 0.5381 | 8.8628 | Signal | 7444 |

全て `WrResource`。Ready になってから実行再開するまでの時間は 0.0076～0.0109 ms で、主な区間は Waiting だった。

選択 Frame の `Queue.SubmitLock` / `CommandPool.SubmitLock` は各 0.0002～0.0003 ms、`Graph.MarkSubmitted` は 0.0014～0.0146 ms。今回の長い待機は Kernel の Device アクセス排他の経路に対応した。Signal の HRESULT は八件とも 0、Fence detail は各 Frame の提出完了点に対応する値を保持している。

## 4. 待った側と解除した側

以下は呼出元 → 待機点の抜粋。ETW が追加した Stackwalk 関数は省略し、全 Frame / IP / RVA は JSONL に保持した。

### Engine の Signal 区間

```text
Engine の Queue.Signal 計測区間
  → D3D12Core.dll
  → win32u.dll
  → dxgkrnl!DxgkSubmitSignalSyncObjectsToHwQueue
  → dxgkrnl!SubmitSignalSyncObjectsToHwQueue
  → dxgkrnl!DXGDEVICEACCESSLOCKEXCLUSIVE::Acquire
  → ntoskrnl!ExAcquireResourceExclusiveLite
  → ntoskrnl!ExpWaitForResource
  → ntoskrnl!KeWaitForSingleObject
```

再実行時の対象 Thread Stack に、この取得待ちの呼出し経路が残っていた。Stack の Thread ID と時刻を対象 Thread / 再実行イベントに照合した。

### Present 内で待った一件

```text
dxgi!CDXGISwapChain::Present
  → dxgi!CDXGISwapChain::PresentImplCore
  → dxgi!CDXGISwapChain::FlipPresentToDWM
  → dxgi!CFlipPresentToDWM<...>::FlipPresentCore
  → dxgi!CDXGISwapChainWrapper::Blt
  → dxgi!CD3D12Device::EnsureCommandList
  → D3D12Core.dll
  → dxgkrnl!DxgkSubmitSignalSyncObjectsToHwQueue
  → dxgkrnl!DXGDEVICEACCESSLOCKEXCLUSIVE::Acquire
  → ntoskrnl!ExAcquireResourceExclusiveLite
```

Engine の明示的な Queue.Signal が短い Frame でも、Present 内部の経路で同じ種類の取得待ちが現れた。API 名だけで別原因へ分けず、下位の待機 Stack も確認する必要がある。

### 八件共通の通知元

```text
ntoskrnl!ExpWorkerThread
  → dxgkrnl!DxgkpProcessStatusChangeWork
  → dxgkrnl!DXGPROCESS::ProcessStatusChange
  → dxgkrnl!DXGDEVICEACCESSLOCKEXCLUSIVE::~DXGDEVICEACCESSLOCKEXCLUSIVE
  → ntoskrnl!ExReleaseResourceLite
  → ntoskrnl!KiProcessThreadWaitList
  → 対象 Render Thread の ReadyThread
```

Ready Stack の PID / TID は SDK が報告した通知元とも一致した。単に別 Thread が同時に同名関数を実行していたという対応ではなく、対象 Render を Ready にした通知の Stack である。八件とも通知は DPC ではなかった。

Lock の実アドレスや引数はこの ETW Stack に含まれないため、Object Address による取得・所有・解放の照合はしていない。確認できたのは、対象 Thread の取得待ちと、ProcessStatusChange からの排他解放・Ready 通知の経路である。

## 5. 優先度更新・NVIDIA との関係

同時に採った CPU Profile から、通知元 System Thread が待機中に何をしていたかも調べた。

- 八区間の通知元 Thread に計 37 標本。
- 五区間で、非 DPC / ISR の標本計 7 件に `DxgkpProcessStatusChangeWork` → `DXGCONTEXT::SetSchedulingPriority` → `DdiSetContextSchedulingProperties` → **`nvlddmkm.sys`** の Stack があった。
- 内訳は補助条件が 1 / 2 / 0 / 0 件、メモ帳条件が 1 / 1 / 2 / 0 件。0 は標本で経路を捕捉できなかったという意味で、実行しなかった証明ではない。
- NVIDIA 内部が一 Frame だけの標本や未解決 RVA も保持した。標本件数を正確な Driver 処理時間へ換算しない。

前回の「GPU 優先度更新と NVIDIA Driver の処理が関係する」という観測に、今回は Device アクセス排他の待機・解除経路を追加できた。Driver 内部のどの処理が排他保持を長くしたか、全保持時間、Driver の欠陥の有無は未確定。

## 6. 全スパイクが解決したという意味ではない

補助条件の全体最大は、切替境界から離れた Frame 27222、Render 9.7674 ms だった。主な区間は `DXGI.Event` 9.3922 ms。

この別の待機は `UserRequest` 9.3630 ms で、通知元 Stack は次の経路だった。

```text
dxgkrnl!CTokenManager::TokenThread
  → dxgkrnl!CTokenManager::CompleteIndependentFlipToken
  → dxgkrnl!CFlipToken::Discard
  → dxgkrnl!CFlipToken::SignalGpuFenceAndPresentLimitSemaphore
  → dxgkrnl!SignalPresentLimitSemaphore
  → ntoskrnl!KeReleaseSemaphore
```

今回の切替直後の Device 排他待ちとは異なる解除経路だった。[続報](M06-15-Presentation-Stackwalk-Fault.md)で、Present 処理 Thread が ETW Stack 展開中の Hard Fault / I/O を約 8.76 ms 待っていたことを確認した。この一件には計測の影響が含まれる。過去の提出後約 30 ms や約 50 ms の全事例も今回の八件で説明したことにはしない。

## 7. 変更・検証・成果物

今回の Repository 変更は本報告と統合記録への参照追加。解析用 Program と生記録は `out/` に保存した。前回までの Engine 計測・未 Commit 変更は維持した。

検証:

- Trace ON の Development Probe / ETW Export Build 成功。
- 二 Capture の前面・Size・全 Marker・GPU 対応の品質 Gate 成功。
- 選択八件の待機・Ready・再実行の Native 一意照合成功。
- Python 解析 Test 14 件成功。
- `CUE_ENABLE_TIMING_TRACE=OFF` に戻し、`pwsh -NoProfile -File scripts/codex_build.ps1` による既定 Debug Build 成功。結果は `DebugBuild.log` に保存。

主要成果物:

- `Companion1/`、`Notepad1/`: Merged ETL、生 Span / GPU CSV、Events.csv、品質・CPU 状態・GPU 対応の全結果。
- 各 `RenderScheduling.jsonl`: Render の全 ThreadActivity と SwitchIn / Ready Stack。
- 各 `SignalComparison.json` / `.log`: 八件の API 区間と Stack の対応。
- 各 `SystemSamples.jsonl`、Root の `NotifierSamples.json`: 通知元 System Thread の CPU Profile。
- `WaitVerification.json`、`NativeNotificationProof.json`、`Notepad1/PeakNative.txt`: 独立した Native 照合。
- `Companion1/NonTransitionWait.json`: 切替以外の最大 Frame の別経路。
- `Notepad.json` / `NotepadAfter.json`: 専用アプリの同一性と配置。
- `TraceProcessor/`、`AnalyzeSignal.py`、`VerifyWaits.py`: 今回の抽出・照合用 Program。
- `CueSpikeTimingProbe.exe` / `ProbeHashes.txt`: 採取時の実行 File と Hash。今回の解析で Engine の関数名は未解決のため、Module / RVA を保持した。
- `DevelopmentBuild.log`、`AnalysisTests.log`、`DebugBuild.log`: Build / Test。

Symbol は前回のローカル Cache のみを使用した。生 ETL、実行 File、Symbol Cache、個別解析 Program は Git 管理外で、Push / Clone には含まれない。

待機方針、GPU / CPU 優先度、Waitable、Driver / Windows 設定を変える改善は行っていない。今回は切替直後の待機経路の確認を完了した段階であり、性能改善の検証結果ではない。
