# FrameController FPS 表示（M06-01）

Milestone: [M06 Performance Tests](https://github.com/tochouseito/CueEngine/milestone/7)
Issue: [#87](https://github.com/tochouseito/CueEngine/issues/87)

## 表示と計測点

既定 EditorHost の Test Window に TEST と `FrameController FPS: <数値>` を ImGui Text で表示する。UI は Main Thread で構築し、`WindowsHost::frame_progress()` が返す FrameController の同期済み Snapshot を利用する。独自 `EditorHostConfig::buildUi` を指定した場合は、その Callback が UI を構築する

FPS は `FrameProgress::lastFrameInterval` の秒数の逆数。Render Callback（WindowsHost では記録・提出・Present）と FPS 上限待機を終えた二つの完了点の間隔を使う。起動直後など二つの完了点が揃わない場合、または間隔が非正の場合は `FrameController FPS: --` と表示し、零除算しない

表示は小数 1 桁の直近 Frame の値であり、平均値や GPU 完了数ではない。ImGui IO.Framerate とモニター Refresh Rate は使わない。GPU 処理は Submit 後も続くため、この値から GPU 負荷を断定しない。最小化中は新規 Frame と UI 構築を停止する。復帰直後の値には休止時間が含まれることがあり、次の Render 完了で通常の間隔へ更新される

## 確認手順

- 既定 Debug Build: `pwsh -NoProfile -File scripts/codex_build.ps1`
- 全 Test: `ctest --preset windows-vs2026-debug --output-on-failure`
- Visual Studio: 生成済み `out/build/windows-vs2026/CueEngine.slnx` の CueEditorHost / Debug x64 を起動する
- 既定の FPS 上限は 60。動作環境や処理負荷によって値は変動する
- Resize / 最小化 / 復帰では Runtime と Worker を維持して UI を再開する

## 検証結果

2026-10-07 に Debug Build と全 38 Test が成功した。実画面確認は Computer Use のユーザー停止により未完了。速度改善は行っていないため、比較対象を伴う性能改善の主張はしない
