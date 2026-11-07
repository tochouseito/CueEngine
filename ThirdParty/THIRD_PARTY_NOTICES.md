# Third-party notices

## DirectXShaderCompiler

- 出自: https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609
- Version: `1.9.2609`（Windows Archive `dxc_2026_09_29.zip`）
- Archive SHA-256: `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`
- License: 公式 Archive の MS / LLVM / MIT / HLSL License を保持する。全体を MIT とみなさない
- 原文: `Licenses/DirectXShaderCompiler-MS.txt`、`DirectXShaderCompiler-LLVM.txt`、
  `DirectXShaderCompiler-MIT.txt`、`DirectXShaderCompiler-HLSL.txt`

公式 Archive の対 DLL と対応 Notice を Hash 検証して利用する。Windows SDK の任意の bin を再配布元にしない。
Engine / Editor Package と DXC を含む実行物に、対応する License / Notice を同梱する。
本体・SDK・Debug CRT の配布条件は [Windows 再配布監査](../Docs/Research/Hub-Windows-Redistribution-Audit.md) を参照。

## WinPixEventRuntime

- 出自: https://github.com/microsoft/PixEvents
- 公式 Package: https://www.nuget.org/packages/WinPixEventRuntime/1.0.240308001
- Version: `1.0.240308001`
- Archive SHA-256: `726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4`
- License: MIT、全文は [Licenses/WinPixEventRuntime-MIT.txt](Licenses/WinPixEventRuntime-MIT.txt)
- 同梱 Notice: [Licenses/WinPixEventRuntime-ThirdPartyNotices.txt](Licenses/WinPixEventRuntime-ThirdPartyNotices.txt)

公式 Archive を CMake で Hash 検証して展開し、Header と x64 DLL／Import Library を変更せず利用する。NuGet restore は実行しない。Debug／Development の FrameGraph Pass の PIX Marker に使用し、Release では Marker を無効にする。Binary 配布時は DLL と上記 License／Notice を同梱する。

## Dear ImGui

- 出自: https://github.com/ocornut/imgui
- 上流 Tag: `v1.92.9b-docking`
- vcpkg Port: `imgui` Version `1.92.9`
- Registry baseline: `19780d9cdf84d0944cf9a318666703b89ab6629c`
- Feature: `docking-experimental`、`win32-binding`、`dx12-binding`
- License: MIT、全文は [Licenses/ImGui-MIT.txt](Licenses/ImGui-MIT.txt)
- 対象: Core、Demo、公式 Win32／DirectX 12 Backend、同梱 Font／補助実装

第三者 Source は変更せず、vcpkg の公式 Port を利用する。Dear ImGui を含む Editor Binary または Source を再配布する場合は、上記 License の Copyright と許諾文を同梱する。Source 内の補助実装に付属する Notice も保持する。

### 埋め込み Font と補助実装

`v1.92.9b-docking` の `imgui_draw.cpp` / `docs/FONTS.md` を照合した。現在は `AddFontDefault` を使用し、既定 Font の除外定義を設定していないため、Editor 配布物に次の Notice も含める。

| 対象 | License / 帰属 | 保持する File |
| --- | --- | --- |
| ProggyClean | MIT、Copyright (c) 2004, 2005 Tristan Grimmer | [Licenses/ProggyClean-MIT.txt](Licenses/ProggyClean-MIT.txt) |
| ProggyForever | MIT、Copyright (c) 2026 Disco Hello / Copyright (c) 2019,2023 Tristan Grimmer | [Licenses/ProggyForever-MIT.txt](Licenses/ProggyForever-MIT.txt) |
| `imstb_rectpack.h` / `imstb_textedit.h` / `imstb_truetype.h` | MIT または Public Domain から MIT を選択、Copyright (c) 2017 Sean Barrett | [Licenses/ImGui-STB-Licenses.txt](Licenses/ImGui-STB-Licenses.txt) |
| 日本語 Glyph Range の Code Point 集合 | 上流が示す文化庁・法務省由来データ、CC BY 4.0 | [Licenses/ImGui-JapaneseGlyphRanges-Notice.txt](Licenses/ImGui-JapaneseGlyphRanges-Notice.txt) |

Font の帰属表記は固定 ImGui Tag に一致する。MIT 全文は ProggyClean の上流 Commit `139ec08a38096161291792313ef5803fc4f0e37b` の `LICENSE`、ProggyForever の上流 Commit `f8868ba43bb42c4d88454d03ad81cbd139789f72` の `LICENSE.txt` から保持した。stb は固定 ImGui Tag に含まれる 3 Header の共通 License Block を保持した。

`misc/fonts/` の追加 Font や第三者の ImGui 拡張を将来配布する場合は、上表の範囲に含めず個別に License を確認する。

## vcpkg

- 出自: https://github.com/microsoft/vcpkg
- 取得に利用した Tool: `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`（Visual Studio 同梱）
- Registry の Build 補助 Port: `vcpkg-cmake`（`2025-08-07`）、`vcpkg-cmake-config`（`2026-07-21`）。Manifest baseline で固定
- License: MIT、全文は [Licenses/vcpkg-MIT.txt](Licenses/vcpkg-MIT.txt)

`vcpkg-cmake-config` に付属する Notice は [Licenses/VcpkgCMakeConfig-MIT.txt](Licenses/VcpkgCMakeConfig-MIT.txt) に保持する。

vcpkg Tool と Build 補助 Port は開発時に利用し、Editor Binary には含めない。これらの Source／Script を再配布する場合は Copyright と許諾文を保持する。

## First-party 移植 Source

`Cue.ECS` は同じ所有者の旧 `CueEngineLegacy` の自作実装であり、第三者 ECS Library ではない。出典 Commit は `8476b656c24e4ef6262761fefed9b37566932306`、元の MIT License は [../Engine/Source/ECS/LICENSE.txt](../Engine/Source/ECS/LICENSE.txt) に保持する。詳細は [ECS 移植記録](../Docs/Research/M11-Legacy-ECS-Migration.md) を参照する。

元の Root と ECS の License は Copyright が `[year] [fullname]` だったが、2026-11-04 のユーザー明示指定により `Copyright (c) 2026 tochouseito` へ更新した。MIT の許諾本文は変更していない。第三者の Copyright は本表記で置き換えない。
