# RotateCube

M11 の Sample Project。最終目標は、Editor の手動操作で Scene 作成・Cube 配置・Script 設定・保存を行い、
Editor から Build / 起動した単独アプリで Cube の回転を継続できること。
自動 Test に加え、実際の手動制作経路も完走して確認する。
詳細は [目標と手動完了条件](../../Docs/Research/M11-RotateCube-Goal-And-Manual-Workflow.md) を参照する。
現在は Scene の編集・保存、Script の割当と別 Process Play、静的リンクした製品アプリの生成を実装している。
保存済みの Cube には回転 Script を割り当てている。Editor の Preview は編集用、Play / 製品は実行用の World を持つ。

起動 Scene は `.cueproj` の `startupScene=Scenes/RotateCube.cuescene` で指定する。
Path は Content Root 基準。Editor は Main 所有の Preview World を作り、既定 Test UI に Scene 名と Object 数を表示する。
Scene 形式は [ADR-0012](../../Docs/Decisions/0012-scene-format-and-instantiation.md)、
Camera / Mesh の拡張と描画経路は [ADR-0014](../../Docs/Decisions/0014-camera-and-initial-scene-rendering.md) を参照する。

Repository Root で Build 後、以下のように Editor を起動する。

```powershell
& ./out/build/windows-vs2026/bin/Debug/CueEditorHost.exe --project ./Samples/RotateCube/RotateCube.cueproj
```

Visual Studio の生成 Project には、この Sample を開く固定のデバッグ起動引数を設定している。
他の CWD から起動する場合は `.cueproj` の絶対 Path を渡すか、その CWD からの相対 Path を渡す。
Editor のタイトルと既定 Test Window に Project 名を表示する。

Editor の `Build > Build Settings...` で Windows / x64 と Debug / Development / Release を選び、`Build` を押す。
未保存なら `Save and Build` / `Build Saved Files` / `Cancel` を選ぶ。
`Build Saved Files` は未保存編集を保持し、保存 File を配布する。起動対象は Panel に表示する Project の startupScene。
Build 中は `Cancel Build`、成功後は `Run` で製品アプリを起動できる。
製品を閉じるまで再 Build を抑止し、Editor を閉じても製品は独立して動作する。
Build の出力と結果は Panel 内の `Build Log` から確認する。

配布先は `Build/Windows/x64/<構成>/Player`。この Directory 一式をコピーする。
Tool の配置を変えた場合は Editor の生成時設定を更新する。
開発用ゲーム DLL の更新と Script Types Reload は製品 Build と別の操作で行う。

## Script の編集と Debug Play

`Script > Open Visual Studio` で、この Project 用の生成 Solution を開く。
生成先は `Intermediate/IDE/<Engine識別値>/Windows/x64`。CMake が生成し、生成 File は管理対象にしない。
Visual Studio と .NET Framework 4.8 Reference Assemblies が必要。
連携用 `VisualStudioBridge.exe` は Engine の MSBuild 時に生成して Editor 横へ配置する。
操作ごとの NuGet Restore / `dotnet run` は行わない。

Script の Source は `Source/RotateCube.cpp`、Editor 用の対応表は `Source/ScriptSources.cueide`。
Inspector で割当済み Script の `Edit Source` を押すと、`sample.RotateCube` に対応する File を開く。
Source を追加する場合は CMake の Source 定義と対応表も更新する。
Map の Path は Source Directory 基準で、絶対 Path や `..` は指定しない。

編集後は Play を停止して `Script > Build Scripts` を実行する。
DLL / PDB / Manifest の検証が成功したときだけ Script Types を Reload する。
DLL の構成は動作中 Editor に合わせるため、製品 Build Settings の構成とは別になる。
失敗は Editor の状態と診断で確認し、`Cancel Script Operation` で処理を取り消せる。

Breakpoint を使う場合は Visual Studio で Source に設定し、Editor の `Debug Play` を押す。
Editor は DLL を Build し、DLL 読込前に待機する Play 子へ Native Debugger を接続してから Script を開始する。
通常の `Play` は接続待機を行わない。`Stop` は所有 Play 子を停止する。
実 VS での Breakpoint 到達の検証状況は [検証記録](../../Docs/Research/M11-VisualStudio-Integration-Validation.md) を参照する。

Visual Studio から直接 Build する場合は、先に Play を停止して Editor 側の Build / Script 操作の完了を待つ。
外部 IDE の Build は Editor の Project Lock を取得せず、同時実行と自動 Reload は保証しない。
連携の責務と制限は [ADR-0020](../../Docs/Decisions/0020-editor-ide-and-debug-play.md) を参照する。

`.cueproj` の形式と所有権は [ADR-0009](../../Docs/Decisions/0009-project-loading.md) を参照する。
この Sample を移動しても `projectId` は維持する。別 Project として複製するときは新しい ID を割り当てる。
