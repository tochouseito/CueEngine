# FrameController FPS 表示（M06-01）

Milestone: [M06 Performance Tests](https://github.com/tochouseito/CueEngine/milestone/7)
Issue: [#87](https://github.com/tochouseito/CueEngine/issues/87)

## 表示と計測点

既定 EditorHost の Test Window に TEST と `FrameController FPS: <数値>` を ImGui Text で表示する。UI は Main Thread で構築し、`WindowsHost::frame_progress()` が返す FrameController の同期済み Snapshot を利用する。独自 `EditorHostConfig::buildUi` を指定した場合は、その Callback が UI を構築する

FPS は `FrameProgress::lastFrameInterval` の秒数の逆数。Render Callback（WindowsHost では記録・提出・Present）と FPS 上限待機を終えた二つの完了点の間隔を使う。起動直後など二つの完了点が揃わない場合、または間隔が非正の場合は `FrameController FPS: --` と表示し、零除算しない

表示は小数 1 桁の直近 Frame の値であり、平均値や GPU 完了数ではない。ImGui IO.Framerate とモニター Refresh Rate は使わない。GPU 処理は Submit 後も続くため、この値から GPU 負荷を断定しない。最小化中は新規 Frame と UI 構築を停止する。復帰直後の値には休止時間が含まれることがあり、次の Render 完了で通常の間隔へ更新される

## FPS 待機と区間別計測（M06-02 / M06-14）

[#88](https://github.com/tochouseito/CueEngine/issues/88) は旧 CueEngine の高精度 Timer と短い Spin を採用する。Windows の `Waiter::sleep_for` は Thread ごとの Waitable Timer と停止 Event を所有し、Main／Update／Render の期限を混線させない。高精度 Timer 非対応時は通常 Timer、生成失敗時は Condition Variable へ戻る。停止要求で Timer 待機を解除する。通知待機は従来の世代付き通知を維持する

FrameController は Frame 間隔の大半を Sleep し、最後の `clamp(interval / 8, 250 us, 1 ms)` を時計と `Waiter::relax` で合わせる。2 ms 以下の Frame 間隔は Spin のみ。上限 0 は待機しない。遅れた Frame は現在の完了時刻から次回期限を計算し、遅れを取り戻す連続投入は行わない。通常 FPS でも短い Spin の CPU 使用があるため、FPS と電力・CPU 使用率は別に評価する

[#100](https://github.com/tochouseito/CueEngine/issues/100) の `FrameController::timing_info()` は Main／Update／Render Callback、FPS 待機、完了間隔を独立に返す。直近 120 件の固定容量 Sample に対して直近値・整数平均・nearest-rank p95・最大を返す。共有 Lock 中に Sample をコピーし、集計は Lock 外で行う。Render Callback 時間へ FPS 待機を混ぜない。WindowsHost の Render には Graph 記録・GPU 依存待機・Present が含まれ、これらの内訳を合計して Render に加算しない

## 確認手順

- 既定 Debug Build: `pwsh -NoProfile -File scripts/codex_build.ps1`
- 全 Test: `ctest --preset windows-vs2026-debug --output-on-failure`
- Visual Studio: 生成済み `out/build/windows-vs2026/CueEngine.slnx` の CueEditorHost / Debug x64 を起動する
- 既定の FPS 上限は 60。動作環境や処理負荷によって値は変動する
- Resize / 最小化 / 復帰では Runtime と Worker を維持して UI を再開する

## M06-01 時点の検証結果

2026-10-07 に Debug Build と全 38 Test が成功した。実画面確認は Computer Use のユーザー停止により未完了。速度改善は行っていないため、比較対象を伴う性能改善の主張はしない

M06-02 以降の待機改善、構成別検証と比較数値は [M06Performance.md](M06Performance.md) に記録する
