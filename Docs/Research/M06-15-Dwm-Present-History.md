# M06-15: PresentHistory と DWM の CPU 描画区間の調査

- 日付: 2026-10-20
- Issue: [#144](https://github.com/tochouseito/CueEngine/issues/144)
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 開始時 Commit: `6effec98a6cf93d8f8e8eb54fed23251ce35e7ca`
- 前回: [前面切替を制御した同時調査](M06-15-Controlled-Focus-Comparison.md)
- 今回の成果物: `out/validation/Performance20261020/Present144/`
- 元の記録: `out/validation/Performance20261019/Focus144/Run3`、`Run4`、`Run6`
- 状態: 既存 ETL を追加解析。今回の変更は未 Commit / 未 Push / 未 Merge

## 結論

**切替直後の大きなスパイクでは、DWM メイン Thread の長い CPU 描画区間が、Engine の表示許可待ちに重なっていた。**

- Run3 / Frame 12903 は Engine の Render が 29.6719 ms。その同じ時間窓で DWM TID 2232 は Running 29.6535 ms、Ready 0.0184 ms、Unknown 0。
- 対応する DWM の `ETWGUID_DRAWEVENT` は全体で 30.574 ms。その内側に Shader Link と Shader Create の区間がある。
- Run6 / Frame 16007 も Render 31.4203 ms の時間窓で DWM が Running 31.4203 ms、Unknown 0。
- 一方、Run4 の最大 Frame 10108 は Render 5.6045 ms の時間窓で DWM が Waiting 5.6045 ms。今回確認した大きなスパイクと、小さな表示待ちは同じ内訳ではない。

**表示方式の変更は確認されなかった。** Frame と一意に対応した 62,068 件の Present は全て `Hardware Composed: Independent Flip`。約 25～31 ms のスパイク中も同じだった。

これは、選択したスパイクを Engine の Shader 実行や Worker の起床遅延だけへ帰す説明より具体的な結果である。ただし DWM 内部のどの関数が CPU 時間の全てを使ったかは未確定。以前の約 50 ms と GPU 提出後約 30 ms の停滞を同じ原因と断定するものでもない。

## 1. 今回行ったこと

新しいアプリ起動や ETW 採取は行わず、前回の欠落ゼロの三 ETL を使った。

1. Intel 公式 PresentMon の Standalone CLI **v2.6.0** で各 ETL をオフライン解析。
2. `--v1_metrics --qpc_time` の QPC と、Engine の生 `DXGI.Present` 開始・終了区間を ClockSync で一意に対応付け。
3. 同じ ETL の DXGI / DWM Provider を xperf で復号し、PresentHistory、IndependentFlip、Surface Update を調査。
4. DWM の Native Start / Stop 区間を対応付け、Kernel CSwitch / ReadyThread から同 Thread の Running / Waiting / Ready / Unknown を積算。

PresentMon は Engine に導入していない。調査用 `out/` 配下に保存し、Intel Corporation の有効な署名と SHA256 を記録した。`Tools/Help.txt`、`Signature.json`、`SHA256.json` に根拠を保存している。

`Hardware Composed: Independent Flip` は PresentMon が Hardware Overlay Plane を使う表示として分類する名称である。表示方式名から、DWM が一切関与しないと判断することはできない。[公式 PresentMon の CSV 定義](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)

## 2. Present Mode の照合結果

| 元の実行 | 生 Present 区間 | PresentMon と一意に照合 | 照合失敗 | 対応した全 Present の Mode |
| --- | ---: | ---: | ---: | --- |
| Run3 | 18,516 | 18,516 | 0 | Hardware Composed: Independent Flip |
| Run4 | 20,497 | 20,497 | 0 | 同上 |
| Run6 | 23,055 | 23,055 | 0 | 同上 |

Warmup や生区間の境界外の PresentMon 行は、この表の Frame に混ぜない。FrameController の測定期間の完了 Frame 数と、開始・終了が記録期間内に収まった Present 区間数が同じとは限らない。

`analyze_present_modes.py` は次を確認する。

- 対象 PID、単一 SwapChain、QPC の単調性と重複。
- ClockSync の生 QPC と単調時計を用い、巨大な絶対時刻は整数・Fraction で扱う。
- API 区間を ClockSync の読取り幅だけ広げ、その中に一件だけ存在する Present を採用。
- 複数候補、欠落、同じ Present の再使用を失敗として保存する。最も近い別 Frame に推定で割り当てない。
- Mode の初期状態と変化を全記録から保存し、スパイク Frame の情報も残す。

Run3 の全 Process 記録では起動初期に Composed: Flip があるが、測定期間の対応済み Frame では全て Hardware Composed だった。対象 Surface の IndependentFlipTransition も今回抽出した記録には存在しなかった。従って、**今回の前面切替スパイクを Composed と Independent Flip の往復だけで説明する根拠はない**。

PresentMon の `msGPUActive`、Dropped、表示完了推定を今回の根本原因の数値としては採用していない。元の Capture には Win32k Provider を指定しておらず、公式 Consumer にも Win32k Event がない場合の補完経路がある。GPU の時間は Engine の生 Query、CPU 状態は Kernel の記録で確認する。[使用 Version の Consumer 実装](https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentData/PresentMonTraceConsumer.cpp)

## 3. Frame と履歴の対応方法

Run3 は Process 起動前から記録しており、対象は単一 SwapChain。

- 対象 PID 13952 の DXGI Present Start は **20,267 件**。SwapChain は `0x0000018e321b1ab0` の一つ。
- 対象 PID の PresentHistoryDetailed Start も **20,267 件**。後半では Render TID 30508 とは別の TID 30756 で発行されている。
- Engine 所有 Context の IndependentFlip は **20,038 件**。Surface LUID `0x03805c0c`、Bind ID 1、PresentCount 230～20267 が重複・欠落なく続く。
- 対応済みの API 区間と Native DXGI Start を照合し、全呼出し順とこの連続 Counter を検証して履歴の発行順へ対応付けた。Native Start → History Start → IndependentFlip の順序も全対象で確認。

この記録の Process 開始・単一 SwapChain・連続 Counter を前提とする照合である。途中から記録した任意の ETL に、無条件で「Frame ID + 1 が PresentCount」と適用する方法ではない。

### Token の再利用

Token と TokenData は再利用される。TokenData 自体を Frame ID や PresentCount と解釈していない。

全 Process の履歴を時系列で処理し、Token / TokenData / Model の未完了世代が一件だけある Stop を採用した。対象 Process は **20,267 組全て**対応し、曖昧な Stop は 0。対象外を含む全体では対応する Detailed Start のない Stop が 35 件あり、未対応のまま保持している。

Stop と Surface Update は別の Event。Frame の通知時刻へ最も近い Token を選ぶ処理にはしていない。

## 4. 約 29.7 ms の時系列

Run3 / Frame 12903 は、12 秒の非前面化の直後に開始した。以下は **同じ Merged ETL 相対 µs**。小数付きの CPU / Ready 時刻と、µs 表示の Native Event は精度が異なる。

| Event | 相対 µs |
| --- | ---: |
| DWM の Draw 開始 | 15,248,145 |
| 直前 Frame 12902 の Present 呼出し | 15,249,878.6 |
| Frame 12903 の Render 開始 | 15,250,098.65 |
| Frame 12903 の DXGI.Ready 開始 | 約 15,250,103.95 |
| 直前 Frame の履歴発行: PresentCount 12903 に対応 | 15,250,178 |
| 同じ PresentCount の IndependentFlip | 15,250,972 |
| 同じ Token 世代の PresentHistory Stop: DWM TID 5544 | **15,251,468** |
| DWM の Draw 終了 | **15,278,719** |
| DWM の SCHEDULE_PROCESS_FRAME 終了 | 15,278,948 |
| DWM が次の SCHEDULE_PROCESS_FRAME を開始 | 15,278,999 |
| DWM TID 2232 の実行 Context 中で Render が Ready | **15,279,016.5** |
| DXGI.Event の Signaled Return | **15,279,025.6** |
| 同じ Surface / PresentCount の SCHEDULE_SURFACEUPDATE | **15,279,057** |
| 現在 Frame 12903 の Present 呼出し | 15,279,422.3 |

直前 Frame の履歴 Stop から Ready 通知までは **27.5485 ms**。この間に DWM メイン Thread の長い描画が存在する。一方、Ready 通知から Native Wait Return は約 **9.1 µs**である。

**Surface Update は Ready 通知より約 40.5 µs 後**に記録されている。この Event 自体が通知を発生させた、という因果順序にはしない。履歴 Stop を「GPU の最後の Query」や「画面表示そのもの」とも同一視しない。

今回の結果は「履歴の提出や処理が約 29 ms 全く進まなかった」という説明を絞り込む。履歴を扱う DWM の別 Thread は早く処理していても、メイン Thread の描画処理と表示許可通知は別の時点になる。

根拠: `Run3/HistoryJoin.json`、`TokenJoin.json`、`DisplayEvents.jsonl`、前回の `Run3/ReadyDetails.json`、`Processes.txt`。

## 5. 待っている間の DWM の CPU 状態

各行は Engine の同じ Render 開始～終了に切り詰めた DWM TID 2232 の状態。Native DWM Event の PID は全て 872 だった。

| 実行 / Frame | Engine Render ms | DWM Running ms | Waiting ms | Ready ms | Unknown ms | 同じ窓に重なる Shader Create ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Run3 / 12903 | 29.6719 | **29.6535** | 0 | 0.0184 | 0 | 5.3194 |
| Run3 / 9214 | 25.1877 | **25.1877** | 0 | 0 | 0 | 3.0320 |
| Run3 / 16608 | 24.3781 | **24.3781** | 0 | 0 | 0 | 2.9470 |
| Run4 / 10108 | 5.6045 | 0 | **5.6045** | 0 | 0 | 0 |
| Run4 / 18299 | 5.3302 | 0 | **5.3302** | 0 | 0 | 0 |
| Run6 / 16007 | 31.4203 | **31.4203** | 0 | 0 | 0 | 5.6689 |
| Run6 / 20604 | 28.5543 | **28.5543** | 0 | 0 | 0 | 3.5280 |

Running は Thread が CPU に割り当てられていた状態で、ISR / DPC 等を除いた純粋な DWM 関数演算時間ではない。ただし長時間 Waiting / Ready だった状態とは区別できる。

Run3 の対応する DWM Native 区間は次のとおり。

- SCHEDULE_PROCESS_FRAME: 33.419 ms。
- その内側の SCHEDULE_RENDER: 32.410 ms。
- その内側の ETWGUID_DRAWEVENT: **30.574 ms**。Running 30.5556 ms、Ready 0.0184 ms。
- Draw 内の SHADERS_D3DSHADER_CREATE: 3.629 ms と 3.021 ms、計 6.650 ms。
- SHADERS_D3DSHADER_LINK: 0.401 ms と 0.289 ms。

Shader Create のうち Engine の Render 時間窓に重なる部分が表の 5.3194 ms。親の描画区間と内側の生成区間は加算しない。残る時間を Shader Compile、Driver、Cache miss のいずれかへ断定する CPU Stack はまだない。

Run4 の選択した上位五 Frame には重なる Shader Create がなかった。これだけで「DWM の Cache が温まったから Run4 は速い」と確定することもできない。

## 6. 変更・検証・制限

### 変更

- `scripts/analyze_present_modes.py` と、その境界・巨大 QPC・曖昧な候補を検証する Test を追加。
- 本報告を追加し、既存の統合記録から参照する。
- Token / DWM の個別解析 Program は今回の `out/validation/.../Present144/` に保存。
- Engine の C++、描画、PSO、待機方針、Thread 優先度、Windows の表示・電力設定は変更していない。前回の未 Commit 変更も保全。

### 検証

- 三回の元 Capture は ETW Event / Buffer Lost と Decode Error が全て 0。今回の PresentMon はオフライン実行で、採取時の負荷には影響しない。
- 記録済み Present 区間 62,068 件の一意照合に成功。Mode 照合の欠落・曖昧・重複採用は 0。
- Native Header / 列数の曖昧さを拒否した。Run3 の対象履歴 20,267 組と IndependentFlip 20,038 件の順序を検証した。
- Python の新規 Test 3 件成功。
- `pwsh -NoProfile -File scripts/codex_build.ps1`: 既定 Debug Build 成功。`CUE_ENABLE_TIMING_TRACE:BOOL=OFF` を維持。
- 性能の速度合否判定は追加していない。過去の Lifecycle Test の一度の Timeout は本調査で解消を確認したものではない。

### まだできていない確認

- 通常の別アプリへの切替比較は今回新たに実行していない。前回の小さい補助 Window の条件を利用した解析である。
- DWM / Driver の CPU Sampling Stack は元 ETL に採取していない。Draw 内の約 30 ms の関数別内訳は未確定。
- Win32k を含む完全な表示追跡や光学的な表示時刻は確認していない。
- 以前の約 50 ms / 提出後約 30 ms を今回の DWM CPU 描画と同じ原因にはできない。

## 7. 再現と成果物

[使用した公式配布の Standalone CLI](https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe)

使用した CLI の SHA256: `B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF`

Repository Root から、既存の Run3 に対する例:

```powershell
out/validation/Performance20261020/Present144/Tools/PresentMon-2.6.0-x64.exe `
    --etl_file out/validation/Performance20261019/Focus144/Run3/Merged.etl `
    --process_id 13952 --v1_metrics --qpc_time --no_console_stats `
    --output_file out/validation/Performance20261020/Present144/Run3-PresentMon.csv
python -B scripts/analyze_present_modes.py `
    out/validation/Performance20261019/Focus144/Run3/on-q2 `
    out/validation/Performance20261020/Present144/Run3-PresentMon.csv `
    out/validation/Performance20261020/Present144/Run3
```

PID は元の ETL の対象 Process ID を使用し、現在の生きている Process ID に置き換えない。

主要成果物:

- `Run3/4/6-PresentMon.csv`、`Run3/4/6-modes.json`、`Run3/4/6-frames.jsonl`: 全行と Frame 対応、Mode。
- `Run3/HistoryJoin.json`、`TokenJoin.json`: Native 呼出し順と PresentCount・Token。
- `Run3/DisplayEvents.jsonl`、`DwmIntervals.json`、各回の `DwmComparison.json`、`DwmSummary.json`: DWM の Native 区間と CPU 状態。
- `InspectPresent.py`、`JoinHistory.py`、`JoinTokens.py`、`InspectDwm.py`、`DwmCompare.py`: 個別解析用 Program。
- `DebugBuild.log`、`ModeTests.log`、`Tools/`: Build / Test と外部 Tool の出自。

`out/` は Git 管理外。Clone / Push に生 ETL、外部 exe、個別解析成果物は含まれない。

## 8. 次に原因を狭めるなら

優先するのは **DWM の CPU Sampling / Stack を同時に採ること**。Shader Link / Create と残りの Draw 内で、DWM 自身、D3D Runtime、Shader Compiler、GPU Driver のどこに CPU 時間を使っているかを分ける。

通常アプリへの切替でも同じ処理が出るかの比較は、その Stack 採取と組み合わせるのがよい。現時点で Engine の PSO Cache、Worker OFF、Waitable 無効化が、この DWM 側処理を解消するとは判断しない。
