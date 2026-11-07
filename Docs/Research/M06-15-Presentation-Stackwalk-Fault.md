# M06-15 / Issue #144 — 表示準備待ちに重なった ETW Stack 展開のページ読み込み

- 調査日: 2026-10-21
- Branch: `m06/issue144/gpu-waits-and-telemetry`、HEAD: `6effec9`
- 前回: [Signal 内の待機と GPU Device 排他](M06-15-Signal-Wait-Confirmation.md)
- 今回は取得済み ETL の追加解析。Engine Code、計測設定、Driver 設定は変更していない。Commit / Push / Merge は行っていない。

## 1. 結論

**前回残った Frame 27222 の約 9.4 ms の表示準備待ちには、ETW の Stack 収集処理が起こしたページ読み込み待ちが含まれていた。**

Present を処理する Intel User Mode Driver の Thread が、ETW Stack 展開中に Hard Fault の I/O を待ち、8.7596 ms 停止していた。その間、直前の Frame の Clear / ImGui Pass は既に完了していたが、表示側の PresentHistory / IndependentFlip 処理が進まなかった。I/O 完了後に Driver Thread、表示側の Token 処理、Render の順で再開した。

これは Engine の描画負荷が約 9 ms あったことを意味しない。また、この一件を通常実行時にも同じ頻度で起こるスパイクの代表値として扱わない。Stack を収集しない比較実行は今回行っていないため、計測を外したときの Max / p95 の改善量は未測定。

## 2. 対象と記録品質

対象は `out/validation/Performance20261021/Signal144/Companion1/` の保存済み記録。

- Probe PID 18940、Render TID 13940、Intel Driver の Present 処理 Thread TID 15980。
- DWM PID 12780、Token Thread TID 14924。PID は当該 ETL の Native Event の Process 名で確認した。
- Frame 27222 は計測開始から 19.4933 秒、最後の前面切替から約 3.49 秒後。
- Frame.Render 9.7674 ms、DXGI.Event 9.3922 ms、DXGI.Present 0.1932 ms。
- Development、960 × 720、Buffering 2、Waitable ON、Queued 2、VSync OFF、Tearing ON、上限なし。詳細な前面・Size・記録欠損の Gate は前回報告を参照。
- Kernel 記録は `CSwitch+ReadyThread+Profile` の Stackwalk 有効。`HARD_FAULTS` / Disk I/O の専用 Event は有効にしていない。
- PresentMon 2.6.0 で計測対象 25,596 件の Present を全て一意に照合。欠落・曖昧対応は 0 件。全件 `Hardware Composed: Independent Flip` で、表示方式の切替はなかった。
- PresentMon 全 27,847 行のうち、計測対象外 2,251 行は Frame 対応済み統計に混ぜていない。

## 3. Render を止めていた経路

Render は `DXGI.Event` 内で `UserRequest` の Waiting 9.3630 ms、その後 Ready 0.0167 ms を経て再開した。通知元は DWM TID 14924。

通知元 Stack は前回のとおり、Token の完了・破棄に伴う Present 枠の返却だった。

```text
dxgkrnl!CTokenManager::TokenThread
  → dxgkrnl!CTokenManager::CompleteIndependentFlipToken
  → dxgkrnl!CFlipToken::Discard
  → dxgkrnl!CFlipToken::SignalGpuFenceAndPresentLimitSemaphore
  → dxgkrnl!SignalPresentLimitSemaphore
  → ntoskrnl!KeReleaseSemaphore
```

今回 DWM Token Thread 自身も調べた。この Thread は約 8.7012 ms、`ProcessTokens → NtWaitForMultipleObjects` で待ち、GPU Scheduler の `PropagatePresentHistoryToken → KeSetEvent` で Ready になった。再実行までの Ready は 0.0072 ms。そのため、DWM Token Thread が約 9 ms CPU を使い続けていたという説明にはならない。

## 4. Present 処理 Thread の停止原因

TID 15980 の再実行 Stack には、次の呼出しの入れ子が記録されていた。読みやすいよう呼出し元から待機先へ並べた。

```text
igd10um64xe.dll
  → ntoskrnl!KiDeliverApc
  → ntoskrnl!EtwpApcPoolThunk
  → ntoskrnl!EtwpTraceStackWalk
  → ntoskrnl!EtwpExpandedStackWalk
  → ntoskrnl!RtlWalkFrameChain
  → ntoskrnl!RtlpLookupFunctionEntryForStackWalks
  → ntoskrnl!KiPageFault
  → ntoskrnl!MmAccessFault
  → ntoskrnl!MiIssueHardFault
  → ntoskrnl!MiIssueHardFaultIo
  → ntoskrnl!IoPageReadEx
  → Ntfs!NtfsCommonRead / NtfsNonCachedIo / NtfsWaitOnIo
  → ntoskrnl!KeWaitForSingleObject
```

この Thread の Waiting は `Executive` 8.7596 ms。Ready の通知元は Idle PID 0 / TID 0 の DPC で、Stack は次の I/O 完了経路だった。

```text
storport!RaidpAdapterRedirectDpcRoutine / RaidUnitCompleteRequest
  → CLASSPNP!TransferPktComplete
  → ntoskrnl!IofCompleteRequest
  → Ntfs!NtfsMasterIrpSyncCompletionRoutine
  → ntoskrnl!KeSetEvent
```

Ready 後の CPU 再実行待ちは 0.0451 ms。再開後、同 Thread は Native の PresentHistoryDetailed を続けて発行した。

