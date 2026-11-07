# M06-15 / Issue #144 — 再起動後の旧版・現在版比較

2026-10-25、Windows の最終起動 22:01:11。22:03 から同条件の比較を行った。

## 結果

再起動後も 2000 FPS には戻らなかった。2000 FPS 当時の保存 Binary は合算平均 1201.190 FPS、現在版は 1189.489 FPS。現在版の差は -0.974% だが、各試行の変動より小さく、この値を確定した Profiler 回帰率とは扱わない。

| 順序 | Binary | 完了 Frame | 測定秒 | FPS |
|---|---|---:|---:|---:|
| 1 | Old | 12,083 | 10.000589 | 1208.229 |
| 2 | Current | 11,454 | 10.000154 | 1145.382 |
| 3 | Current | 11,641 | 10.000612 | 1164.029 |
| 4 | Old | 12,521 | 10.000185 | 1252.077 |
| 5 | Old | 11,433 | 10.000303 | 1143.265 |
| 6 | Current | 12,591 | 10.000360 | 1259.055 |

| Binary | 試行数 | 完了 Frame 合計 | 実経過秒合計 | 合算 FPS |
|---|---:|---:|---:|---:|
| Old | 3 | 36,037 | 30.001077 | 1201.190 |
| Current | 3 | 35,686 | 30.001126 | 1189.489 |

合算は Frame 総数 / 実経過秒総数。瞬間 FPS の算術平均ではない。

## 条件

- RTX 3060、Driver 32.0.16.1742、高パフォーマンス電源プラン。設定変更なし
- Development の保存済み実行 File を使用し、今回再 Build していない
- 枠付き Client 1280 × 720、DISPLAY1、Window 位置 (20,250)
- Pointer (950,934)、Hover は計測アプリ。全試行中・試行間で Pointer 移動なし
- ImGui 通常 UI 有効、Worker ON、Buffering 2、Queued Presents 2、Waitable ON
- VSync OFF、Tearing ON、FPS 上限なし
- Warmup 2 秒、測定 10 秒を各版 3 回
- WPR / PIX / Nsight の Capture なし。アプリ内 CPU / GPU Trace は RAM に保持し、各試行の終了・Worker 停止・GPU 回収後に CSV 保存
- 版ごとに同じ `Bundle/CueSpikeTimingProbe.exe` の Path / File 名へ配置し、開始・終了 Hash を確認

Old SHA256: `37C7DAC0B4B887E175008623528A8E060B8E4AA99A191E4B4F08BDE745CFA7F9`

Current SHA256: `992144D34C321A862B75B1F907C607AD160B24D12F2013B9DC914F5EC3921266`

Old は当時の 2000 FPS 記録に使った保存実行 File と Hash が一致する。

## 周期的な停止

全 6 試行で 3 ms 超の Render 完了間隔は合計 1507 件。最大区間は DXGI.Ready 1504 件、Fence.Wait 2 件、DXGI.Present 1 件だった。

74.973 Hz に対する停止時刻の位相集中度は 0.964～0.988。以前と同型の周期的な Ready 待機が、再起動後にも残っている。今回 Kernel Event の Capture は行っていないため、DWM 内部の履歴処理との照合は前回の記録による知見であり、今回改めて直接観測したものではない。

各試行の測定区間内で RAM 保存した CPU 累積値から、全 16 論理 CPU の非 Idle は約 58.5～71.6%。再起動後であっても、PC 全体が低負荷だったとは扱わない。今回の値から特定 Process の原因を断定しない。

### 計測終了後の負荷発生元の確認

後続の読み取り調査では、通常の Get-Process で CPU 累積値を取得できる Process の合計だけでは、PC 全体の使用率と大きな差があった。保護された Process などを含むため、読み取れた合計を全体使用率として扱わない。

Windows の `Win32_PerfFormattedData_PerfProc_Process` では MsMpEng が CPU 使用率上位となり、1 論理 CPU を 100% とする Counter 値は 146、System は 65 だった。これは Benchmark 終了後の別時刻の Snapshot であり、6 試行中の CPU 内訳や周期スパイクの原因を直接示す値ではない。Defender の停止、除外設定、他アプリの停止などは行っていない。

## 品質と制約

- 全 6 試行 Exit 0、前面・可視・非最小化・寸法の検査成立、状態不一致・状態変化 0
- Pointer / Window 移動 0、Snapshot 追加確保 0、CPU Trace 欠落 0、GPU 欠落・上書き 0
- CSV 行数と宣言記録数、GPU 成功 Status、各試行の開始・終了 Binary Hash を確認
- 補助 Script の実行中と気付かず一度重複起動したが、共有 EXE が使用中のため Copy-Item で失敗し、二つ目の計測アプリは起動していない。元の比較は継続した。重複 Script と時間が重なった試行 4 を除く Old の参考合算は 1175.748 FPS。Current との小差の方向も変わるため、約 1% の差を確定的な回帰と解釈しない
- 再起動前とは Pointer 座標・時刻・背景負荷が異なる。再起動前の約 1100 FPS からの差を、再起動だけの改善率とは扱わない
- 当時の 2073 FPS は複数試行合算、今回の比較は別時刻の 10 秒 × 3 回。全 OS / 表示 / 背景条件の一致を保証するものではない

Engine Source / 設定 / Git 操作の変更なし。既存のローカル変更は保持した。今回の変更は調査用 Script と記録のみで、Engine の Build / CTest は実行していない。計測アプリは終了済み。

## 証跡

`out/validation/Performance20261025/FpsCause/Reboot-20261025-220354/`

- 各試行の Log、CPU / GPU CSV、起動・終了 JSON、Launcher の位置・Pointer 検査
- `Summary.json`: 品質検査、FPS、CPU 区間、GPU Query、周期分類、背景 CPU 使用率
- `Environment.json`: 最終起動時刻、GPU / Driver、電源プラン

調査 Script は `RunRebootStudy.ps1` / `AnalyzeReboot.py`。重複起動の失敗は `Reboot-20261025-220442/` に分けて保持し、正式な試行数・集計には含めていない。
