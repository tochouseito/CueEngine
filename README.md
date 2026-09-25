# CueEngine

新CueEngineの最小Build基盤とWindows Host。`CueWindowsHost` はWindows用の起動・終了基盤。共通`Runtime`がFrame進行を担当する。Editor、Graphics、Runtime Worldはまだ含まない。Build定義はCMakeを正本とする。

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
& 'out/build/windows-vs2026/bin/Debug/CueWindowsHost.exe'
```

DevelopmentとReleaseは、Build/Test Preset名末尾の`debug`をそれぞれ`development`、`release`に置き換える。実行ファイルは`out/build/windows-vs2026/bin/<Configuration>/`に生成される。`CueWindowsHost` は1280×720のClient Areaを持つWindowを開き、右上の閉じるボタンで終了する。CTestはFoundation、Windows UTF変換、Platform公開Header、Win32 Lifecycle、別ProcessでのWindow表示とCloseを確認する。

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

以下はM00完了当時の記録。Console Smoke Targetは現在の構成から削除した。

| 構成 | Configure | Build | CTest | 実行 |
| --- | --- | --- | --- | --- |
| Debug | 成功 | 成功 | 1/1成功 | `CueEngine smoke OK` |
| Development | 同じ複数構成のBuild Treeを使用 | 成功 | 1/1成功 | `CueEngine smoke OK` |
| Release | 同じ複数構成のBuild Treeを使用 | 成功 | 1/1成功 | `CueEngine smoke OK` |

CIと製品Packageの経路はまだ用意していない。CIを追加するときは同じPresetを使い、対応するVisual Studio 2026環境で検証する。

## M01 Window到達範囲

M01ではWindowSystemとWindowの所有、Close要求後の明示的な破棄を検証した。現在は`CueWindowsHost`が同じ経路を使う。描画、Swap Chain、ImGui、複数Windowは未実装。Platformの公開HeaderはWin32型を公開せず、Windows実装は`Engine/Source/Platform/Windows/`に置く。UTF-8／Windows UTF-16の相互変換は`Engine/Source/Foundation/Windows/`に集約する。

Windows x64、CMake 4.2.3、Visual Studio 2026、Windows SDK 10.0.26100.0で、M01追加後のBuildとCTestを確認した。

| 構成 | Build | CTest |
| --- | --- | --- |
| Debug | 成功 | 6/6成功 |
| Development | 成功 | 6/6成功 |
| Release | 成功 | 6/6成功 |

## M02 Frame制御のダミー実行

M02ではHostの初期化時に`FrameController`へUpdate／RenderのCallbackを登録し、MainThreadから`step()`を呼ぶ構成を検証した。当時の`CueWindowHost`は両Callbackで各16ms待機するダミーを実行した。M03でWindows用の`CueWindowsHost`へ統合したため、このダミー実行Targetは削除した。`FrameController`のFrame順序、Worker、FPS制御は専用Testで引き続き検証する。

`FrameController`は既定でRender完了間隔を最大60 FPSに制限し、`maxFps=0`で制限を無効化できる。まだPresentがないため、これは旧EngineのPresent後の制御に対応する暫定的な同期点。処理負荷やOSの待機精度によって60 FPSを保証するものではない。

2026-09-24、Windows x64、CMake 4.2.3、Visual Studio 2026、Windows SDK 10.0.26100.0で確認した。当時のダミー実行Targetでは16ms待機とWindow Closeを検証した。Worker失敗時の伝播、Frame順序、指定20 FPSでのRender完了間隔40ms以上は`Cue.Runtime.FrameController` Testで確認した。次の表はM02完了時点の記録であり、現在のTest件数ではない。

| 構成 | Build | CTest |
| --- | --- | --- |
| Debug | 成功 | 10/10成功 |
| Development | 成功 | 10/10成功 |
| Release | 成功 | 10/10成功 |

## M03 WindowsHostと共通Runtimeの起動・終了基盤

`CueWindowsHost`の`WindowsHost`は`WindowSystem`、Window、Windows用の時間／Thread Service、共通`Runtime`を所有する。`Runtime`は注入されたServiceを借用して`FrameController`とWorkerを所有し、WindowやWin32型を持たない。MainThreadは`WindowsHost::step()`を呼び、Window Messageで終了要求がなければ内部で`Runtime::step()`へ進む。Close要求を受けた周回ではFrameを進めず、RuntimeのWorkerを停止・joinしてからWindowを破棄する。M03完了時点のUpdate／Render Callbackは空処理で、Runtime World、Renderer、GPU Submitは未接続だった。設計契約は[ADR-0004](Docs/Decisions/0004-runtime-host-lifecycle.md)を参照する。

```powershell
& 'out/build/windows-vs2026/bin/Debug/CueWindowsHost.exe'
```

自動終了、単一Thread、Render失敗の注入は`Engine/Tests/Platform/WindowsHostProcessTests.cpp`のTest専用子Processで行う。製品用`CueWindowsHost.exe`のMainと`WindowsHostDesc`にはTest専用引数を含めない。

2026-09-25、Windows x64、CMake 4.2.3、Visual Studio 2026、Windows SDK 10.0.26100.0で、WindowなしのRuntime起動、WindowsHostの起動・Close、自動終了、単一Thread、Callback失敗のTestを含めて確認した。Debugの`CueWindowsHost.exe`を実際に表示し、タイトルバーのCloseでWindowが消えることも確認した。

| 構成 | Build | CTest |
| --- | --- | --- |
| Debug | 成功 | 12/12成功 |
| Development | 成功 | 12/12成功 |
| Release | 成功 | 12/12成功 |

この表はConsole Smoke削除後の再検証結果。

`CueWindowHost`を削除した後もWindowsHostのProcess Test 4件は継続する。

## M04 最小Renderer

`Cue.Renderer.D3D12`がWindows用のSwap Chain、RTV、Command Allocator、Fenceを所有する。`WindowsHost`はRendererを初期化し、共通`Runtime`のRender Callbackから単色ClearとPresentを実行する。現在は`FrameController`の60 FPS制御を使い、`Present(0, 0)`で二重の待機を避ける。Rendererの所有境界は[ADR-0005](Docs/Decisions/0005-minimal-renderer-presentation.md)を参照する。
