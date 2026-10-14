# FrameController FPS 表示（M06-01）

Milestone: [M06 Performance Tests](https://github.com/tochouseito/CueEngine/milestone/7)
Issue: [#87](https://github.com/tochouseito/CueEngine/issues/87)

## 表示と計測点

既定 EditorHost の Test Window に TEST と `FrameController FPS: <数値> (avg <区間数> intervals)` を ImGui Text で表示する。UI は Main Thread で構築し、UI 開始前に取得した FrameController の同期済み `FrameTimingInfo` を利用する。独自 `EditorHostConfig::buildUi` を指定した場合は、その Callback が UI を構築する

FPS は `FrameTimingInfo::frameInterval.averageDuration` の秒数の逆数。Render Callback（WindowsHost では記録・提出・Present）を終えた二つの完了点の間隔を、最大 120 区間で平均する。各区間の FPS を算術平均せず、区間数 / 合計経過時間に相当する完了頻度を使う。Render / Present 後には FPS 待機を挟まない。起動直後など二つの完了点が揃わない場合、または平均間隔が非正の場合は `FrameController FPS: --` と表示し、零除算しない

表示は小数 1 桁の平均 FPS であり、GPU 完了数ではない。Sample が 120 区間未満の間は取得済み区間だけを使い、実際の区間数を表示する。ImGui IO.Framerate とモニター Refresh Rate は使わない。GPU 処理は Submit 後も続くため、この値から GPU 負荷を断定しない。最小化中は新規 Frame と UI 構築を停止する。復帰直後の平均には休止時間が含まれ、その Sample が最大 120 区間で置き換わるまで影響が残る

`Frame interval` は同じ Snapshot の Render 完了間隔を平均・p95・最大 ms で表示する。平均 FPS の安定と実際の Frame Pacing の安定を区別し、短時間の遅延を平均表示で隠さない。直近の瞬間値が必要な利用者は `FrameProgress::lastFrameInterval` を参照できる

## FPS 待機と区間別計測（M06-02 / M06-14）

[#88](https://github.com/tochouseito/CueEngine/issues/88) は旧 CueEngine の高精度 Timer と短い Spin を採用する。Windows の `Waiter::sleep_for` は Thread ごとの Waitable Timer と停止 Event を所有し、Main／Update／Render の期限を混線させない。高精度 Timer 非対応時は通常 Timer、生成失敗時は Condition Variable へ戻る。停止要求で Timer 待機を解除する。通知待機は従来の世代付き通知を維持する

FrameController の `maxFps` は Main / UI 構築の開始頻度を制限する。`wait_for_frame()` は Callback を実行せず、開始期限と空き枠を確認する。未準備の場合は最大 1 ms の指定時間だけ待ち、最後の `clamp(interval / 8, 250 us, 1 ms)` を時計と `Waiter::relax` で合わせる。OS の実際の休止時間は指定時間より長くなることがある。上限 0 は FPS 待機を行わず、空き枠の待機だけを行う

WindowsHost は開始待機後に Message Pump を行い、開始可能な周回だけ Main → Update → Render → Present を進める。待機中の周回も Message を処理するため、低 FPS でも Close / Resize を扱える。Resize の GPU 待機後は次の Pump で入力を取り直してから UI を再開する。Window を持たない Runtime は `step()` で開始待機と実行をまとめられる。単一 Thread でも未準備の `step()` は false を返し、呼出側が再試行する

`advance()` は FPS / 容量待機を行わず、未準備なら false を返す。遅れた Frame は実際の Main 開始時刻から次回期限を計算し、古い予定へ追いつく連続投入は行わない。先行 Frame の Render が遅れている場合は許可済み Frame が続けて完了することがあり、直近の Render 完了間隔の逆数が一時的に `maxFps` を超えることがある。開始頻度と表示完了頻度を区別する。通常 FPS でも短い Spin の CPU 使用があるため、FPS と電力・CPU 使用率は別に評価する

[#100](https://github.com/tochouseito/CueEngine/issues/100) の `FrameController::timing_info()` は Main／Update／Render Callback、FPS 待機、完了間隔を独立に返す。直近 120 件の固定容量 Sample に対して直近値・整数平均・nearest-rank p95・最大を返す。共有 Lock 中に Sample をコピーし、集計は Lock 外で行う。Render Callback 時間へ FPS 待機を混ぜない。WindowsHost の Render には Graph 記録・GPU 依存待機・Present が含まれ、これらの内訳を合計して Render に加算しない

FPS 待機 Sample は次の開始を初めて拒否した時刻から期限までの時間を投入時に記録する。空き枠による期限後の追加待機は含めない。`lastLimitWaitDuration` は最後に投入した Frame の値、`lastFrameInterval` は最後に完了した Frame の値であり、並列実行中は対象 Frame が異なる場合がある

## 確認手順

- 既定 Debug Build: `pwsh -NoProfile -File scripts/codex_build.ps1`
- 全 Test: `ctest --preset windows-vs2026-debug --output-on-failure`
- Visual Studio: 生成済み `out/build/windows-vs2026/CueEngine.slnx` の CueEditorHost / Debug x64 を起動する
- 既定の FPS 上限は 60。動作環境や処理負荷によって値は変動する
- Resize / 最小化 / 復帰では Runtime と Worker を維持して UI を再開する

## M06-01 時点の検証結果

2026-10-07 に Debug Build と全 38 Test が成功した。実画面確認は Computer Use のユーザー停止により未完了。速度改善は行っていないため、比較対象を伴う性能改善の主張はしない

M06-02 以降の待機改善、構成別検証と比較数値は [M06Performance.md](M06Performance.md) に記録する

## #114 の開始待機への変更と検証

2026-10-09 に `scripts/codex_build.ps1` の既定 Debug Build と全 45 CTest が成功した（39.74 秒）。FrameController Test は単一 Thread / Worker の両方で、Main 前の開始制限、待機後の入力採用、Render 後の待機なし、独立した時間計測、遅延後の連続投入拒否を検証する。WindowsHost Test は 1 FPS でも Message を処理し、最新入力を Main へ渡し、次回期限前に Close を処理することを確認する。既存の Resize / 最小化 / 復帰と ImGui 描画 Test も成功した

Development / Release の Build と、実操作によるドラッグの体感・入力から画面表示までの遅延測定は未実施。先行 Frame 数と GPU / DXGI の待機は今回変更していない

## 2026-10-12 の FPS 表示の揺れの確認

変更前の既定 Editor を診断用 Host から各構成 360 Frame 起動し、最初の 120 Frame を除いた 240 区間を観測した。Windows / MSVC x64、1280×720、Main / UI は構築 Thread、Update / Render は Worker、Frame 枠数 2、上限 60 FPS、VSync / Tearing は無効。独自 UI は注入せず、毎完了 Frame に進行情報と統計を取得した。診断取得の負荷を含む各構成 1 回の観測であり、性能改善率や他環境での範囲を保証する比較ではない

| 構成 | 瞬間 FPS の範囲 | 120 区間の平均 FPS の範囲 | 完了間隔の平均 / p95 / 最大 |
| --- | --- | --- | --- |
| Debug | 36.19〜431.67 | 59.62〜60.93 | 16.670 / 18.419 / 27.632 ms |
| Development | 57.62〜62.71 | 59.97〜60.15 | 16.670 / 16.823 / 17.355 ms |

開始制限が働いていても UI / Render の所要時間や Worker の実行遅延で完了時刻は変動する。Debug では長い Render の次に短い完了間隔も観測した。平均進行頻度は 60 FPS 付近だったが、実際の完了間隔の揺れは残っている。CPU の Render 完了と OS が画面へ表示した時刻は異なり、表示側の Frame Pacing は今回測っていない

主表示を最大 120 区間の平均 FPS へ変更し、同じ Snapshot の `Frame interval` の平均・p95・最大を追加した。制御の変更や Frame Pacing の高速化は行っていない。元の CSV と診断 Source は `out/validation/FpsJitter/` に保存した

`scripts/codex_build.ps1` の Debug Build、Development の CueEditorHost / CueEditorHostTests Build が成功した。Debug の FrameController / EditorHost Lifecycle 2 Test、Development の EditorHost Lifecycle 1 Test が成功した。単一 Thread / Worker の既定 UI 描画と停止を確認した。Release の再 Build、修正後の UI 文字列の目視確認、長時間 / 高負荷 / 入力から表示までの計測は未実施
