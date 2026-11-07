# Hub / Engine Windows 配布依存の監査

- 確認日: 2026-11-04
- 対象: Windows x64 の Engine / Editor / SDK / Tools と、将来の .NET 10 WPF Hub
- ゲームの配布・更新は今回の対象外
- 実物と公式の配布条件を照合した記録であり、個別契約や地域の法的判断を代替しない

## 結果

予定している Engine / Editor / SDK / Hub の配布方式全体を禁止する条件は確認していない。
ただし Microsoft の Debug Runtime や開発 Tool を任意に詰め合わせて公開する権利はなく、
現在の Build 出力をそのまま ZIP にする方法は配布条件の検証を満たさない。

| 対象 | 条件 / 実装方針 |
| --- | --- |
| Microsoft Debug CRT | 一般向け配布に含めない。自分の別 PC における Debug / Test 用配置と区別する |
| Retail VC++ Runtime | 正規の再配布対象を License に従って利用する。対象 Architecture と必要 Version を検出する |
| DirectX Shader Compiler | 公式 Release の対になる DLL と License / Notice を Version / SHA-256 固定で利用する |
| CMake | BSD 3-Clause と内包第三者の License / Notice を保持する |
| MSVC / Windows SDK / Visual Studio | インストール先全体を同梱しない。利用者の導入済み Toolchain を検出する |
| .NET 10 WPF self-contained | 実際の Runtime Pack の License / Notice を含める。全体を MIT と表示しない |

## Debug CRT と公開する Editor

現在の `CMakeLists.txt` は Debug に `/MDd`、Development / Release に `/MD` を選ぶ。
`dumpbin /DEPENDENTS` で既存 Debug `CueEditorHost.exe` に次の依存を確認した。

- `MSVCP140D.dll`
- `MSVCP140D_ATOMIC_WAIT.dll`
- `VCRUNTIME140D.dll`
- `VCRUNTIME140_1D.dll`
- `ucrtbased.dll`

Development は D なしの Retail Runtime を参照する。
配布 Editor / PlayHost は Development または Release を使い、必要に応じて自作 PDB を付ける。
自作 SDK の Debug 用 Library / PDB と、Microsoft Debug CRT の再配布を混同しない。
Microsoft の SDK / CRT の `.lib` そのものを CueEngine SDK の Library として収集しない。

