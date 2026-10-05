# EditorHost

M05-01 / [Issue #77](https://github.com/tochouseito/CueEngine/issues/77) の Editor 起動基盤

## 構成と所有

`CueEditorHost` は Windows 用の Editor Executable、`Cue.EditorHost` は起動・Frame 進行・停止を扱う Library

`EditorHost` が `WindowsHost` を一意所有し、WindowsHost が WindowSystem、Window、Renderer Backend、FrameGraph、Thread Service、Runtime を所有する。Window Message、起動、停止、破棄は構築 Thread から行う。Runtime と Renderer は EditorHost に依存しない

EditorHost の既定設定は `CueEngine Editor` / 1280×720、Frame 枠数 2、`useWorkerThreads = false`、上限 60 FPS。UI Context を Window Message と同じ Thread で扱う初期構成とする。GPU の非同期実行と CPU Frame の Worker 利用は別の設定であり、単一 CPU Thread でも GPU 完了前に Resource を破棄しない

現在は既存の ClearFinalColor / PresentToSwapChain Graph を使う。EditorHost はまだ ImGui Context や Editor Document を生成しない

## 起動と停止

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1
& 'out/build/windows-vs2026/bin/Debug/CueEditorHost.exe'
```

Window の Close 要求で Loop を終え、Runtime 停止、Graph の GPU 完了待ち、Backend 停止、Window 破棄の順で終了する。初期化・実行・停止の失敗は Result と非 0 の終了 Code へ反映する。初期化は一度だけ、shutdown は複数回呼べる

```powershell
ctest --preset windows-vs2026-debug -R 'Cue.EditorHost' --output-on-failure
```

Lifecycle Test は初期化失敗後の停止、二重停止、停止後の操作拒否、Owner Thread 以外の操作拒否、既定構成による Frame の同一 Thread 実行を確認する。Process Test は実際の CueEditorHost.exe を起動し、Window 表示と WM_CLOSE 後の終了 Code 0 を確認する

## 後続の機能

| Issue | 機能 |
| --- | --- |
| [#78](https://github.com/tochouseito/CueEngine/issues/78) | Host から表示 Pass を注入 |
| [#80](https://github.com/tochouseito/CueEngine/issues/80) | ImGuiManager と Win32 入力 |
| [#81](https://github.com/tochouseito/CueEngine/issues/81) | 公式 DX12 Backend と GPU 資源 |
| [#82](https://github.com/tochouseito/CueEngine/issues/82) | ImGuiPass、Demo、描画 Texture 表示 |
| [#83](https://github.com/tochouseito/CueEngine/issues/83) | Render Thread への描画 Data 転送 |
| [#84](https://github.com/tochouseito/CueEngine/issues/84) | 導入検証と Completion Gate |

表示 Pass の選択と UI の所有は EditorHost 側に追加する。WindowsHost の接続入口には FrameGraphPass 等の抽象契約を渡し、GraphicsBackend から Editor の具体型を生成しない

Dear ImGui の Docking 版と公式 Win32／DX12 Backend は #79 で PRIVATE 依存として導入済み。準備と構成別 Library の検証は [ImGuiDependencies](ImGuiDependencies.md) を参照する

実際の GPU Resize は M04 の #22 / #59 の残作業。現在の WindowsHost は初期サイズと異なる間は描画を停止し、SwapChain / FinalColor を再生成しない。M05 の最終検証では Window 状態変更と GPU の描画復帰を分けて確認する

## 検証記録（2026-10-05）

- Windows x64 / Visual Studio 2026 の既定 Debug Build 成功
- `scripts/codex_build.ps1` は継承環境の Path/PATH 重複を除いた子 Process で実行
- `ctest --preset windows-vs2026-debug` は 31/31 成功。EditorHost の Lifecycle / Process の 2 件を含む
- `git diff --check` 成功。追加した C++ の if / else はすべて波括弧を使用
- Development / Release の Build と手動による連続操作は未実施。ImGui と GPU Resize の検証は後続 Issue
