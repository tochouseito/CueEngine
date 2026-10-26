# M06-15: 前面切替を制御した CPU / GPU / 表示の同時調査

- 日付: 2026-10-19
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 開始時 Commit: `6effec98a6cf93d8f8e8eb54fed23251ce35e7ca`
- 前回の入口: [作業・性能調査の統合記録](M06-15-Work-Summary.md)
- 成果物: `out/validation/Performance20261019/Focus144/`
- 状態: 調査 Tool と本記録を更新。今回の追加分は Commit / Push / Merge していない

## 結論

**今回の条件では、非前面を維持するだけで継続的に大きく遅くなる結果は得られなかった。一方、前面を切り替えた直後の DXGI 表示準備待ちに、約 25～31 ms のスパイクを再現できた。**

有効な三回は各 20 秒、CPU / GPU 生記録の欠落・上書きと ETW Event / Buffer Lost は全て 0。CPU Marker と元 CSV の全件照合、各安定区間の実際の前面 PID の検証も成功した。

今回の大きなスパイクは Shader の長時間実行や、通知後の長い CPU Ready 待ちでは説明できない。選択した Frame では GPU Pass は約 0.03 ms、CPU 状態の大部分は Waiting だった。DWM Thread の実行 Context 中の通知で DXGI 待機から復帰している。

ただし、**以前の約 49.6 ms と CPU 提出後の約 30.4 ms は再現しなかった**。同じ原因を完全に捕捉した結論にはしない。DWM 内部で表示枠を出すまでに何が起きたか、他 GPU 作業・Driver・HWS の詳細原因もまだ確定していない。

## 1. 実験方法

### 固定した条件

- Development、通常 UI、Client 1280×720、FPS 無制限
- Buffering 2、GPU 未完了上限 2、SwapChain 2、Waitable ON、Queued presents 2
- VSync OFF、Tearing ON。Engine の Thread 優先度、Affinity、描画・待機方針は変更しない
- CPU 全区間、GPU 生 Timestamp / Calibration、Kernel Scheduling、DxgKrnl Base / Scheduling / HWS、DXGI / DWM を同時に記録
- Warmup 2 秒後、20 秒計測。計測中に Build や重い解析を並行しない

### 切替の制御

`CueSpikeTimingProbe` に `FocusCycle` を追加した。起動時に同じ実行 File の別 Process を補助として起動し、GPU Device を作らない静的な Win32 Window を用いる。補助 Window は 120×60。計測 Window の外に配置できない場合は開始を拒否する。

| Phase | 計測開始から | 期待する前面 |
| --- | --- | --- |
| 0 | 0～4 秒 | Probe |
| 1 | 4～8 秒 | 補助 Process |
| 2 | 8～12 秒 | Probe |
| 3 | 12～16 秒 | 補助 Process |
| 4 | 16～20 秒 | Probe |

前面にいる Process から正規の許可を渡し、補助側の Message Handler で切り替える。Windows が拒否した要求や、第三のアプリが前面を取得した状態は成功した比較に含めない。入力の擬似送信、Foreground Lock の無効化、Thread の強制接続は行わない。[SetForegroundWindow の契約](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)

要求時刻・成否とは別に、Host Step 境界で実際の前面 HWND / PID / TID を観測し、変化を全件保存する。Window の可視・最小化・Client Size は既存の観測も継続する。[GetWindowThreadProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowthreadprocessid)

切替途中では一時的に前面 HWND が 0 になる例があった。実測では約 1～13 ms の範囲で期待した Window に移った。安定区間は各 Phase の前後 250 ms を分離した **各 3.5 秒**とし、この範囲内で期待した PID を維持したか検証する。切替区間やそれを跨ぐ Frame も、生記録と別の全体統計に残す。除外してスパイクを隠す扱いにはしない。

## 2. 試行と採否

