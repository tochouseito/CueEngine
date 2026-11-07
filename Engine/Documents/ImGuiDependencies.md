# Dear ImGui 依存導入（M05-03）

Issue: [#79](https://github.com/tochouseito/CueEngine/issues/79)

## 固定する依存

`ThirdParty/vcpkg.json` の builtin baseline は `19780d9cdf84d0944cf9a318666703b89ab6629c`。
imgui Port を `1.92.9` に固定し、`docking-experimental`、`win32-binding`、`dx12-binding` を指定する。
この Port が取得する上流 Source は `v1.92.9b-docking`。Core、Demo と公式 Backend は同じ `imgui::imgui` Library に含まれるため、Backend Source を別途 Compile しない。

`Cue.EditorHost` だけが ImGui を PRIVATE に利用する。Editor の公開 Header、Runtime、Renderer、`CueWindowsHost` に ImGui を公開しない。vcpkg の `x64-windows` Triplet は MSVC の DLL Runtime に合わせる。Debug は Debug Library、Development と Release は Release Library を利用する。

## 準備と Build

Visual Studio 2026 の C++ Workload に同梱された vcpkg、または別途用意した vcpkg を利用する。Script は `VCPKG_ROOT` を優先し、未設定なら `vswhere` で最新の C++ Workload の vcpkg を解決する。

```powershell
pwsh -NoProfile -File scripts/prepare_dependencies.ps1
pwsh -NoProfile -File scripts/codex_build.ps1
ctest --preset windows-vs2026-debug --output-on-failure
```

別の vcpkg を使う場合は事前に `$env:VCPKG_ROOT = 'C:/tools/vcpkg'` のように実際の配置を指定する。取得先は `ThirdParty/vcpkg_installed/`、Download／Binary Cache は `out/vcpkg/`。どちらも Git 管理しない。通常 Build は `VCPKG_MANIFEST_INSTALL=OFF` とし、依存の不足や更新があれば準備 Script を再実行する。

既存の Build Tree に初めて Toolchain を導入する場合、または `VCPKG_ROOT` を変更する場合は、Configure を一度やり直す。直接 CMake を使う場合、Visual Studio の CMake Project を開く場合も、親 Process に `VCPKG_ROOT` を設定する。

```powershell
$env:VCPKG_ROOT = 'C:/tools/vcpkg' # 使用する実際の配置に置き換える
cmake --fresh --preset windows-vs2026
cmake --build --preset windows-vs2026-debug
```

日常 Build Script の `-Configuration Development`／`Release` と同名の CTest Preset で各構成を確認できる。

## 到達範囲と検証

`Cue.Editor.ImGuiDependency` は Version、Docking API、Demo の CPU 描画 Data 生成、公式 Win32／DX12 Backend 関数の Link を確認する。#80 で UI Context と公式 Win32 Backend、#81 で公式 DX12 Backend の起動と記録 Adapter を EditorHost に接続した。実画素と資源回収は [ImGuiDX12Backend](ImGuiDX12Backend.md) の Test で検証する。

Host の表示 Pass 注入（#78）、ImGuiManager と Win32 入力（#80）、DX12 Backend／GPU 資源（#81）、ImGuiPass と Test UI 表示（#82）を接続した。今回の UI 指定は Test / TEST の Window 一つ。DockSpace は未接続で、Multi-Viewport は無効のままとする。

License と再配布条件は [THIRD_PARTY_NOTICES](../../ThirdParty/THIRD_PARTY_NOTICES.md) に記録する。

## 検証記録（2026-10-05）

- Windows x64、Visual Studio 2026、CMake 4.2.3、Visual Studio 同梱 vcpkg で Manifest の初回取得に成功
- `scripts/codex_build.ps1` の Debug／Development／Release Build に成功
- 各構成の CTest は 32/32 成功。`Cue.Editor.ImGuiDependency` と既存 Host の起動・停止検証を含む
- 生成された Build 定義で Debug は `debug/lib/imguid.lib`、Development／Release は `lib/imgui.lib` を選択することを確認
- `CueWindowsHost` と `Cue.Renderer.DX12` の生成された Build 定義には ImGui の Link がないことを確認
- 継承環境の Path／PATH 重複で最初の Compiler 検出が失敗したため、環境変数を整理した子 Process で Configure と Build Script を再実行
- 画面上の Docking 操作、GPU Backend の起動、Multi-Viewport は未接続であり、この依存導入 Test では検証しない