したがって、この停止を Intel Driver の Shader 実行時間や Engine Worker の通常の待機として扱うことはできない。Stack 展開そのものの途中でページ読み込み待ちに入ったことと、I/O 完了で再開したことを確認した。一方、読み込んだ File / Page のアドレス、どの Stackwalk 指定がこの APC を発生させたか、ストレージ内部の処理時間の内訳は記録から特定できていない。Driver の内部関数名も未解決のため Module / RVA を保持した。

## 5. 同一時計での順序

以下は ETL 開始からの ms。Native Event は整数 µs 表示、ThreadActivity は ns から換算している。

| ETL 時刻 ms | 観測 |
|---:|---|
| 23574.9365 | Driver TID 15980 がページ読み込み待ちへ入る |
| 23575.4050 | Frame 27221 の最後の GPU Timestamp。Clear / ImGui の区間は約 0.0297 ms |
| 23575.4565 | Render TID 13940 が表示準備の Waiting へ入る |
| 23583.6961 | ストレージ I/O 完了の DPC が Driver Thread を Ready にする |
| 23583.7412 | Driver Thread が再実行 |
| 23583.797 / .832 | Driver Thread が二件の PresentHistoryDetailed を発行 |
| 23583.846 | 当該 Surface の IndependentFlip、PresentCount 27221 |
| 23583.8615 | GPU Scheduler が DWM Token Thread を Ready にする |
| 23583.8687 | DWM Token Thread が再実行 |
| 23584.806 | DWM が先ほどの Token の PresentHistory Stop を発行 |
| 23584.8195 | DWM が Render を Ready にする |
| 23584.8362 | Render が再実行 |
| 23584.8460 | Frame 27222 の DXGI.Event 計測が終了 |

Driver の停止開始時点では Frame 27221 の GPU 処理はまだ終わっておらず、途中で完了した。Render が表示準備を待ち始める時点では既に完了していた。GPU Timestamp の対象は Graph の Pass 区間であり、表示側のコピー・同期・Driver 内部処理まで全て完了したことを示すものではない。

PresentMon は Frame 27220 の `msUntilRenderComplete` を約 9.20 ms、`msGPUActive` を約 9.12 ms と報告した。一方、Engine の同 Frame の GPU Pass 区間は約 0.0307 ms。この二つは計測範囲が異なる。PresentMon の値を Clear / ImGui が約 9 ms 実行した証拠として使わない。

なお、Frame 番号と Native PresentCount は同じ識別子ではない。上表では両者を区別し、非同期の PresentHistory を単に最寄りの CPU Frame に割り当てていない。

## 6. 検証と制限

- Driver / Render それぞれの Waiting 開始、Ready、再実行の計六件を xperf の Native Event と一意照合。最大時刻差は 500 ns で、整数 µs 表示の丸め幅内だった。
- Native Process 名による DWM の同一性、DPC の I/O 完了 Stack、ETW Stack 展開内の Hard Fault の入れ子を確認。
- GPU 校正値で換算した Frame 27221 の最終 Timestamp が Render の Waiting 開始より前であることを検証。当該 Frame の校正感度は約 1.61 µs。
- Driver Thread の全記録を検索し、1 ms 以上の Waiting のうち `EtwpExpandedStackWalk / MiIssueHardFault / NtfsWaitOnIo` が全て揃うものは今回の一件だった。条件に一致しない待機を全て無害と判定したものではない。Warmup や通常の作業待ちも含むため、長い待機件数自体をスパイク件数として扱わない。
- Present 対応と前面条件の既存 Python Test 計 8 件成功。`git diff --check` 成功。
- ETW の Event Lost が 0 件でも、Stackwalk による実行への影響がないことは保証しない。
- 前回の切替直後八件の Device アクセス排他待ちを、今回の一件で取り消したり、全て計測由来と分類したりしない。過去の約 30 / 50 ms の事例も未解決のものが残る。

## 7. 次に比較すべき条件

次は同じ実行 File、UI、Buffering / Waitable、Size と前面条件で、`Stacks` と `CpuSampling` を外した記録を取り、既存の CPU Span・GPU Timestamp・Present / CSwitch / ReadyThread の情報を維持して比較する。

目的は Engine 内の統計品質を落とすことではなく、診断用 ETW Stackwalk が Max / p95 に加える影響を分離すること。Stack なしでスパイクが消える量はまだ測っていないので、改善済みとはしない。再現した待機についてのみ必要な Stack を追加する段階へ進む。

## 8. 成果物と変更範囲

解析出力は `out/validation/Performance20261021/Presentation144/`。

- `PresentMon.csv`、`Companion-modes.json`、`Companion-frames.jsonl`: Present の全件対応。
- `NativeEvents.jsonl`: 約 70 ms の Native 表示・GPU Event。元 Dump は `Signal144/Companion1/NonTransitionNative.txt`。
- `PresentWorkerScheduling.jsonl`、`TokenScheduling.jsonl`: Driver / DWM Thread の全 ThreadActivity と Stack。
- `PresentWorkerScheduling-window.json`、`TokenScheduling-window.json`: 対象時刻の抜粋。
- `DwmSamples.jsonl`、`DwmSamples-window.json`: DWM CPU Sample。対象窓に DWM メイン Thread の Sample は一件あり、長い Shader Compiler 処理を示すものではなかった。
- `Verification.json`: 六件の Native 一意照合と GPU 時刻。
- `LongWorkerWaits.json`: Driver Thread の長い待機と Stack 条件の検索結果。
- `Inspect.py`、`Scheduling.py`、`Quantify.py`、`Verify.py` ほか: 今回の個別解析と Log。

Repository の今回の変更は本報告、前回報告の続報参照、統合記録の入口のみ。今回 Engine Code は変更しておらず、新たな Build は実施していない。前回までの未 Commit 変更を維持した。解析結果はローカルの保存済み ETL に依存し、生記録と個別解析 File は Git 管理外。