| 試行 | 結果と扱い |
| --- | --- |
| Run1 / Placement | 当初の補助 Window サイズでは非遮蔽配置ができず、計測前に終了。サイズを縮小した |
| Run2 | 最初の往復は成功したが、2 回目の非前面化を OS が拒否。比較の不成立として残す |
| Run3 | 4 回の切替、安定区間の PID、全生記録照合、ETW 欠落 0 を確認。有効 |
| Run4 | 同上。有効 |
| Run5 | 切替は成功したが、ETW Event Lost **796,768**。Exporter は失敗を返した。完全な同時記録として採用しない |
| Run6 | Marker Session を分離し、収集 Buffer を拡張して再測定。全ての検証が成功。有効 |

Run3 / Run4 は従来の逐次 Session、Run6 は Kernel 64～128 MiB、GPU / 表示 128～256 MiB、Marker 64～256 MiB を目安とする別 Session で記録した。Run6 の Buffer 設定は異なるため、別実行の FPS 差を前面効果や Engine 改善率には使わない。Marker の大量出力と File 保存は計測終了後に行う。

## 3. 安定区間と切替区間の比較

表の平均の範囲は、同一実行内の前面三 Phase / 非前面二 Phase のそれぞれの平均。p95 の異なる標本群を平均した値ではない。単位は ms。

| 試行 | 前面の Render 平均範囲 | 非前面の Render 平均範囲 | 前面の安定区間 Max | 非前面の安定区間 Max | 切替を含む全 Render Max |
| --- | --- | --- | ---: | ---: | ---: |
| Run3 | 1.0652～1.0742 | 1.0576～1.0647 | 5.0105 | 4.9317 | **29.6719** |
| Run4 | 0.9673～0.9678 | 0.9664～0.9698 | 3.5642 | 3.6386 | 5.6045 |
| Run6 | 0.8443～0.8547 | 0.8541～0.8576 | 4.9812 | 5.1205 | **31.4203** |

各安定 Phase の p95 は Run3 が約 4.31～4.40、Run4 が約 2.83～2.84、Run6 が約 3.77～3.95 ms。前面化だけで安定区間の待機がなくなる結果ではない。

CPU 提出終了 → 最初の GPU Query の安定区間の最大は、Run3 が前面 0.6223 / 非前面 0.2218、Run4 が 0.3413 / 0.3040、Run6 が 0.2181 / 0.2030 ms。切替区間を含めた全 Frame の最大も 1.2657 / 0.3605 / 1.4799 ms で、以前の約 30.4 ms とは異なる。

この比較は「描画しない小さな別アプリが前面、計測 Window は可視で遮蔽しない」という条件である。別ゲーム・Browser・全画面アプリが前面になる場合や、長時間の背景実行、遮蔽・最小化まで一般化しない。前面と非前面の安定時間も一回あたり 10.5 秒 / 7 秒と異なる。

## 4. 切替直後の同一 Frame の内訳

| 試行 / Frame | 切替と Render 開始 | CPU Render | Running / Waiting / Ready | GPU Pass 区間 | 提出終了 → 最初の GPU Query |
| --- | --- | ---: | --- | ---: | ---: |
| Run3 / 5503 | 非前面要求 4 秒、開始 4.0445 秒 | 14.6825 | 0.7482 / 13.8033 / 0.1310 | 0.033792 | 1.265724 |
| Run3 / 9214 | 前面復帰要求 8 秒、開始 8.0234 秒 | 25.1877 | 0.8290 / 24.2245 / 0.1342 | 0.038912 | 0.859180 |
| Run3 / 12903 | 非前面要求 12 秒、開始 12.0111 秒 | **29.6719** | 0.8313 / 28.7618 / 0.0788 | 0.033792 | 0.983164 |
| Run3 / 16608 | 前面復帰要求 16 秒、開始 16.0231 秒 | 24.3781 | 0.8425 / 23.3856 / 0.1500 | 0.033792 | 0.750344 |
| Run6 / 16007 | 非前面要求 12 秒、開始 12.0126 秒 | **31.4203** | 0.9332 / 30.3529 / 0.1342 | 0.027648 | 0.924636 |
| Run6 / 20604 | 前面復帰要求 16 秒、開始 16.0250 秒 | 28.5543 | 0.8266 / 27.6354 / 0.0923 | 0.028672 | 0.825044 |

