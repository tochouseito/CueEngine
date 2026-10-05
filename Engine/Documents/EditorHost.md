# EditorHost

M05-01 / [Issue #77](https://github.com/tochouseito/CueEngine/issues/77) の Editor 起動基盤、M05-02 / [Issue #78](https://github.com/tochouseito/CueEngine/issues/78) の表示 Pass 注入、M05-04 / [Issue #80](https://github.com/tochouseito/CueEngine/issues/80) の ImGuiManager と Win32 入力

## 構成と所有

`CueEditorHost` は Windows 用の Editor Executable、`Cue.EditorHost` は起動・Frame 進行・停止を扱う Library

`EditorHost` が `WindowsHost` と `ImGuiManager` を一意所有し、WindowsHost が WindowSystem、Window、Renderer Backend、FrameGraph、Thread Service、Runtime を所有する。Window Message、UI 構築、起動、停止、破棄は構築 Thread から行う。Runtime と Renderer は EditorHost に依存しない

EditorHost の既定設定は `CueEngine Editor` / 1280×720、Frame 枠数 2、`useWorkerThreads = false`、上限 60 FPS。UI Context を Window Message と同じ Thread で扱う初期構成とする。GPU の非同期実行と CPU Frame の Worker 利用は別の設定であり、単一 CPU Thread でも GPU 完了前に Resource を破棄しない

表示 Pass 未指定時は既存の ClearFinalColor / PresentToSwapChain Graph を使う。ImGui Context と Win32 Backend は Window 生成後に起動し、CPU 描画 Data を生成する。ImGui の GPU 描画と Editor Document はまだ接続しない

## UI Frame と入力

`EditorHostConfig::imgui` は Layout 保存先、Font Size と Docking を指定し、`buildUi` は ImGui API を呼ぶ UI 構築 Callback を指定する。Callback は Manager の Context が Current の構築 Thread 上で実行し、失敗を `Result<void>` で返す

WindowsHost に注入した汎用 Callback で Window 生成後の UI 初期化、Runtime の Update 上での UI 構築、Graph 破棄後の UI 停止を行う。Loop の `step()` 回数ではなく、採用された Update Frame ごとに NewFrame → UI Callback → Render を一度だけ呼ぶ。`useWorkerThreads = true` は #83 の Data 転送実装まで InvalidArgument で拒否する

Window は一つの外部 Message Handler を保持し、公式 Win32 Backend に Mouse / Keyboard / Unicode 文字 / Focus を配送する。UI が Message を処理しても Close / Resize / Destroy の必須処理は実行する。Capture Flag は Gameplay 入力の抑制用であり、ImGui への配送を止める条件にはしない。詳細は [ImGuiManager](ImGuiManager.md) を参照する

## Host からの表示 Pass 注入

`EditorHostConfig::graph` と `WindowsHostConfig::graph` は Backend 非依存の `MainFrameGraphConfig` を受け取る。`configure` で追加の描画 Pass を登録し、`displayPass` に Host が生成した `unique_ptr<FrameGraphPass>` を渡す。設定は move して Host を構築する

所有権は EditorHost → WindowsHost → DX12MainFrameGraph → FrameGraph と移り、Graph は ClearFinalColor、追加の描画 Pass、指定した表示 Pass の順に Build する。Pass 未指定時だけ PresentToSwapChainPass を生成する。Graph は一つのままで、SwapChain の Present は Graph 提出後に WindowsHost が呼ぶ

表示 Pass の具体型や名前は検証しない。最後に配置された Pass が指定した実体であること、Graphics Queue で BackBuffer を Write / RenderTarget と宣言していることを検証する。FinalColorTexture の Read / ShaderRead は表示 Pass が必要に応じて宣言する。BackBuffer の終了 State は Graph の終了 Barrier で Present に戻す

過去 CueEngine の `EngineSetupInfo::editorPass` と同じ抽象型の受け渡しを採用する。Editor が生成する ImGuiPass は後続 #82 でこの入口に接続する。Renderer / DX12 は Editor と ImGui の具体型を Include しない

Pass が ImGuiManager 等を非所有参照する場合、その Owner は Host の shutdown 完了まで生存させる。通常停止では Runtime の Callback 停止、GPU 完了待ち、Graph と Pass の破棄、ImGuiManager 停止、Backend 停止の順となる。初期化失敗時も注入 Pass と部分 UI 基盤を回収し、同じ Host を再初期化しない

## 起動と停止

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1
& 'out/build/windows-vs2026/bin/Debug/CueEditorHost.exe'
```

Window の Close 要求で Loop を終え、Runtime 停止、Graph の GPU 完了待ちと破棄、UI の Handler 解除と Context 破棄、Backend 停止、Window 破棄の順で終了する。初期化・実行・停止の失敗は Result と非 0 の終了 Code へ反映する。初期化は一度だけ、shutdown は複数回呼べる。上位の停止 Callback が失敗した場合は Backend / Window を保持して停止を再試行する。Destructor でも借用解除に失敗し続ける場合は、参照先を破棄せず terminate する

```powershell
ctest --preset windows-vs2026-debug -R 'Cue.EditorHost' --output-on-failure
```

Lifecycle Test は初期化失敗後の停止、二重停止、停止後の操作拒否、Owner Thread 以外の操作拒否、注入した表示 Pass による Frame の同一 Thread 実行と停止時の破棄を確認する。Window 生成前の設定失敗と Graph Build 失敗でも Pass を回収する。Process Test は実際の CueEditorHost.exe を起動し、Window 表示と WM_CLOSE 後の終了 Code 0 を確認する

`Cue.Renderer.FrameGraph.Passes` は Clear → 追加描画 → 注入表示の順序、所有権移動、抽象 Context の実行、Present への終了 Barrier、Callback / setup 失敗と表示先未宣言の回収を検証する。既存の DX12 MainFrameGraph Test は未指定時の標準表示と WARP の BackBuffer 画素を確認する

## 後続の機能

| Issue | 機能 |
| --- | --- |
| [#81](https://github.com/tochouseito/CueEngine/issues/81) | 公式 DX12 Backend と GPU 資源 |
| [#82](https://github.com/tochouseito/CueEngine/issues/82) | ImGuiPass、Demo、描画 Texture 表示 |
| [#83](https://github.com/tochouseito/CueEngine/issues/83) | Render Thread への描画 Data 転送 |
| [#84](https://github.com/tochouseito/CueEngine/issues/84) | 導入検証と Completion Gate |

ImGuiPass の生成は EditorHost 側に追加する。実装済みの表示 Pass 注入入口へ抽象型として渡し、GraphicsBackend から Editor の具体型を生成しない

Dear ImGui の Docking 版と公式 Win32／DX12 Backend は #79 で PRIVATE 依存として導入済み。準備と構成別 Library の検証は [ImGuiDependencies](ImGuiDependencies.md) を参照する

実際の GPU Resize は M04 の #22 / #59 の残作業。現在の WindowsHost は初期サイズと異なる間は描画を停止し、SwapChain / FinalColor を再生成しない。M05 の最終検証では Window 状態変更と GPU の描画復帰を分けて確認する

## M05-01 の検証記録（2026-10-05）

- Windows x64 / Visual Studio 2026 の既定 Debug Build 成功
- `scripts/codex_build.ps1` は継承環境の Path/PATH 重複を除いた子 Process で実行
- `ctest --preset windows-vs2026-debug` は 31/31 成功。EditorHost の Lifecycle / Process の 2 件を含む
- `git diff --check` 成功。追加した C++ の if / else はすべて波括弧を使用
- Development / Release の Build と手動による連続操作は未実施。ImGui と GPU Resize の検証は後続 Issue

## M05-02 の検証記録（2026-10-05）

- `scripts/codex_build.ps1` の最終 Debug Build 成功
- `ctest --preset windows-vs2026-debug --output-on-failure` は 32/32 成功。注入した Editor 表示 Pass の実描画、所有権移動、失敗時回収、既存 Standalone の表示と起動・停止を含む
- 下位層の生成 Project に ImGui の Link がないことを確認し、独立した読み取り Review でも所有と停止順の重大な問題は見つからなかった
- 初回 Build は ImGui 未配置で失敗し、環境変数の vcpkg は Version DB が古かった。Visual Studio 同梱 vcpkg で固定 Version を準備し、Toolchain を fresh Configure して検証した
- `git diff --check` 成功。Development / Release、実 ImGui UI の描画と入力は今回未検証。後続実装では ImGuiManager を借用する Pass より長く生存させる

## M05-04 の検証記録（2026-10-05）

- `scripts/codex_build.ps1` の既定 Debug Build 成功
- `ctest --preset windows-vs2026-debug --output-on-failure` は 34/34 成功。新しい Message Handler と ImGuiManager、既存 Editor / Standalone の実 Process 起動・Close を含む
- Context の復元、Unicode 入力 / Mouse / Keyboard / Focus、CPU 描画 Data と Capture、Layout の保存・再読込、読込 / 保存失敗回収、UI Callback の未完了 Stack 回復と次の Frame、Thread / 再入拒否を確認
- 上位停止 Callback の失敗時に Window を保持し、二度目の shutdown で回収できることを確認。読み取り Review の借用寿命と Stack 回復の指摘を修正し、再確認で追加の問題なし
- `git diff --check` 成功。Development / Release と手動の連続 UI 操作は未実施。ImGui GPU 描画、Worker Data 転送、日本語 Font は未接続
