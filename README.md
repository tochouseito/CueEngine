# CueEngine

新CueEngineの最小Build基盤とWindows Window Host。`CueEngine` はConsole Smoke、`CueWindowHost` は実Windowを開くTarget。Editor、Graphics、製品Runtimeはまだ含まない。Build定義はCMakeを正本とする。

## 開発環境

- Windows x64
- Visual Studio 2026の「Desktop development with C++」WorkloadとWindows SDK
- CMake 4.2.0以上（`Visual Studio 18 2026` Generator）
- PowerShell 7（`scripts/codex_build.ps1`を使う場合）

NuGet restoreと第三者Libraryの取得は不要。Repository RootをVisual StudioのCMake Projectとして開くか、以下をPowerShellで実行する。

## Configure、Build、Test、実行

```powershell
cmake --preset windows-vs2026
cmake --build --preset windows-vs2026-debug
ctest --preset windows-vs2026-debug --output-on-failure
& 'out/build/windows-vs2026/bin/Debug/CueEngine.exe'
& 'out/build/windows-vs2026/bin/Debug/CueWindowHost.exe'
```

DevelopmentとReleaseは、Build/Test Preset名末尾の`debug`をそれぞれ`development`、`release`に置き換える。実行ファイルは`out/build/windows-vs2026/bin/<Configuration>/`に生成される。`CueWindowHost` は1280×720のClient Areaを持つWindowを開き、右上の閉じるボタンで終了する。CTestはConsole Smoke、Foundation、Windows UTF変換、Platform公開Header、Win32 Lifecycle、別ProcessでのWindow表示とCloseを確認する。

日常のDebug Buildは次のScriptでも実行できる。`-Configuration Development`または`Release`も指定できる。

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1
```

Build Tree、生成されたVisual Studio Project、Test Logは`out/build/windows-vs2026/`以下に置き、Git管理しない。Testは通常BuildのTargetに含まれるが、Buildだけでは自動実行されない。

## 構成と配置

| 構成 | 用途 |
| --- | --- |
| Debug | 診断用。Debug Runtime、最適化なし、Assert有効 |
| Development | 開発時の動作確認。Release Runtime、最適化、Debug Symbol、Assert有効 |
| Release | 製品条件の検証。Release Runtime、最適化、`CUE_SHIPPING=1`、Assert無効 |

First-party Sourceは`Engine/Source/`、CTest登録は`Engine/Tests/`、Coding Rulesは`Engine/Documents/`、設計決定は`Docs/Decisions/`に置く。第三者Libraryはまだ導入していない。導入時の`ThirdParty/`配置と依存取得方法は[ADR-0002](Docs/Decisions/0002-build-system-source-of-truth.md)に従う。

この基盤の設計境界は[ADR-0001](Docs/Decisions/0001-architecture-boundaries.md)、記述規約は[CODING_RULES.md](Engine/Documents/CODING_RULES.md)を参照する。

## M00検証記録（2026-09-24）

Windows x64、CMake 4.2.3、Visual Studio 2026（MSVC 19.51.36260.0）、Windows SDK 10.0.26100.0で確認した。

| 構成 | Configure | Build | CTest | 実行 |
| --- | --- | --- | --- | --- |
| Debug | 成功 | 成功 | 1/1成功 | `CueEngine smoke OK` |
| Development | 同じ複数構成のBuild Treeを使用 | 成功 | 1/1成功 | `CueEngine smoke OK` |
| Release | 同じ複数構成のBuild Treeを使用 | 成功 | 1/1成功 | `CueEngine smoke OK` |

CIと製品Packageの経路はまだ用意していない。CIを追加するときは同じPresetを使い、対応するVisual Studio 2026環境で検証する。

## M01 Window到達範囲

`CueWindowHost` がWindowSystemとWindowを所有し、Close要求を受けてWindowを明示的に破棄する。描画、Swap Chain、ImGui、複数Windowは未実装。Platformの公開HeaderはWin32型を公開せず、Windows実装は`Engine/Source/Platform/Windows/`に置く。UTF-8／Windows UTF-16の相互変換は`Engine/Source/Foundation/Windows/`に集約する。

Windows x64、CMake 4.2.3、Visual Studio 2026、Windows SDK 10.0.26100.0で、M01追加後のBuildとCTestを確認した。

| 構成 | Build | CTest |
| --- | --- | --- |
| Debug | 成功 | 6/6成功 |
| Development | 成功 | 6/6成功 |
| Release | 成功 | 6/6成功 |

## M02 Frame制御のダミー実行

Hostの初期化時に`FrameController`へUpdate／RenderのCallbackを登録する。`Main.cpp`はWindow Messageを処理し、終了要求がなければ`FrameController::step()`を直接呼ぶ。終了要求を受けた周回ではFrameを進めず、Workerを停止・joinしてからWindowとSystemを破棄する。UpdateとRenderはそれぞれ別のWorker Threadで各Frameに16ms待機するダミー。Runtime World、Renderer、GPU Submitは接続していない。Frameは最大2件まで先行し、同じFrameのUpdate完了後にRenderを開始する。

DebuggerにはFrame数、直近のUpdate／Render経過時間、Render完了間隔から求めたFPS、Thread識別子を約60Frameごとに出力する。`FrameController`は既定でRender完了間隔を最大60 FPSに制限し、`maxFps=0`で制限を無効化できる。まだPresentがないため、これは旧EngineのPresent後の制御に対応する暫定的な同期点。処理負荷やOSの待機精度によって60 FPSを保証するものではない。`CueWindowHost.exe --test-frames=4`は4Frameのダミー処理完了後に自動終了するTest用入口。`--single-thread`を追加すると、同じWindow Hostで両CallbackをMainThreadから順に実行する。

```powershell
& 'out/build/windows-vs2026/bin/Debug/CueWindowHost.exe'
& 'out/build/windows-vs2026/bin/Debug/CueWindowHost.exe' --test-frames=4
& 'out/build/windows-vs2026/bin/Debug/CueWindowHost.exe' --test-frames=4 --single-thread
```

2026-09-24、Windows x64、CMake 4.2.3、Visual Studio 2026、Windows SDK 10.0.26100.0で確認した。自動終了Testは4Frame後にWindowとProcessが終了することと、直近のUpdate／Render待機を各15ms以上観測したことを検証する。Window Close Testは別Processへ`WM_CLOSE`を送り、Workerをjoinした後に正常終了する経路を検証する。Worker失敗時の伝播、Frame順序、指定20 FPSでのRender完了間隔40ms以上は`Cue.Runtime.FrameController` Testで確認する。

| 構成 | Build | CTest |
| --- | --- | --- |
| Debug | 成功 | 10/10成功 |
| Development | 成功 | 10/10成功 |
| Release | 成功 | 10/10成功 |
