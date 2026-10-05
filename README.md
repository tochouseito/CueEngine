# CueEngine

新CueEngineのBuild基盤とWindows用の表示・Frame実行基盤。`CueWindowsHost`はWindow、DX12 Backend、FrameGraphとRuntimeの生成、Message処理、描画とPresent、終了を担当する。`CueEditorHost`は同じWindows基盤を利用するEditor用の起動入口で、現在は既存Graphの描画まで接続している。ImGuiのDocking版は依存導入済みで、UIの起動・描画とRuntime Worldは未接続。Build定義はCMakeを正本とする。後半のM00〜M04の記録は各時点の履歴であり、現在の到達範囲はSourceと個別の機能Documentを参照する。

## 開発環境

- Windows x64
- Visual Studio 2026の「Desktop development with C++」WorkloadとWindows SDK
- CMake 4.2.0以上（`Visual Studio 18 2026` Generator）
- PowerShell 7（`scripts/codex_build.ps1`を使う場合）

初回は `pwsh -NoProfile -File scripts/prepare_dependencies.ps1` で Dear ImGui の Docking 版を取得する。通常 Build 中は依存を取得しない。vcpkg の配置と直接 CMake／Visual Studio を使う場合の `VCPKG_ROOT` 設定は [ImGui 依存導入](Engine/Documents/ImGuiDependencies.md) を参照する。NuGet restore は不要。

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

## EditorHost（M05）

```powershell
& 'out/build/windows-vs2026/bin/Debug/CueEditorHost.exe'
ctest --preset windows-vs2026-debug -R 'Cue.EditorHost' --output-on-failure
```

`CueEditorHost`は`CueEngine Editor`のWindowを表示し、Closeで終了する。初期構成はCPUのUpdate／RenderをMainThreadで実行する。Hostは抽象型の表示PassをGraphの最後へ注入でき、未指定時は既存の全画面表示を使う。ImGuiのDocking版、Demo、公式Win32／DX12 BackendはEditorのPRIVATE依存として接続済み。UIの起動・描画は後続Issueで行う。依存取得は[ImGui 依存導入](Engine/Documents/ImGuiDependencies.md)、所有、表示Pass注入、起動・停止、未対応のResizeと機能別Issueは[EditorHost](Engine/Documents/EditorHost.md)を参照する。

## 構成と配置

| 構成 | 用途 |
| --- | --- |
| Debug | 診断用。Debug Runtime、最適化なし、Assert有効 |
| Development | 開発時の動作確認。Release Runtime、最適化、Debug Symbol、Assert有効 |
| Release | 製品条件の検証。Release Runtime、最適化、`CUE_SHIPPING=1`、Assert無効 |

First-party Sourceは`Engine/Source/`、CTest登録は`Engine/Tests/`、Coding Rulesは`Engine/Documents/`、設計決定は`Docs/Decisions/`に置く。各Moduleの公開Headerは`Public/<Module名>/`に置き、`Cue/`を含めずにIncludeする。第三者LibraryのManifestとLicenseは`ThirdParty/`で管理し、配置と依存取得方法は[ADR-0002](Docs/Decisions/0002-build-system-source-of-truth.md)に従う。

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

以下はM03完了当時の構成であり、現在のWindowsHostはWindowだけを所有する。`CueWindowsHost`の`WindowsHost`は`WindowSystem`、Window、Windows用の時間／Thread Service、共通`Runtime`を所有する。`Runtime`は注入されたServiceを借用して`FrameController`とWorkerを所有し、WindowやWin32型を持たない。MainThreadは`WindowsHost::step()`を呼び、Window Messageで終了要求がなければ内部で`Runtime::step()`へ進む。Close要求を受けた周回ではFrameを進めず、RuntimeのWorkerを停止・joinしてからWindowを破棄する。M03完了時点のUpdate／Render Callbackは空処理で、Runtime World、Renderer、GPU Submitは未接続だった。設計契約は[ADR-0004](Docs/Decisions/0004-runtime-host-lifecycle.md)を参照する。

```powershell
& 'out/build/windows-vs2026/bin/Debug/CueWindowsHost.exe'
```

当時の自動終了、単一Thread、Render失敗の注入にはTest専用子Processを使用した。現在のProcess Testは製品Windowの表示、状態変更、Closeを確認する。

2026-09-25、Windows x64、CMake 4.2.3、Visual Studio 2026、Windows SDK 10.0.26100.0で、WindowなしのRuntime起動、WindowsHostの起動・Close、自動終了、単一Thread、Callback失敗のTestを含めて確認した。Debugの`CueWindowsHost.exe`を実際に表示し、タイトルバーのCloseでWindowが消えることも確認した。

| 構成 | Build | CTest |
| --- | --- | --- |
| Debug | 成功 | 12/12成功 |
| Development | 成功 | 12/12成功 |
| Release | 成功 | 12/12成功 |

この表はConsole Smoke削除後の再検証結果。

上記のTest件数と構成はM03当時の記録。

## M04 最小Renderer

M04では`Cue.Renderer.DX12`と`DX12Backend`によるSwap Chain、RTV、Command Allocator、Fenceの実装を検証した。この構成は段階的な再設計のためSourceとBuildから削除した。当時の設計記録は[ADR-0005](Docs/Decisions/0005-minimal-renderer-presentation.md)を参照する。

## Renderer再構築

`Cue.Renderer.RHI`はPlatform型を含まない`IRenderDevice`契約を公開する。`Cue.Renderer.DX12`はこれを実装し、DXGI Factory、Adapter、D3D12 Deviceを所有する`DX12RenderDevice`を提供する。Hardware Adapterを高性能順で試し、対応するDeviceがなければWARPを使用する。検証用にWARPを明示選択することもできる。Debug構成では利用可能なDebug Layer、GPU Validation、DREDとInfoQueueを設定する。生成失敗はHRESULTを含む`Result`で返し、診断用の名前付け失敗はWarningとして扱う。

`Cue.Renderer.RHI`の`create_backend()`は現在のWindows用実装として`DX12Backend`を生成する。Backendの生成中に`DX12RenderDevice::create()`を呼び、成功したDeviceを一意所有する。`CueWindowsHost.exe`はWindow生成後にBackendを作り、終了時はBackendを停止してからWindowを破棄する。Swap Chain、FrameGraph、Command／Queue Poolと描画処理は後続の移植対象となる。