公式の [Debug 実行 File の Test 用配備](https://learn.microsoft.com/en-us/cpp/windows/preparing-a-test-machine-to-run-a-debug-executable?view=msvc-170)
は、Debug Runtime の配備を自分の別 PC での Debug / Test に限定して説明している。
[Visual Studio 2026 の再配布一覧](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)
は `debug_nonredist` を通常の再配布対象から除外する。
Microsoft 自身が Preview とした Component の再配布制限と、CueEngine の Preview 公開は別の条件である。

## 変更前の DXC

従来の CMake は Windows SDK の最新 `bin/<version>/x64` を選び、DLL を Copy していた。
確認した実物は次のとおり。

| File | Product Version | SHA-256 |
| --- | --- | --- |
| SDK `bin/10.0.26100.0/x64/dxcompiler.dll` | `1.9.2602.17 (21d28f727)` | `495033935c058fc6d318ac257afdfa910795bf41b7e5ba905a0e7191dc8bd4fa` |
| SDK `bin/10.0.26100.0/x64/dxil.dll` | `1.9.2602.17 (21d28f727)` | `db108ffef7bcecb984bdccf2b4725719d65b2ce7f1ebc40019dbe6c5a7016e09` |

[Windows SDK REDIST.TXT](https://learn.microsoft.com/en-us/legal/windows-sdk/redist)
は `Redist/D3D/x64/dxcompiler.dll` と `dxil.dll` を Classic Windows Application 向けに列挙している。
この PC の Redist は `dxcompiler.dll` が `1.9.2602.17`、`dxil.dll` が `101.7.2308.24` であり、
上記 bin の組合せと異なる。Copy 元だけを Redist へ変更すると Validator の Version も変わるため、
互換性を確認せず差し替えない。

## 固定する公式 DXC Package

公式 [DXC v1.9.2609](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609)
は 2026-09-29 公開の通常 Release であり、Windows Package を固定して利用する。

- Archive: `dxc_2026_09_29.zip`
- URL: `https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip`
- SHA-256: `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`
- GitHub Asset ID: `596913261`
- Native CMake では NuGet Restore を追加しない

`dxcompiler.dll` と `dxil.dll` を同じ Package から利用し、実物 Package の License と Notice を保持する。
Archive の SHA-256 は実際の Download 結果と GitHub Asset の `digest` の両方で照合した。
実行 File 横と Player Package へ Copy する保持 File は次のとおり。

| Archive 内の File | 保持する File 名 |
| --- | --- |
| `LICENSE-MS.txt` | `DirectXShaderCompiler-MS.txt` |
| `LICENSE-LLVM.txt` | `DirectXShaderCompiler-LLVM.txt` |
| `LICENCE-MIT.txt` | `DirectXShaderCompiler-MIT.txt` |
| `inc/hlsl/LICENCE.txt` | `DirectXShaderCompiler-HLSL.txt` |

上記は Archive の原文 Byte 列を `ThirdParty/Licenses` にも保持する。
同梱 HLSL Header は Apache-2.0 with LLVM Exceptions であり、その条件も省かない。
公式 [Release Notes の License 表](https://github.com/microsoft/DirectXShaderCompiler/blob/release-1.9.2609/docs/ReleaseNotes.md)
は `d3d12shader.h` と他 File の License を区別する。Archive に含まれる Microsoft License も保持し、
古い SDK の `dxil.dll` と同じ License 割当と決めつけない。
LLVM 系の Binary 再配布条件には Copyright / 条件 / 免責の保持と、名前による無断の推奨表示禁止がある。
Package 内の Microsoft License も含めて配布し、DXC を一律 MIT と説明しない。

Source-backed Engine Package の `ThirdParty/Packages/DXC-1.9.2609.zip` を優先して検証し、
利用者の Build Tree に展開する。同梱 Archive がある場合は Network を要求しない。
通常の開発 Checkout では Build Tree の第三者 Cache に取得する。Archive と実際に使う DLL / License の
SHA-256 は再 Configure 時にも検証する。
SDK の `dxcapi.h` は引き続きコンパイル時の Windows SDK から利用できる。

## .NET 10 WPF Hub

[Windows 向け .NET License 情報](https://github.com/dotnet/core/blob/main/license-information-windows.md)
は次の区分を説明している。

- `coreclr.dll`、`PresentationNative_cor3.dll`、`wpfgfx_cor3.dll`、`vcruntime140_cor3.dll` 等は .NET Library License
- WPF の `D3DCompiler_47_cor3.dll` は Windows SDK License
- その他の Binary / File は原則 MIT。実際の第三者 Notice は別途保持する

[.NET Library License](https://dotnet.microsoft.com/en-us/dotnet_library_license.htm)
はアプリに組み込む Object Code の配布を許可するが、単独配布、Notice の保持、下流の利用条件等を定める。
Hub の self-contained 出力は、使用した Runtime Pack に対応する License / ThirdPartyNotices を同梱する。
自作 Hub の License は Microsoft の条件を上書きしない。

監査時の導入 SDK は `10.0.403`、WindowsDesktop Runtime は `10.0.14`。
`C:/Program Files/dotnet/LICENSE.txt` と `ThirdPartyNotices.txt` の存在を確認した。
最終的な根拠は publish が選択する Runtime Pack とし、開発 PC の SDK 全体を配布しない。

## Toolchain の境界

[CMake の公式 License 説明](https://cmake.org/licensing/)は BSD 3-Clause と内包第三者の License を案内する。
Windows SDK 実物の `Licenses/10.0.26100.0/sdk_license.rtf` は REDIST 一覧を配布対象の根拠とし、
SDK の `.lib` をプログラムとの Link 結果として配布する条件を持つ。
MSBuild の OSS License と Visual Studio のインストール物全体の配布条件を混同しない。
Engine SDK は CueEngine 自身の Header / Library / PDB / Build 定義を提供し、
MSVC と Windows SDK は導入検出と公式導入先への案内を行う。

## 検証範囲と残作業

実物 File Version、Hash、Editor の依存 DLL、SDK License、公式配布条件を確認した。
DXC Module 単独では、同梱 Archive だけを使う Offline 展開、破損 Archive の拒否、展開後の DLL 改変の拒否を確認した。
固定 Package の x64 `dxcompiler.dll` / `dxil.dll` は両方 `1.9.2609.5 (01b62ad47)`。
固定 Package の `dxc.exe` で `StaticMesh.hlsl` と `FullscreenTriangle.hlsl` の
`vs_main` / `ps_main` を Shader Model 6.6 / HLSL 2021 で Compile し、4 件とも成功した。
Module の Fixture は `out/validation/DxcModule-0d2ef5d83e694f2c922f89335ef8dae3`、
生成 DXIL は `out/validation/Dxc-1.9.2609-*.dxil` に保持した。
FrameGraph、Scene 描画、ImGui、移動先での起動は親側の集中 Build / Test で再検証する。
最終 Engine / Hub 配布物の依存閉包・License 同梱・Retail Runtime を検証する Gate が別途必要である。
独立した法的見解や、未確認の第三者 Package まで配布可能と判定した記録ではない。
