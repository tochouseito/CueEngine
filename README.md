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