表の期間は ms、開始時刻は測定開始からの秒。選択した Frame の Unknown / Ended は 0。GPU 区間は Query 間であり、純粋な Shader 演算時間ではない。

Run3 / Frame 12903 の DXGI.Event は 8.9074 ms Timeout、15.9482 ms Timeout、4.0305 ms Signaled。`DXGI.Ready` 全体は **28.9232 ms**。1 ms の Timeout 指定が実際の Return 間隔の上限ではない点は従来と同じ。

### DWM による復帰と CPU 再開

Run3 の選択した五 Frame の最後の Signaled 待機について、ReadyThread が出た CPU の直前 CSwitch を照合した。全て TID **2232** の実行 Context 中だった。同じ ETL の Process / Thread 一覧では **dwm.exe PID 872** に対応する。

Frame 12903 は Ready 通知 → DXGI.Event Return が約 **9.1 µs**、五 Frame は約 6.45～76.85 µs。今回の約 29.7 ms 全体を、通知後に Render Thread が長く実行されなかったことでは説明できない。

これは DWM Context 中の通知という観測である。Stack による ISR / DPC の分離や、DWM の内部 Policy の特定は行っていない。以前の別 ETL の PID を流用せず、この記録内で対応付けた。

### Engine の GPU Queue と表示待機の関係

Run3 の同一 ETL を xperf で復号し、Device → Context → HwQueue の所有を追跡した。選択 Process の QueuePacket **485,573 組**を開始・終了で対応付け、未対応 Start / Stop は 0。Event 名と Payload 長ごとの Header を用い、型と SubmitSequence の不一致を拒否した。

Frame 12903 では:

- 直前 Frame 12902 の最後の GPU Query は、現在の Render 開始より約 81.7 µs 前に完了していた。
- 照合した Engine の先行 Queue Packet は Render 開始後約 0.91 ms までに完了している。
- そこから DXGI.Ready Return まで約 **28.0 ms** 残った。現在 Frame の Command 提出は表示許可を得た後だった。

従って、この Frame の待機全体が「Engine 自身の既知の先行 Packet が約 29 ms 未完了だった」という形ではない。GPU Query 完了と Queue Packet 完了も同一時点として扱わない。

HWS SchedulingLog は 200 Event、従来型 DmaPacket Start / Stop は各 3,108、現代の DmaPacket Info も取得した。ただし HWS の Binary を GPU の全 Context の実行区間へ完全展開できてはいない。他 Process の GPU 使用、DWM の合成や表示判断、Driver 内部まで不存在を証明するものではなく、「GPU 全体が 28 ms Idle」とは主張しない。

根拠: `Run3/GpuSchedulingDetails.json`、`ReadyDetails.json`、`Processes.txt`、`Dxg.txt`、`Clock.txt`、`on-q2-spans.csv`、`on-q2-gpu-frames.jsonl`。

## 5. 記録品質

| 試行 | 全保持 CPU 区間 | GPU 生記録 | ETW CPU Marker（Clock 2 件を含む） | Event / Buffer Lost |
| --- | ---: | ---: | ---: | --- |
| Run3 | 813,363 | 74,100 | 813,365 | 0 / 0 |
| Run4 | 899,747 | 82,020 | 899,749 | 0 / 0 |
| Run6 | 1,011,203 | 92,256 | 1,011,205 | 0 / 0 |

- 三回合計 2,724,313 CPU 区間、248,376 GPU 記録を保持。CPU / GPU の容量不足と上書きは 0。
- 62,074 提出 Frame / 124,148 Pass を対応付けた。計測開始境界の各 2 Frame / 4 Pass は Warmup の提出が生記録にないため、未対応理由を保存した。
- CPU 生 CSV と ETW Marker は Thread / Sequence / 名称 / Frame / 時刻 / Detail / Outcome を全件照合した。
- Provider Decode Error は 0。Kernel の未分類 State は全 Process 合計で 136 / 110 / 115 件を残しており、無視していない。表の対象スパイクは Unknown 0。
- 各回で補助 Process の終了、Collector の Session 停止と Merge を確認。Run6 は別 Marker Session の停止も確認した。
- Clock 対応の不確かさと Calibration 投影差は元の解析に保持する。全 Frame が同じ精度とは仮定しない。

