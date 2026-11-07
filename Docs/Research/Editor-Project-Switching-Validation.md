# Editor の Project 再選択

- 日付: 2026-11-05
- 契約: [ADR-0022](../Decisions/0022-editor-project-switching.md)
- 作業 Branch: `m11/issue146/project-loading`
- 既存の未コミット変更を保全して実装。Commit / Push / Merge は実施していない

## 実装

`File > Open Project...` / `Ctrl+Shift+O` から Folder を選び、直下の `.cueproj` を開く。
引数なし起動は Project 未選択画面を表示し、Scene 編集・Script・Build・Play を抑止する。
`--project` を明示した Hub 起動は維持し、不正な明示引数は起動前に拒否する。

現在の Project / Scene / Preview / Script 接続は、候補の検証と保存確認を終えてから切り替える。
UI の Action を処理中に Document を破棄せず、次の `step()` 境界で公開する。
Window / Renderer / Frame ID は維持し、Render は所有 Snapshot だけを使う。
旧 Build の成功結果・起動先・ログは `reset_project()` で失効させる。
新 Project の Build 済み型情報は、起動時と同じ Metadata 取得経路で読み直す。

同じ Project の「保存して開く」は、保存成功後に候補を再読込する。
保存前に準備した Scene を採用して、保存した編集内容を画面上だけ巻き戻さない。
保存失敗・取消・不正入力では旧 Project と Document を保持する。

## 検証

- 規定の `pwsh -NoProfile -File scripts/codex_build.ps1`: Debug 成功
- `Cue.Editor.ProjectSwitch`: Single / Worker の両方で成功
- `Cue.Editor.Ui`: 未選択 UI と Shortcut の抑止 / 開く要求が成功
- 全構成の生成 `.vcxproj`: `LocalDebuggerCommandArguments` が空であることを確認
- Development Editor: Build 成功、引数なし継続動作・正常終了成功
- Release Editor: Build 成功、引数なし継続動作・正常終了成功
- 全 Debug CTest: 同時 Build のない再確認で 126 / 126 成功（138.13 秒）

最初の全体確認は 125 件成功、既存 `Cue.WindowsHost.Lifecycle` が 10 秒制限で時間切れだった。
他の構成の Build を終えてから単独実行は 8.20 秒で成功し、続く全件実行も成功した。
無関係な既存 Test の制限時間や製品処理は変更していない。

主な記録:

| File | 内容 |
| --- | --- |
| `out/validation/EditorProjectSwitchVerifiedBuild.log` | 最終 Debug Build |
| `out/validation/EditorProjectSwitchFinalTargetBuild.log` | 型 Alias 命名を整えた後の Editor 再 Build |
| `out/validation/EditorProjectSwitchFinalTargetTests.log` | 最終 Library での Project 切替 Test |
| `out/validation/EditorProjectSwitchTargetTests.log` | Project 切替と UI の対象 Test |
| `out/validation/EditorProjectSwitchAllTests.log` | 全 Debug CTest |
| `out/validation/EditorProjectSwitchIdleAllTests.log` | 同時 Build のない全 Debug CTest、126 件成功 |
| `out/validation/EditorProjectSwitchLifecycleRetry.log` | 既存 Host Test の単独再確認 |
| `out/validation/EditorProjectSwitchDevelopmentBuild.log` | Development Editor Build |
| `out/validation/EditorProjectSwitchDevelopmentRun.log` | Development 引数なし起動・終了 |
| `out/validation/EditorProjectSwitchReleaseBuild.log` | Release Editor Build |
| `out/validation/EditorProjectSwitchReleaseRun.log` | Release 引数なし起動・終了 |

初回の Build で生成 Header の Include Path、Path 値の比較と Test の直接依存を修正した。
成功記録は上記の最終 Build と区別する。

### 自動テストの内容

`EditorProjectSwitchTests.cpp` は一つの Host で未選択 → A → B → A、同じ A の File 更新再読込、
Cube / Camera の描画 Snapshot、保存 / 破棄 / 取消、保存失敗、読込失敗、不正 Scene、
Engine Version 不一致、別 Thread、Build / 製品アプリ稼働中の拒否、旧ゲーム起動の抑止を確認する。
Single / Worker の両方で同じ契約を検証する。

WindowsServices Test は Folder 内の 0 / 1 / 複数候補と通常 File 条件を確認する。
GameBuildService Test は reset の稼働中 / 別 Thread / 停止後の拒否と、別 Project での再利用を確認する。
Process Test は引数なしの通常起動と、値のない明示 `--project` の拒否を分けて検証する。

### 実画面

Computer Use で実 Editor の File Menu、Windows Folder Dialog と読込結果を確認した。
PID `48728` / Window Handle `3674704` のまま、未選択 → `Samples/RotateCube` →
専用検証 Root の `Empty3D` へ切り替わった。
タイトルは `CueEngine Editor` → `CueEngine Editor - RotateCube` → `CueEngine Editor - Empty Scene`。
RotateCube の描画と Empty3D の Camera 一つの表示を確認し、最後に正常終了（Exit 0）した。
Sample の Scene / Project は編集・保存していない。
未 Build の Empty3D は Build 不足を通知し、編集画面の動作を継続した。

## 境界と未実施

- 対応するのは現在の Engine Version の Project と、互換な v1 Project。
- Play / Metadata 取得 / Build / 製品アプリ / IDE の稼働中は切替を要求できない。
- Folder の直下に複数の `.cueproj` がある場合は、候補を推測せずエラーにする。
- 実 Visual Studio の F5 / Breakpoint 接続操作は未実施。全構成の生成起動設定と実行 File を検証する。
- 配布パッケージの再生成と別 PC での導入検証は今回の対象外。
- 将来 Project 固有 GPU Asset を導入するときは、退役と世代管理を別途追加する。現在は共通 Built-in Mesh を保持する。