`Run5/Export.log` の Lost は失敗として保持する。Event Lost を単に GPU Pass が短かったことや他の成功した試行で相殺しない。

## 6. 実装範囲と検証

変更は手動 Probe、`FocusExperiment.h`、Collector の `-FocusCycle`、`analyze_focus_cycle.py` と Test、報告文書。Engine の描画・Thread・GPU 同期・Waitable の通常方針と直近 120 標本の統計は変更していない。

- Trace ON / Development: Probe と ETW Exporter の Build 成功。
- 実 Window: 正常な切替、実 PID、非遮蔽配置、OS 拒否時の記録、補助 Process 終了、ETW 欠落時の失敗を確認。
- Python: 安定区間を跨ぐ標本、他 PID、欠落した要求、要求失敗を確認する 3 Test 成功。
- `pwsh -NoProfile -File scripts/codex_build.ps1`: 既定 Debug Build 成功。TimingTrace / SpikeTrigger の 2 Test も成功。`Focus144/DebugBuild.log`、`DebugTests.log` に保存した。
- `git diff --check` 成功。通常の `CUE_ENABLE_TIMING_TRACE:BOOL=OFF` へ復元した。

## 7. 再現手順

Trace ON の Development Probe / Exporter を Build 後、新しい空の出力 Directory で実行する。Collector のみ UAC で昇格する。初期前面化が OS に拒否された場合は、45 秒以内に計測 Window をクリックする必要がある。

```powershell
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=ON
cmake --build --preset windows-vs2026-development --target CueSpikeTimingProbe CueSpikeEtwExport
pwsh -NoProfile -File scripts/capture_spike_etw.ps1 `
    -OutputDirectory C:/Users/sinse/source/repos/CueEngine/out/validation/FocusNew `
    -Seconds 20 -Cases latency2 -FocusCycle -HardwareScheduling
```

`FocusCycle` は 20 秒・`latency2`・Triggered なしに限定し、Foreground 確認と Deferred Marker を自動的に有効にする。

```powershell
# RUN は実際の出力 Directory に置き換える
out/build/windows-vs2026/Engine/Tests/Development/CueSpikeEtwExport.exe RUN/Merged.etl RUN/Events.csv > RUN/Export.log 2>&1
python -B scripts/analyze_gpu_timeline.py RUN/on-q2
python -B scripts/analyze_spike_etw.py RUN/Events.csv
python -B scripts/analyze_focus_cycle.py RUN/on-q2
cmake --preset windows-vs2026 -DCUE_ENABLE_TIMING_TRACE=OFF
pwsh -NoProfile -File scripts/codex_build.ps1
```

Exporter の標準出力・標準 Error は `RUN/Export.log` に保存する。前面比較の解析はこの欠落情報と `CpuScheduling.json` も確認する。`succeeded` は前面条件と記録品質の両方が成立した時だけ true で、成功時 Exit Code 0、それ以外は 1。生 CSV / ETL / 解析成果物は `out/` 配下の Git 管理外に置く。

## 8. 残る調査

今回、切替直後のスパイクについて、表示許可待ちと Engine の既知の GPU 作業を同じ ETL 上で分離できた。次は **DWM / PresentHistory / Present Mode の切替を具体的な Token と Frame に対応付けること**、および通常の別アプリへの切替でも同じ挙動かの確認が候補となる。

以前の約 50 ms / 提出後約 30 ms を解くには、前面 Process とその GPU 負荷も記録した状態で同じ停滞を再度捕捉する必要がある。今回の小さな補助アプリとの比較だけを根拠に、Waitable を外す、Fence を待たない、Worker を OFF にする変更は提案しない。
